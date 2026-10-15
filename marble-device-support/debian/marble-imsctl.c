// SPDX-License-Identifier: MIT
/*
 * marble-imsctl - enable IMS services (VoLTE / SMS over IMS) on the SM7475 modem
 *
 * The modem firmware (MPSS, "WAIPIO_GEN_PACK" in the marble case) keeps its IMS
 * stack disabled until the application processor confirms which IMS services may
 * run. Android does that from its IMS stack (ims.apk -> QMI IMSS); on mainline
 * nobody does, so the modem never even tries to register: IMSA reports
 * "not registered" with error code 0 and WMS keeps routing SMS through the CS
 * domain, which China Telecom rejects (reject cause 18 / cs-domain-not-available).
 *
 * Applying the "Set IMS Services Enabled Setting" message once (it is stored in
 * the modem's NV) makes the modem register IMS as soon as the IMS PDN is up:
 * 81voltd serves the IMS Data service, ModemManager brings the "IMS" APN bearer
 * up, and the modem then registers (IMSA: REGISTERED, sms/voice/video "ready").
 * SMS then flows over IMS and CS-less operators work without any MM change.
 *
 * Note that the IMSA client must be bound (Bind, binding 0) before the modem
 * answers registration queries: without the bind every request fails with
 * QMI error 70 'InvalidOperation'.
 */

#include <glib.h>
#include <gio/gio.h>
#include <libqrtr-glib.h>
#include <libqmi-glib.h>
#include <stdio.h>
#include <string.h>

enum mode {
	MODE_STATUS,
	MODE_APPLY,
	MODE_CHECK,
};

static GMainLoop *loop;
static QrtrBus *bus;
static QmiDevice *device;
static QmiClientImsa *imsa;
static QmiClientIms *imss;
static enum mode mode = MODE_STATUS;
static int exit_code;
static guint apply_poll_left;

static void quit(void)
{
	g_main_loop_quit(loop);
}

static void fail(const char *where, GError *e)
{
	g_printerr("%s: %s\n", where, e ? e->message : "unknown error");
	if (e)
		g_error_free(e);
	exit_code = 1;
	quit();
}

static void step_imss_get(void);
static void step_imsa_status(void);
static gboolean poll_imsa_status(gpointer user_data G_GNUC_UNUSED);

#define PBOOL(fn, out, name) do { \
		gboolean b; \
		if (fn(out, &b, NULL)) \
			printf("  %-24s %s\n", name, b ? "yes" : "no"); \
	} while (0)

static const char *service_status_str(QmiImsaServiceStatus st)
{
	if (st == QMI_IMSA_SERVICE_AVAILABLE)
		return "ready";
	if (st == QMI_IMSA_SERVICE_LIMITED)
		return "limited";
	return "unavailable";
}

static void imss_get_ready(GObject *src, GAsyncResult *res, gpointer u G_GNUC_UNUSED)
{
	QmiMessageImsGetImsServicesEnabledSettingOutput *out;
	GError *e = NULL;

	out = qmi_client_ims_get_ims_services_enabled_setting_finish(QMI_CLIENT_IMS(src), res, &e);
	if (!out) {
		fail("couldn't get IMS services enabled setting", e);
		return;
	}

	printf("IMSS enabled setting:\n");
	PBOOL(qmi_message_ims_get_ims_services_enabled_setting_output_get_ims_registration_service_enabled, out, "registration_service");
	PBOOL(qmi_message_ims_get_ims_services_enabled_setting_output_get_ims_voice_service_enabled, out, "voice_service");
	PBOOL(qmi_message_ims_get_ims_services_enabled_setting_output_get_ims_sms_service_enabled, out, "sms_service");
	PBOOL(qmi_message_ims_get_ims_services_enabled_setting_output_get_ims_video_telephony_service_enabled, out, "video_telephony");
	PBOOL(qmi_message_ims_get_ims_services_enabled_setting_output_get_ims_voice_wifi_service_enabled, out, "voice_wifi");
	PBOOL(qmi_message_ims_get_ims_services_enabled_setting_output_get_ims_ut_service_enabled, out, "ut_service");
	PBOOL(qmi_message_ims_get_ims_services_enabled_setting_output_get_ims_ussd_service_enabled, out, "ussd_service");
	qmi_message_ims_get_ims_services_enabled_setting_output_unref(out);
	quit();
}

static void step_imss_get(void)
{
	qmi_client_ims_get_ims_services_enabled_setting(imss, NULL, 15, NULL, imss_get_ready, NULL);
}

static void imsa_services_ready(GObject *src, GAsyncResult *res, gpointer u G_GNUC_UNUSED)
{
	QmiMessageImsaGetImsServicesStatusOutput *out;
	GError *e = NULL;
	QmiImsaServiceStatus st;

	out = qmi_client_imsa_get_ims_services_status_finish(QMI_CLIENT_IMSA(src), res, &e);
	if (!out) {
		fail("couldn't get IMS services status", e);
		return;
	}

	printf("IMS services status:\n");
	if (qmi_message_imsa_get_ims_services_status_output_get_ims_sms_service_status(out, &st, NULL))
		printf("  %-24s %s\n", "sms", service_status_str(st));
	if (qmi_message_imsa_get_ims_services_status_output_get_ims_voice_service_status(out, &st, NULL))
		printf("  %-24s %s\n", "voice", service_status_str(st));
	if (qmi_message_imsa_get_ims_services_status_output_get_ims_video_telephony_service_status(out, &st, NULL))
		printf("  %-24s %s\n", "video", service_status_str(st));
	qmi_message_imsa_get_ims_services_status_output_unref(out);
	step_imss_get();
}

/*
 * 'apply' is run by the boot unit right after the modem announces its QMI
 * services, but the modem only registers once the IMS PDN is up - the modem
 * asks 81voltd for it, ModemManager creates the bearer, and registration
 * follows tens of seconds later. Poll for a while instead of exiting with
 * "not registered" (and failing the unit) on the first look.
 */
static gboolean poll_imsa_status(gpointer user_data G_GNUC_UNUSED)
{
	step_imsa_status();
	return G_SOURCE_REMOVE;
}

static void imsa_reg_ready(GObject *src, GAsyncResult *res, gpointer u G_GNUC_UNUSED)
{
	QmiMessageImsaGetImsRegistrationStatusOutput *out;
	QmiImsaImsRegistrationStatus st;
	GError *e = NULL;
	guint16 ec;
	const gchar *em = NULL;
	guint32 tech = 0;

	out = qmi_client_imsa_get_ims_registration_status_finish(QMI_CLIENT_IMSA(src), res, &e);
	if (!out) {
		fail("couldn't get IMS registration status", e);
		return;
	}

	if (!qmi_message_imsa_get_ims_registration_status_output_get_result(out, &e)) {
		g_printerr("IMS registration status: error: %s\n", e ? e->message : "unknown");
		if (e)
			g_error_free(e);
		qmi_message_imsa_get_ims_registration_status_output_unref(out);
		exit_code = 1;
		quit();
		return;
	}

	if (qmi_message_imsa_get_ims_registration_status_output_get_ims_registration_status(out, &st, NULL)) {
		const char *s;

		switch (st) {
		case QMI_IMSA_IMS_REGISTERED:
			s = "registered";
			break;
		case QMI_IMSA_IMS_REGISTERING:
			s = "registering";
			break;
		case QMI_IMSA_IMS_LIMITED_REGISTERED:
			s = "limited";
			break;
		default:
			s = "not registered";
			break;
		}

		if (st != QMI_IMSA_IMS_REGISTERED && mode == MODE_APPLY && apply_poll_left > 0) {
			apply_poll_left--;
			printf("IMS registration status: %s, retrying (%u left)\n", s, apply_poll_left);
			fflush(stdout);
			qmi_message_imsa_get_ims_registration_status_output_unref(out);
			g_timeout_add_seconds(3, poll_imsa_status, NULL);
			return;
		}

		printf("IMS registration status: %s\n", s);
		if (st != QMI_IMSA_IMS_REGISTERED)
			exit_code = 2;
	}

	/* The modem reports why registration failed here, which is the only
	 * visible diagnostic when the IMS config is rejected. */
	if (qmi_message_imsa_get_ims_registration_status_output_get_ims_registration_error_code(out, &ec, NULL))
		printf("  %-24s %u\n", "error code", ec);
	if (qmi_message_imsa_get_ims_registration_status_output_get_ims_registration_error_message(out, &em, NULL))
		printf("  %-24s %s\n", "error message", em);
	if (qmi_message_imsa_get_ims_registration_status_output_get_ims_registration_technology(out, &tech, NULL))
		printf("  %-24s %u\n", "tech", tech);

	qmi_message_imsa_get_ims_registration_status_output_unref(out);

	if (mode == MODE_CHECK)
		quit();
	else
		qmi_client_imsa_get_ims_services_status(imsa, NULL, 15, NULL, imsa_services_ready, NULL);
}

static void step_imsa_status(void)
{
	qmi_client_imsa_get_ims_registration_status(imsa, NULL, 15, NULL, imsa_reg_ready, NULL);
}

static void imss_set_ready(GObject *src, GAsyncResult *res, gpointer u G_GNUC_UNUSED)
{
	QmiMessageImsSetImsServicesEnabledSettingOutput *out;
	GError *e = NULL;

	out = qmi_client_ims_set_ims_services_enabled_setting_finish(QMI_CLIENT_IMS(src), res, &e);
	if (!out) {
		fail("couldn't set IMS services enabled setting", e);
		return;
	}
	if (!qmi_message_ims_set_ims_services_enabled_setting_output_get_result(out, &e)) {
		g_printerr("set IMS services enabled setting failed: %s\n", e ? e->message : "unknown");
		if (e)
			g_error_free(e);
		exit_code = 1;
		qmi_message_ims_set_ims_services_enabled_setting_output_unref(out);
		quit();
		return;
	}
	qmi_message_ims_set_ims_services_enabled_setting_output_unref(out);
	printf("IMS services enabled setting: applied\n");
	step_imsa_status();
}

static void imss_apply(void)
{
	QmiMessageImsSetImsServicesEnabledSettingInput *in;
	GError *e = NULL;

	/* ~90 s of slack for the modem to come up and register after the config */
	apply_poll_left = 30;

	in = qmi_message_ims_set_ims_services_enabled_setting_input_new();
#define SETF(fn, val, label) do { \
		if (!fn(in, val, &e)) { \
			g_printerr("  %s: could not be set: %s\n", label, e ? e->message : "unknown"); \
			if (e) \
				g_error_free(e); \
			e = NULL; \
		} \
	} while (0)
	SETF(qmi_message_ims_set_ims_services_enabled_setting_input_set_ims_voice_over_lte_enable, TRUE, "voLTE");
	SETF(qmi_message_ims_set_ims_services_enabled_setting_input_set_ims_video_telephony_service_enable, TRUE, "video telephony");
	SETF(qmi_message_ims_set_ims_services_enabled_setting_input_set_ims_service_enabled, TRUE, "registration service");
	SETF(qmi_message_ims_set_ims_services_enabled_setting_input_set_ims_ut_service_enable, TRUE, "UT");
	SETF(qmi_message_ims_set_ims_services_enabled_setting_input_set_ims_sms_service_enable, TRUE, "SMS over IMS");
	SETF(qmi_message_ims_set_ims_services_enabled_setting_input_set_ims_ussd_service_enabled, TRUE, "USSD");
	SETF(qmi_message_ims_set_ims_services_enabled_setting_input_set_ims_presence_enabled, TRUE, "presence");
	SETF(qmi_message_ims_set_ims_services_enabled_setting_input_set_ims_rcs_enabled, TRUE, "RCS");
	SETF(qmi_message_ims_set_ims_services_enabled_setting_input_set_ims_xdm_client_enabled, TRUE, "XDM");
	SETF(qmi_message_ims_set_ims_services_enabled_setting_input_set_ims_autoconfig_enabled, FALSE, "autoconfig");
	/* The modem keeps 2G/3G disabled on marble and China Telecom has no CS
	 * domain at all, so IMS is the only call mode that can work. */
	SETF(qmi_message_ims_set_ims_services_enabled_setting_input_set_ims_call_mode_preference,
	     QMI_IMS_CALL_MODE_PREFERENCE_CELLULAR, "call mode preference");

	qmi_client_ims_set_ims_services_enabled_setting(imss, in, 15, NULL, imss_set_ready, NULL);
	qmi_message_ims_set_ims_services_enabled_setting_input_unref(in);
}

static void imss_bind_ready(GObject *src, GAsyncResult *res, gpointer u G_GNUC_UNUSED)
{
	QmiMessageImsBindOutput *out;
	GError *e = NULL;

	out = qmi_client_ims_bind_finish(QMI_CLIENT_IMS(src), res, &e);
	if (!out) {
		fail("couldn't bind to the IMS service", e);
		return;
	}
	if (!qmi_message_ims_bind_output_get_result(out, &e)) {
		g_printerr("IMS bind failed: %s\n", e ? e->message : "unknown");
		if (e)
			g_error_free(e);
		exit_code = 1;
		qmi_message_ims_bind_output_unref(out);
		quit();
		return;
	}
	qmi_message_ims_bind_output_unref(out);

	if (mode == MODE_APPLY)
		imss_apply();
	else
		step_imsa_status();
}

static void imss_ready(GObject *src G_GNUC_UNUSED, GAsyncResult *res, gpointer u G_GNUC_UNUSED)
{
	QmiMessageImsBindInput *in;
	GError *e = NULL;
	QmiClient *client;

	client = qmi_device_allocate_client_finish(device, res, &e);
	if (!client) {
		fail("couldn't allocate an IMS client", e);
		return;
	}
	imss = QMI_CLIENT_IMS(client);
	in = qmi_message_ims_bind_input_new();
	qmi_message_ims_bind_input_set_binding(in, 0, NULL);
	qmi_client_ims_bind(imss, in, 15, NULL, imss_bind_ready, NULL);
	qmi_message_ims_bind_input_unref(in);
}

static void imsa_bind_ready(GObject *src, GAsyncResult *res, gpointer u G_GNUC_UNUSED)
{
	QmiMessageImsaBindOutput *out;
	GError *e = NULL;

	out = qmi_client_imsa_bind_finish(QMI_CLIENT_IMSA(src), res, &e);
	if (!out) {
		fail("couldn't bind to IMSA", e);
		return;
	}
	if (!qmi_message_imsa_bind_output_get_result(out, &e)) {
		g_printerr("IMSA bind failed: %s\n", e ? e->message : "unknown");
		if (e)
			g_error_free(e);
		exit_code = 1;
		qmi_message_imsa_bind_output_unref(out);
		quit();
		return;
	}
	qmi_message_imsa_bind_output_unref(out);

	if (mode == MODE_CHECK)
		step_imsa_status();
	else
		qmi_device_allocate_client(device, QMI_SERVICE_IMS, QMI_CID_NONE, 15, NULL, imss_ready, NULL);
}

static void imsa_ready(GObject *src G_GNUC_UNUSED, GAsyncResult *res, gpointer u G_GNUC_UNUSED)
{
	QmiMessageImsaBindInput *in;
	GError *e = NULL;
	QmiClient *client;

	client = qmi_device_allocate_client_finish(device, res, &e);
	if (!client) {
		fail("couldn't allocate an IMSA client", e);
		return;
	}
	imsa = QMI_CLIENT_IMSA(client);
	in = qmi_message_imsa_bind_input_new();
	qmi_message_imsa_bind_input_set_binding(in, 0, NULL);
	qmi_client_imsa_bind(imsa, in, 15, NULL, imsa_bind_ready, NULL);
	qmi_message_imsa_bind_input_unref(in);
}

static void device_open_ready(GObject *src G_GNUC_UNUSED, GAsyncResult *res, gpointer u G_GNUC_UNUSED)
{
	GError *e = NULL;

	if (!qmi_device_open_finish(device, res, &e)) {
		fail("couldn't open the QMI device", e);
		return;
	}
	qmi_device_allocate_client(device, QMI_SERVICE_IMSA, QMI_CID_NONE, 15, NULL, imsa_ready, NULL);
}

static void device_ready(GObject *src G_GNUC_UNUSED, GAsyncResult *res, gpointer u G_GNUC_UNUSED)
{
	GError *e = NULL;

	device = qmi_device_new_from_node_finish(res, &e);
	if (!device) {
		fail("couldn't create the QMI device", e);
		return;
	}
	qmi_device_open(device, QMI_DEVICE_OPEN_FLAGS_NONE, 15, NULL, device_open_ready, NULL);
}

static void bus_ready(GObject *src G_GNUC_UNUSED, GAsyncResult *res, gpointer u G_GNUC_UNUSED)
{
	QrtrNode *node;
	GError *e = NULL;

	bus = qrtr_bus_new_finish(res, &e);
	if (!bus) {
		fail("couldn't get the QRTR bus", e);
		return;
	}
	/* The modem is node 0 on this SoC (MPSS runs locally and talks over glink). */
	node = qrtr_bus_peek_node(bus, 0);
	if (!node) {
		g_printerr("QRTR node 0 (modem) not found\n");
		exit_code = 1;
		quit();
		return;
	}
	qmi_device_new_from_node(node, NULL, device_ready, NULL);
}

int main(int argc, char **argv)
{
	int i;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "apply"))
			mode = MODE_APPLY;
		else if (!strcmp(argv[i], "check"))
			mode = MODE_CHECK;
		else if (!strcmp(argv[i], "status"))
			mode = MODE_STATUS;
		else {
			g_printerr("usage: %s [status|apply|check]\n", argv[0]);
			return 1;
		}
	}

	loop = g_main_loop_new(NULL, FALSE);
	qrtr_bus_new(1000, NULL, bus_ready, NULL);
	g_main_loop_run(loop);

	return exit_code;
}

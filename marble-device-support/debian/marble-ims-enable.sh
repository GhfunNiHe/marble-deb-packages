#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Hand the "IMS services enabled" configuration to the modem.
#
# The modem firmware does not register for IMS on its own: it waits for the
# application processor to state which IMS services may run (Android does that
# from ims.apk over QMI IMSS). Without it IMSA reports "not registered, error 0"
# and the modem keeps sending SMS over the CS domain, which operators without
# 2G/3G fallback (e.g. China Telecom) reject.
#
# 81voltd provides the IMS Data service, ModemManager brings the "IMS" APN bearer
# up; once the services are enabled the modem does the SIP registration itself.
set -eu

CTL=/usr/libexec/marble-imsctl

# MPSS needs tens of seconds to boot and announce its QMI services, and the QRTR
# lookup only lists them once the name service knows the node.
i=0
while [ "$i" -lt 90 ]; do
	if qrtr-lookup 2>/dev/null | grep -q 'IMS application service'; then
		break
	fi
	i=$((i + 1))
	sleep 2
done

# Idempotent: the modem stores the setting in NV, re-applying it is harmless.
exec "$CTL" apply

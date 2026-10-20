#!/bin/sh
# marble: qcom-phone-utils' qcom-modem-setup waits only one second for the SIM
# (sim_wait_time=1, deliberately short so a phone without a card does not stall
# boot). This modem's UIM takes closer to ten seconds to report
# "Card state: 'present'", so the stock script gives up with
# "E: No sim detected after 1 seconds." and never activates the primary GW
# provisioning session. ModemManager then fails with
#
#   modem couldn't be initialized: Couldn't check unlock status:
#   QMI operation failed: GW primary session index unknown
#
# and settles on state=failed (sim-missing). A failed modem never reports
# "unlock-required: sim-pin", so the shell has nothing to prompt for and the
# user never sees a PIN dialog - the phone comes up without registration.
#
# Instead of patching the Debian package, re-run the stock script with a longer
# budget. The patched copy lives in /run, so it keeps tracking whatever version
# of qcom-phone-utils is installed.
set -eu

src=/usr/bin/qcom-modem-setup
dst=/run/marble-qcom-modem-setup.sh

sed 's/^sim_wait_time=1$/sim_wait_time=30/' "$src" >"$dst"
chmod 700 "$dst"

if ! grep -q '^sim_wait_time=30$' "$dst"; then
	echo "W: sim_wait_time not found in $src, running it unmodified" >&2
fi

exec /bin/sh "$dst"

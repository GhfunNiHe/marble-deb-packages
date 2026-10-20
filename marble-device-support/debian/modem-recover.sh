#!/bin/sh
# marble: last-resort recovery for the modem initialisation race.
#
# marble-modem-setup raises qcom-modem-setup's SIM wait to 30 s, which covers a
# cold boot. A modem that still ends up in state=failed/sim-missing (SLPI
# restart, card reseated, a boot that lost the race anyway) stays there
# forever: ModemManager does not retry on its own, so the phone keeps running
# with no PIN prompt, no registration and no SMS until someone restarts the
# stack by hand.
#
# Run by marble-modem-recover.timer shortly after boot; restart the stack once
# if - and only if - the modem reports that exact failure.
set -u

log() { logger -t marble-modem-recover "$*"; }

modem_field() {
	# $1 = key below modem.generic.
	mmcli -m any --output-keyvalue 2>/dev/null |
		sed -n "s/^modem\.generic\.$1 *: //p" | head -n1
}

i=0
while [ "$i" -lt 10 ]; do
	i=$((i + 1))
	state=$(modem_field state)
	case "$state" in
	registered | enabled | locked | connecting | connected | initializing | enabling)
		log "modem state '$state', nothing to do"
		exit 0
		;;
	failed)
		reason=$(modem_field state-failed-reason)
		if [ "$reason" = "sim-missing" ]; then
			log "modem stuck in failed/sim-missing, restarting qcom-modem-setup + ModemManager"
			systemctl restart qcom-modem-setup.service || true
			systemctl restart ModemManager.service || true
		else
			log "modem failed with reason '$reason', leaving it alone"
		fi
		exit 0
		;;
	esac
	sleep 3
done

log "no usable modem state after $i polls, giving up"

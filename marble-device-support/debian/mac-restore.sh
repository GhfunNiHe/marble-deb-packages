#!/bin/sh
# marble: restore the factory WLAN/BT MACs from /persist/qca6490/wlan_mac.bin.
#
# WLAN: copy the file into the ath11k firmware dir before modprobe ath11k_pci
# (blacklisted so udev cannot load it before the file exists).
# Bluetooth: the stock NV blob has no BD_ADDR, so the controller reports
# all-zero and stays out of the mgmt index; push WLAN MAC minus one via btmgmt.
# A Bluetooth failure must not take the unit down: WLAN is the primary link.
set -eu

DST=/lib/firmware/ath11k/WCN6855/hw2.1/wlan_mac.bin
PERSIST_SRC=/persist/qca6490/wlan_mac.bin
MNT=/run/marble-persist
src=

if [ -f "$PERSIST_SRC" ]; then
	src=$PERSIST_SRC
else
	mkdir -p "$MNT"
	if ! mountpoint -q "$MNT"; then
		# /dev/disk/by-partlabel/persist is created by udev and may still be
		# missing this early; ask the kernel by PARTLABEL and retry while the
		# UFS device probes. A bad mount here leaves WLAN down (ath11k_pci is
		# blacklisted), so fail loudly.
		persist_dev=$(findfs PARTLABEL=persist 2>/dev/null || true)
		i=0
		while [ -z "$persist_dev" ] && [ $i -lt 20 ]; do
			sleep 0.5
			persist_dev=$(findfs PARTLABEL=persist 2>/dev/null || true)
			i=$((i + 1))
		done
		if [ -z "$persist_dev" ]; then
			echo "persist partition never appeared; WLAN MAC cannot be restored" >&2
			exit 1
		fi
		mount -o ro,nosuid,nodev "$persist_dev" "$MNT"
	fi
	src="$MNT/qca6490/wlan_mac.bin"
fi

if [ ! -f "$src" ]; then
	# No factory file: do not modprobe ath11k_pci, a random MAC silently
	# breaks DHCP reservations; leave WLAN down and visible.
	echo "no wlan_mac.bin under persist; leaving WLAN down instead of using a random MAC" >&2
	if [ "$src" != "$PERSIST_SRC" ]; then
		umount "$MNT" 2>/dev/null || true
		rmdir "$MNT" 2>/dev/null || true
	fi
	exit 1
fi

wlan=$(sed -n 's/^wlan0=//p' "$src" | tr -d '\r\n' | tail -c 12)

# Copy unconditionally: a stale file from an earlier boot would otherwise
# pin the wrong address forever.
install -Dm644 "$src" "$DST"

if [ "$src" != "$PERSIST_SRC" ]; then
	umount "$MNT" 2>/dev/null || true
	rmdir "$MNT" 2>/dev/null || true
fi

modprobe ath11k_pci

case $wlan in
	'' | *[!0-9A-Fa-f]*)
		echo "cannot parse a WLAN MAC out of persist; leaving Bluetooth unconfigured"
		exit 0
		;;
esac
if [ ${#wlan} -ne 12 ]; then
	echo "WLAN MAC '$wlan' is not 12 hex digits; leaving Bluetooth unconfigured"
	exit 0
fi

wlan=$(echo "$wlan" | tr 'a-f' 'A-F')
last=$((0x${wlan#??????????} - 1))
[ $last -ge 0 ] || last=255
addr=$(printf '%s%02X' "${wlan%??}" $last | sed 's/../&:/g;s/:$//')

i=0
while [ ! -d /sys/class/bluetooth/hci0 ] && [ $i -lt 30 ]; do
	sleep 1
	i=$((i + 1))
done
if [ ! -d /sys/class/bluetooth/hci0 ]; then
	echo "no hci0 after ${i}s; leaving Bluetooth unconfigured"
	exit 0
fi

# Reprogramming needs the controller powered down, but bluetoothd powers it up
# on appearance, so retry a few times.
i=0
while [ $i -lt 5 ]; do
	btmgmt --index 0 power off >/dev/null 2>&1 || true
	if btmgmt --index 0 public-addr "$addr" >/dev/null 2>&1; then
		echo "hci0 public address set to $addr"
		# AutoEnable only fires on index add; power back on ourselves since the
		# address change happened afterwards.
		btmgmt --index 0 power on >/dev/null 2>&1 || true
		# bluetoothd initialised mgmt while hci0 still had no address, so
		# restart it to re-enumerate the now-configured controller.
		# --no-block avoids waiting on this unit (After=bluetooth.service).
		systemctl --no-block restart bluetooth.service </dev/null >/dev/null 2>&1 || true
		exit 0
	fi
	sleep 2
	i=$((i + 1))
done

echo "could not set the hci0 public address to $addr"
exit 0

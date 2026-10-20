#!/bin/sh
# marble: restore the factory WLAN and Bluetooth MACs from the persist partition.
#
# Both radios on this board are programmed from one file on the Android persist
# partition, /persist/qca6490/wlan_mac.bin, an 18-byte ASCII "wlan0=<12 hex
# digits>". Neither the ath11k firmware nor the Bluetooth NV blob carries an
# address of its own, so without this both come up with a random / zero address.
#
# WLAN: the ath11k patch in linux-redmi-marble
# (ath11k-load-wlan-mac-from-firmware) reads wlan_mac.bin through
# ath11k_core_firmware_request() when neither the device tree nor the firmware
# provided a MAC, so the file has to be in the firmware directory before the
# driver registers. ath11k_pci is blacklisted in
# /usr/lib/modprobe.d/marble-wlan.conf to keep udev from autoloading it early;
# this script loads it once the file is in place.
#
# Bluetooth: the stock NV blob qca/wcnhpnv21g.bin has no BD_ADDR, so the
# controller answers HCI_Read_BD_ADDR with 00:00:00:00:00:00 and the kernel
# marks it HCI_UNCONFIGURED, closes it and leaves it out of the management
# index list - bluetoothd never sees a controller. hci_qca provides
# ->set_bdaddr, so pushing a public address over the management interface
# unblocks it. Xiaomi derives the Bluetooth address from the WLAN address by
# decrementing the last octet (boards that also carry persist/wlan_bt/bt.mac
# show exactly that), which is what the bt_* branch below reproduces. The
# controller forgets it on the next power cycle, so this runs on every boot.
#
# A Bluetooth failure must not take the unit down: Wi-Fi is the primary
# interface, so that part only warns.
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
		# Do not rely on /dev/disk/by-partlabel/persist: that symlink is
		# created by udev, and this unit runs early enough that it is
		# sometimes still missing. Mounting that path then fails with
		# "Can't lookup blockdev", the unit dies, ath11k_pci is never
		# modprobe'd (it is blacklisted on purpose) and the device boots
		# with no WLAN at all - only Bluetooth comes up, because bootmac
		# handles that one separately.
		#
		# findfs asks the kernel for the partition by its PARTLABEL
		# property, so it works before udev has published any symlink.
		# The retry covers the window where the UFS block device itself
		# has not been probed yet.
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
	# Deliberately no modprobe here: loading ath11k_pci without the factory
	# address makes the driver fall back to a random MAC, which silently
	# breaks MAC-bound DHCP reservations and AP allowlists. Better to leave
	# WLAN down and let the failure be visible.
	echo "no wlan_mac.bin under persist; leaving WLAN down instead of using a random MAC" >&2
	if [ "$src" != "$PERSIST_SRC" ]; then
		umount "$MNT" 2>/dev/null || true
		rmdir "$MNT" 2>/dev/null || true
	fi
	exit 1
fi

wlan=$(sed -n 's/^wlan0=//p' "$src" | tr -d '\r\n' | tail -c 12)

# Copy unconditionally: /lib/firmware persists across boots, so a file left
# behind by an earlier boot (or by a build that shipped a placeholder) would
# otherwise keep the wrong address forever.
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

# The controller cannot be reprogrammed while powered, and bluetoothd powers it
# up as soon as it shows up, so power it down first and retry a few times in
# case the daemon races us.
i=0
while [ $i -lt 5 ]; do
	btmgmt --index 0 power off >/dev/null 2>&1 || true
	if btmgmt --index 0 public-addr "$addr" >/dev/null 2>&1; then
		echo "hci0 public address set to $addr"
		# bluetoothd's auto-enable only fires when the index is added; our
		# power off / address change happens after that, so whether the
		# controller ends up powered is a race. Power it back on here.
		btmgmt --index 0 power on >/dev/null 2>&1 || true
		exit 0
	fi
	sleep 2
	i=$((i + 1))
done

echo "could not set the hci0 public address to $addr"
exit 0

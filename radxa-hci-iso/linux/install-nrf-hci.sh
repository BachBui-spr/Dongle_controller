#!/bin/bash
# install-nrf-hci.sh - Cai phia Radxa Dragon Q6A cho nRF52840 Dongle (firmware radxa-hci-iso)
#
#   sudo bash install-nrf-hci.sh                # cai + tat Bluetooth AIC (dongle thanh hci0)
#   sudo bash install-nrf-hci.sh --keep-aic-bt  # cai nhung giu Bluetooth AIC (dongle thanh hci1)
#   sudo bash install-nrf-hci.sh --uninstall    # go bo, tra lai Bluetooth AIC
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
[ "$(id -u)" = 0 ] || { echo "Can chay voi sudo"; exit 1; }

if [ "${1:-}" = --uninstall ]; then
	systemctl stop radxa-nrf-hci.service 2>/dev/null || true
	systemctl stop radxa-addr-res.service 2>/dev/null || true
	rm -f /etc/udev/rules.d/90-radxa-nrf-hci.rules /etc/systemd/system/radxa-nrf-hci.service \
	      /usr/local/sbin/radxa-addr-res.sh /etc/systemd/system/radxa-addr-res.service \
	      /etc/modprobe.d/disable-aic-bt.conf
	systemctl daemon-reload
	udevadm control --reload
	modprobe aic_btusb_usb 2>/dev/null || true
	echo "Da go. Neu Bluetooth AIC chua len: rut dien board ~10 s roi cam lai."
	exit 0
fi

install -m 644 "$HERE/90-radxa-nrf-hci.rules" /etc/udev/rules.d/
install -m 644 "$HERE/radxa-nrf-hci.service"  /etc/systemd/system/
install -m 755 "$HERE/radxa-addr-res.sh"      /usr/local/sbin/
install -m 644 "$HERE/radxa-addr-res.service" /etc/systemd/system/
if [ "${1:-}" != --keep-aic-bt ]; then
	install -m 644 "$HERE/disable-aic-bt.conf" /etc/modprobe.d/
	# Go ngay driver Bluetooth AIC (khong can khoi dong lai); Wi-Fi van chay.
	# File DKMS la aic_btusb_usb.ko nhung module nap vao mang ten aic_btusb: modprobe -r khong tim thay
	# ten nao khop ca hai (modinfo aic_btusb -> not found) nen phai go bang rmmod theo ten trong lsmod.
	rmmod aic_btusb 2>/dev/null || rmmod aic_btusb_usb 2>/dev/null || true
fi
systemctl daemon-reload
# Dongle dang chay tu lan cai truoc (luc do la hci1): dang ky lai de lay chi so trong nho nhat (hci0)
systemctl try-restart radxa-nrf-hci.service 2>/dev/null || true
udevadm control --reload
modprobe hci_uart
udevadm trigger --subsystem-match=tty --action=add
sleep 3

echo "== Trang thai"
if [ -e /dev/ttyRadxaHCI ]; then
	echo "  dongle: $(readlink -f /dev/ttyRadxaHCI)"
	systemctl --no-pager --lines=0 status radxa-nrf-hci.service | sed -n '1,3p'
else
	echo "  chua thay dongle (/dev/ttyRadxaHCI): cam dongle da nap firmware radxa-hci-iso"
fi
for h in /sys/class/bluetooth/hci*; do
	[ -e "$h" ] || continue
	# Dongle qua btattach: thiet bi la cong tty, khong co link driver (readlink -f van tra ".../driver")
	drv=""
	[ -e "$h/device/driver" ] && drv=$(basename "$(readlink -f "$h/device/driver")")
	echo "  $(basename "$h"): driver=${drv:-hci_uart}"
done
echo "Kiem tra: bluetoothctl show | grep -E 'Controller|Powered'; sudo bash ~/hci-le-audit.sh"

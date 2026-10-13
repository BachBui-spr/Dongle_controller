#!/bin/bash
# flash-dongle.sh - Nap firmware radxa-hci-iso vao dongle nRF52840 (chay tren PC).
#
#   bash flash-dongle.sh [iso|lean|baseline] [cong]      (mac dinh: iso; build truoc bang build.sh <bien the>)
#
# Tu nhan loai bootloader dang chay:
# - Bootloader UF2 (vd Adafruit nRF52): dongle hien thanh MOT O USB co file INFO_UF2.TXT. Dua dongle vao bootloader
#   nhu cach cua dongle (thuong la bam nut reset 2 lan nhanh, hoac giu nut khi cam vao) cho den khi o USB hien ra,
#   roi chay script: script chep dist/radxa-hci-<bien the>.uf2 vao o do. O USB nam cho khac /media/$USER hay
#   /run/media/$USER thi chi duong dan: UF2_DRIVE=/duong/dan/o bash flash-dongle.sh lean
# - Bootloader Nordic (PCA10059 nguyen ban, "Open DFU Bootloader", 1915:521f): nhan RESET (SW2) cho LED do sang mo dan,
#   script nap goi DFU qua cong serial bang nrfutil. Can quyen /dev/ttyACM* (nhom dialout).
set -euo pipefail
ROOT=${NRF_HCI_ROOT:-$HOME/nrf-hci-iso}
P=$(cd "$(dirname "$0")" && pwd)
VARIANT=iso
case "${1:-}" in iso|lean|baseline) VARIANT=$1; shift ;; esac

# ---- Bootloader UF2: o USB co INFO_UF2.TXT
UF2=$P/dist/radxa-hci-$VARIANT.uf2
if [ -n "${UF2_DRIVE:-}" ]; then
	INFO=$UF2_DRIVE/INFO_UF2.TXT
	[ -f "$INFO" ] || { echo "Khong thay $INFO: $UF2_DRIVE khong phai o USB cua bootloader UF2"; exit 1; }
else
	INFO=$(ls /media/"$USER"/*/INFO_UF2.TXT /run/media/"$USER"/*/INFO_UF2.TXT 2>/dev/null | head -1 || true)
fi
if [ -n "$INFO" ]; then
	DRIVE=$(dirname "$INFO")
	[ -f "$UF2" ] || { echo "Chua co $UF2 - chay: bash build.sh $VARIANT"; exit 1; }
	echo "Bootloader UF2 tai $DRIVE:"
	sed 's/^/  /' "$INFO"
	echo "Chep $(basename "$UF2") ..."
	cp "$UF2" "$DRIVE/"
	sync || true   # bootloader nap xong tu khoi dong lai, o USB bien mat
	echo "Xong. Dongle khoi dong lai voi firmware '$VARIANT'; tren Linux se hien: 2fe3:0004 'Radxa HCI ISO nRF52840' (ttyACM)."
	exit 0
fi

# ---- Bootloader Nordic (Open DFU Bootloader)
PKG=$P/dist/radxa-hci-$VARIANT-nrf52840dongle.zip
export NRFUTIL_HOME=$ROOT/.nrfutil
[ -f "$PKG" ] || { echo "Chua co $PKG - chay: bash build.sh $VARIANT"; exit 1; }
PORT=${1:-}
if [ -z "$PORT" ]; then
	# Bootloader Nordic: VID 1915, PID 521f, ten "Open DFU Bootloader"
	PORT=$(ls /dev/serial/by-id/*Open_DFU_Bootloader* 2>/dev/null | head -1 || true)
	[ -n "$PORT" ] || { echo "Khong thay dongle o che do bootloader: khong co o USB UF2 (INFO_UF2.TXT) va khong co cong" \
		"'Open DFU Bootloader'. Dua dongle vao bootloader roi chay lai."; exit 1; }
fi
echo "Nap $PKG vao $PORT ..."
"$ROOT/bin/nrfutil" nrf5sdk-tools dfu usb-serial -pkg "$PKG" -p "$PORT"
echo "Xong. Dongle khoi dong lai voi firmware '$VARIANT'; tren Linux se hien: 2fe3:0004 'Radxa HCI ISO nRF52840' (ttyACM)."

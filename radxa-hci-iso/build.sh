#!/bin/bash
# build.sh - Build lai firmware radxa-hci-iso cho nRF52840 Dongle va dong goi file nap (DFU zip).
# Can moi truong da dung san trong $NRF_HCI_ROOT (mac dinh ~/nrf-hci-iso): Zephyr v4.4.2, SDK 1.0.1, nrfutil.
#   bash build.sh            # ban "iso": firmware/ (moi tinh nang ISO, overlay-all cua Zephyr CI)
#   bash build.sh lean       # firmware-lean/: cau hinh LE Audio CIS central chinh thuc cua Zephyr
#   bash build.sh baseline   # firmware-baseline/: nguyen ban hci_uart, khong ISO (doi chung)
# Ket qua: dist/radxa-hci-<bien the>-nrf52840dongle.{zip,hex} (bootloader Nordic) va dist/radxa-hci-<bien the>.uf2
# (bootloader UF2: chep vao o USB cua bootloader, flash-dongle.sh tu lam)
set -euo pipefail
ROOT=${NRF_HCI_ROOT:-$HOME/nrf-hci-iso}
P=$(cd "$(dirname "$0")" && pwd)
VARIANT=${1:-iso}
case $VARIANT in
	iso)      APP=$P/firmware;          BUILD=$P/build
	          CHECK="BT_HCI_RAW_H4 BT_CTLR_ADV_ISO BT_CTLR_SYNC_ISO BT_CTLR_CENTRAL_ISO BT_CTLR_PERIPHERAL_ISO" ;;
	lean)     APP=$P/firmware-lean;     BUILD=$P/build-lean;     CHECK="BT_HCI_RAW_H4 BT_CTLR_CENTRAL_ISO" ;;
	baseline) APP=$P/firmware-baseline; BUILD=$P/build-baseline; CHECK="BT_HCI_RAW_H4" ;;
	*) echo "Dung: bash build.sh [iso|lean|baseline]"; exit 2 ;;
esac
NAME=radxa-hci-$VARIANT-nrf52840dongle
. "$ROOT/.venv/bin/activate"
export ZEPHYR_SDK_INSTALL_DIR=$ROOT/zephyr-sdk-1.0.1
export ZEPHYR_BASE=$ROOT/zephyrproject/zephyr
export NRFUTIL_HOME=$ROOT/.nrfutil

cd "$ROOT/zephyrproject"
west build -p always -b nrf52840dongle/nrf52840 -d "$BUILD" "$APP"

# Kiem tra cac tuy chon cot loi that su duoc bat (Kconfig co the am tham tat neu thieu phu thuoc)
for opt in $CHECK; do
	grep -q "^CONFIG_${opt}=y" "$BUILD/zephyr/.config" || { echo "LOI: CONFIG_${opt} khong duoc bat"; exit 1; }
done

mkdir -p "$P/dist"
"$ROOT/bin/nrfutil" nrf5sdk-tools pkg generate --hw-version 52 --sd-req=0x00 \
	--application "$BUILD/zephyr/zephyr.hex" --application-version 1 \
	"$P/dist/$NAME.zip"
cp "$BUILD/zephyr/zephyr.hex" "$P/dist/$NAME.hex"
# Ban UF2 cho bootloader UF2 kieu Adafruit (family nRF52840 0xADA52840, nap tu 0x1000 nhu .hex). Tao tu .hex chu
# khong tu .bin: giong tung byte file da nap thanh cong (khe trong giua section lap 00 thay vi ff).
python "$ZEPHYR_BASE/scripts/build/uf2conv.py" -c -f 0xADA52840 -o "$P/dist/radxa-hci-$VARIANT.uf2" \
	"$BUILD/zephyr/zephyr.hex" >/dev/null
(cd "$P/dist" && sha256sum radxa-hci-*-nrf52840dongle.* radxa-hci-*.uf2 > SHA256SUMS && cat SHA256SUMS)

#!/bin/bash
# radxa-addr-res.sh - Giu co ADDRESS_RESOLUTION (0x4) cho cac thiet bi LE da ghep (co IRK) tren hci0. Chay bang root.
#
# Ly do (TEST_REPORT F58): dongle co LL Privacy -> kernel 6.18 dua DIA CHI DINH DANH cua tai nghe vao accept list va de
# controller tu phan giai dia chi rieng tu (RPA), nhung chi nap IRK vao controller khi thiet bi co co ADDRESS_RESOLUTION.
# BlueZ 5.72 khong dat co nay (5.87 moi dat) -> controller loc bo quang ba RPA cua tai nghe da ghep, tai nghe khong tu
# noi lai. Co mat moi khi hci0 dang ky lai (cam lai dongle, khoi dong lai, nap firmware) hoac BlueZ them lai thiet bi
# (ghep lai, ngat/noi tay) -> chay thanh dich vu rieng (radxa-addr-res.service), cu INTERVAL giay kiem tra va bat lai.
#
# KHONG chay trong ExecStartPost cua radxa-nrf-hci.service: ban dau lam vay, script chay qua 90 s (TimeoutStartSec) khi
# co tai nghe chua vao danh sach tu noi lai -> systemd giet btattach, khoi dong lai moi 92 s -> hci0 dang ky lai, tai
# nghe mat ket noi giua luc phat nhac (TEST_REPORT F65).
#
#   radxa-addr-res.sh            # chay mai (dich vu)
#   radxa-addr-res.sh --once     # kiem tra mot lan roi thoat
# Bien moi truong (de thu): BT_STATE_DIR (mac dinh /var/lib/bluetooth), INTERVAL (mac dinh 10), HCI_INDEX (mac dinh 0)
set -u
STATE=${BT_STATE_DIR:-/var/lib/bluetooth}
INTERVAL=${INTERVAL:-10}
IDX=${HCI_INDEX:-0}
log() { logger -t radxa-addr-res "$*" 2>/dev/null; echo "$*"; }

# Mot luot: moi thiet bi da ghep co IRK cua adapter hien tai -> thieu co 0x4 ma kernel ho tro thi bat.
check_once() {
	local adapter dir mac type out sup cur flags
	adapter=$(timeout 5 btmgmt --index "$IDX" info 2>/dev/null | awk '/^[[:space:]]*addr / {print $2; exit}')
	[[ -n "$adapter" && -d "$STATE/$adapter" ]] || return 0      # chua co hci0 / adapter chua co thu muc
	for dir in "$STATE/$adapter"/??:??:??:??:??:??; do
		[[ -f "$dir/info" ]] || continue
		grep -q '^\[IdentityResolvingKey\]' "$dir/info" || continue  # khong dung dia chi rieng tu: khong can
		mac=${dir##*/}
		type=1                                                    # 1 = LE Public, 2 = LE Random (dinh danh tinh)
		grep -qE '^AddressType=static' "$dir/info" && type=2
		out=$(timeout 5 btmgmt --index "$IDX" get-flags -t "$type" "$mac" 2>/dev/null)
		sup=$(awk '/Supported Flags:/ {print $3}' <<<"$out")
		cur=$(awk '/Current Flags:/ {print $3}' <<<"$out")
		# Chua co trong danh sach tu noi lai cua kernel (Invalid Parameters): luot sau thu lai
		[[ "$sup" =~ ^0x[0-9a-fA-F]+$ && "$cur" =~ ^0x[0-9a-fA-F]+$ ]] || continue
		((sup & 0x4)) || continue                                 # controller khong co LL Privacy: khong can
		((cur & 0x4)) && continue
		flags=$(printf '0x%x' $((cur | 0x4 | (sup & 0x2))))
		timeout 5 btmgmt --index "$IDX" set-flags -f "$flags" -t "$type" "$mac" >/dev/null 2>&1 &&
			log "bat ADDRESS_RESOLUTION cho $mac (co $flags)"
	done
	return 0
}

if [[ "${1:-}" == --once ]]; then
	check_once
	exit 0
fi
while true; do
	check_once
	sleep "$INTERVAL"
done

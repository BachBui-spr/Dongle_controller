# radxa-hci-iso — Bluetooth LE Audio controller cho Radxa Dragon Q6A bằng nRF52840 Dongle

Firmware **Bluetooth HCI controller** chạy trên **Nordic nRF52840 Dongle (PCA10059)**. Dongle cắm vào cổng USB của Radxa Dragon Q6A và **thay phần Bluetooth của chip AIC8800D80**, vốn có firmware ISO bị lỗi (xem `DRAGON-Q6A-LE-AUDIO-BUILD.md`, mục 7). WiFi vẫn chạy trên chip AIC.

| | |
|---|---|
| Firmware | Zephyr **v4.4.2**, mẫu `samples/bluetooth/hci_uart` + controller mã nguồn mở `BT_LL_SW_SPLIT`, bật đầy đủ ISO |
| Giao tiếp | **H4 qua USB CDC-ACM** (cổng serial ảo `/dev/ttyACM*`); USB `2fe3:0004`, tên `Radxa HCI ISO nRF52840` |
| Phía Linux | Kernel `6.18.0-bt54` (hoặc `btle`): `cdc_acm` + `hci_uart` (H4) + `btattach`; không phải build lại kernel |
| Tài nguyên | Flash 216 KB / 1020 KB (21%), RAM 67 KB / 256 KB (26%) |
| Trạng thái | ✅ Build và đóng gói thành công, cấu hình đã được kiểm tra. ⏳ **Chưa thử trên phần cứng thật** (chưa có dongle). |

---

## 1. Kiến trúc

```
 PipeWire / BlueZ 5.72 / Bumble / script RF_Gateway        (giữ nguyên)
            │  ISO socket, HCI User Channel
 Kernel 6.18.0-bt54: ISO subsystem (đã pass iso-tester 136/136)   (giữ nguyên)
            │
 hci_uart (H4)  ◄── btattach -B /dev/ttyRadxaHCI -P h4            (MỚI: systemd + udev)
            │  mỗi gói kèm 1 byte loại: 01 lệnh, 02 ACL, 04 event, 05 ISO
 cdc_acm  ──USB──►  nRF52840 Dongle
                     ├─ USB CDC-ACM (Zephyr USB device_next)
                     ├─ hci_uart: tách gói H4, chuyển cho controller
                     └─ Controller Zephyr (Link Layer): ISO, BIS/CIS, LE Audio
                                     │
                                  radio 2.4 GHz (+8 dBm)

 Chip AIC8800D80: chỉ còn dùng WiFi (driver aic8800_fdrv); Bluetooth AIC bị tắt (blacklist aic_btusb)
```

### Vì sao chọn H4 qua CDC-ACM thay vì `hci_usb`

- **Byte loại gói rõ ràng.** Mỗi gói H4 có byte loại (`0x05` = ISO). Bluetooth SIG chưa chuẩn hóa cách tách gói ISO khỏi ACL khi đi qua USB bulk; đó chính là chỗ mơ hồ đã gặp với chip AIC. H4 tránh hẳn vấn đề này.
- **Driver chuẩn đã sẵn trong kernel.** Kernel đã có `hci_uart` H4, nhận ISO bằng `H4_RECV_ISO` (`drivers/bluetooth/hci_h4.c:104`) và tự gắn byte `0x05` khi gửi.
- **Dễ gỡ lỗi.** Nếu controller gặp lỗi, firmware gửi **sự kiện vendor "Fatal Error" kèm địa chỉ lệnh** (`CONFIG_BT_HCI_VS_FATAL_ERROR`), thay vì treo im lặng như chip AIC.

---

## 2. Cấu trúc thư mục

```
~/nrf-hci-iso/
├── .venv/                     Python venv có west 1.5.0 + thư viện build Zephyr
├── zephyrproject/             Zephyr v4.4.2 (chỉ module: cmsis, cmsis_6, hal_nordic, mbedtls, tf-psa-crypto)
├── zephyr-sdk-1.0.1/          Zephyr SDK 1.0.1 (chỉ toolchain arm-zephyr-eabi, gcc 14.3.0)
├── bin/nrfutil                nrfutil 8.2.1 (+ nrf5sdk-tools 1.1.0 trong .nrfutil/)
└── radxa-hci-iso/
    ├── README.md              tài liệu này
    ├── build.sh               build + kiểm tra cấu hình + đóng gói DFU
    ├── flash-dongle.sh        nạp firmware vào dongle qua USB (bootloader Nordic)
    ├── firmware/
    │   ├── CMakeLists.txt
    │   ├── prj.conf           cấu hình controller + ISO (giải thích ở mục 3)
    │   └── src/main.c         cầu nối H4 ↔ controller (nguyên bản Zephyr v4.4.2, Apache-2.0)
    ├── linux/                 cài lên Radxa
    │   ├── install-nrf-hci.sh
    │   ├── 90-radxa-nrf-hci.rules
    │   ├── radxa-nrf-hci.service
    │   └── disable-aic-bt.conf
    ├── build/                 thư mục build Zephyr
    └── dist/
        ├── radxa-hci-iso-nrf52840dongle.zip   gói nạp DFU
        ├── radxa-hci-iso-nrf52840dongle.hex   (cho J-Link/SWD nếu cần)
        └── SHA256SUMS
```

`firmware/src/main.c` lấy từ Zephyr v4.4.2 và **chỉ sửa một chỗ**: cách báo lỗi controller. Bản gốc gửi sự kiện Fatal Error bằng `uart_poll_out()` sau khi đã khóa ngắt; qua USB CDC-ACM thì sự kiện này không ra được, nên dongle treo im lặng. Bản sửa ghi lỗi vào RAM cố định `0x20030000` rồi reset chip, và gửi lại sự kiện ở lần khởi động sau (TEST_REPORT F59). Ngoài chỗ đó, `main.c` làm ba việc:
- nhận từng byte H4 từ cổng CDC-ACM, ghép thành gói **lệnh, ACL hoặc ISO**, rồi chuyển cho controller qua `bt_send()`;
- đưa event, ACL và ISO từ controller trở lại cổng CDC-ACM (luồng `tx_thread`);
- khi controller gặp lỗi, gửi sự kiện Fatal Error rồi dừng (`bt_ctlr_assert_handle`).

Toàn bộ khác biệt so với mẫu gốc nằm ở `prj.conf`.

---

## 3. Cấu hình firmware (`firmware/prj.conf`)

File này ghép từ ba nguồn của Zephyr:
- `prj.conf` của mẫu `hci_uart`;
- `boards/nrf52840dongle_nrf52840.conf`;
- `overlay-all-bt_ll_sw_split.conf`, cấu hình ISO đầy đủ mà Zephyr CI dùng.

Phần Direction Finding đã được **bỏ**, vì nó cần mảng ăng-ten mà dongle không có.

| Nhóm | Tùy chọn chính | Ý nghĩa |
|---|---|---|
| Giao tiếp | `BT_HCI_RAW=y`, `BT_HCI_RAW_H4=y`, `BT_HCI_RAW_H4_ENABLE=y` | Controller-only, gói theo định dạng H4 |
| USB | `CDC_ACM_SERIAL_PRODUCT_STRING="Radxa HCI ISO nRF52840"`, `MANUFACTURER_STRING="Radxa Q6A"` | Tên USB để udev trên Radxa nhận đúng dongle |
| Bộ đệm HCI | `BT_BUF_CMD_TX_SIZE=255`, `BT_BUF_EVT_RX_SIZE=255`, `BT_BUF_ACL_TX/RX_SIZE=251/255` | Khớp kích thước lệnh (`LE Set CIG Parameters`…) và ACL mà kernel gửi |
| Phát BIS (Auracast) | `BT_CTLR_ADV_ISO=y`, `ADV_ISO_STREAM_MAX=2`, `ADV_ISO_PDU_LEN_MAX=247` | Phát BIG tối đa 2 luồng (stereo) |
| Thu BIS | `BT_CTLR_SYNC_ISO=y`, `SYNC_ISO_STREAM_MAX=2` | Nghe Auracast |
| CIS (tai nghe) | `BT_CTLR_CENTRAL_ISO=y`, `BT_CTLR_PERIPHERAL_ISO=y`, `CONN_ISO_STREAMS=2`, `CONN_ISO_GROUPS=1` | 1 nhóm CIG, 2 luồng CIS (trái + phải) |
| Bộ đệm ISO | `BT_CTLR_ISO_TX_BUFFERS=18`, `ISO_TX_BUFFER_SIZE=255`, `ISO_TX_SDU_LEN_MAX=247` | Báo cho host qua `LE Read Buffer Size v2`: **18 gói × 255 byte**. SDU LC3 48 kHz/10 ms tối đa 155 byte, nằm gọn trong giới hạn. |
| ISO qua HCI | `BT_ISO_TX_MTU=310`, `BT_ISO_RX_MTU=310`, `BT_ISO_MAX_CHAN=4` | Kích thước gói ISO trên đường H4 |
| PAST | `BT_CTLR_SYNC_TRANSFER_SENDER=y`, `…_RECEIVER=y` | 5.1 PAST |
| Công suất phát | `BT_CTLR_TX_PWR_PLUS_8=y`, `BT_CTLR_TX_PWR_DYNAMIC_CONTROL=y` | +8 dBm; đổi công suất lúc chạy bằng lệnh vendor của Zephyr. **Không phải** tính năng LE Power Control 5.2: controller mã nguồn mở không có (Zephyr v4.4.2 chỉ bật `BT_CTLR_LE_POWER_CONTROL`, subrating, PAwR cho ESP32), board báo `LE_POWER_CONTROL_REQUEST [không]` |
| Gỡ lỗi | `BT_CTLR_ASSERT_HANDLER=y`, `BT_HCI_VS_FATAL_ERROR=y` | Controller lỗi thì báo lên host kèm địa chỉ lệnh |
| Lập lịch | `BT_CTLR_ADV_ISO_RESERVE_MAX=y`, `BT_TICKER_EXT=y`… | Giữ nguyên như cấu hình ISO của Zephyr CI |

`build.sh` **tự kiểm tra** trong `build/zephyr/.config` rằng 5 tùy chọn cốt lõi (`BT_HCI_RAW_H4`, `BT_CTLR_ADV_ISO`, `BT_CTLR_SYNC_ISO`, `BT_CTLR_CENTRAL_ISO`, `BT_CTLR_PERIPHERAL_ISO`) thật sự được bật. Kconfig có thể âm thầm tắt một tùy chọn nếu thiếu điều kiện phụ thuộc.

---

## 4. Dựng môi trường build từ đầu (đã làm sẵn trên PC này)

Ghi lại để làm lại trên máy khác:
```bash
B=~/nrf-hci-iso; mkdir -p $B && cd $B
python3 -m venv .venv && . .venv/bin/activate && pip install west
west init -m https://github.com/zephyrproject-rtos/zephyr --mr v4.4.2 zephyrproject
cd zephyrproject
west config manifest.project-filter -- '-.*,+cmsis,+cmsis_6,+hal_nordic,+mbedtls,+tf-psa-crypto'
west update --narrow -o=--depth=1
pip install -r zephyr/scripts/requirements-base.txt
cd $B
U=https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v1.0.1
wget $U/zephyr-sdk-1.0.1_linux-x86_64_minimal.tar.xz $U/toolchain_gnu_linux-x86_64_arm-zephyr-eabi.tar.xz
tar xf zephyr-sdk-1.0.1_linux-x86_64_minimal.tar.xz
mkdir -p zephyr-sdk-1.0.1/gnu && tar xf toolchain_gnu_linux-x86_64_arm-zephyr-eabi.tar.xz -C zephyr-sdk-1.0.1/gnu
mkdir -p bin && curl -L -o bin/nrfutil \
  https://files.nordicsemi.com/artifactory/swtools/external/nrfutil/executables/x86_64-unknown-linux-gnu/nrfutil
chmod +x bin/nrfutil && NRFUTIL_HOME=$B/.nrfutil bin/nrfutil install nrf5sdk-tools
```
- Module `mbedtls` và `tf-psa-crypto` là **bắt buộc**: `BT_CRYPTO` luôn bật cùng Bluetooth và cần thư viện mã hóa PSA. Thiếu hai module này, build dừng ở bước Kconfig.
- Không cần `setup.sh` của SDK: `build.sh` tự đặt `ZEPHYR_SDK_INSTALL_DIR`.

---

## 5. Build firmware

```bash
bash ~/nrf-hci-iso/radxa-hci-iso/build.sh
```
Mất khoảng 25 giây. Script build, kiểm tra cấu hình, đóng gói `dist/radxa-hci-iso-nrf52840dongle.zip` và ghi `SHA256SUMS`. Build tái lập được: file `.hex` cho ra cùng sha256 `3be94b01…eb041` qua các lần build.

---

## 6. Nạp firmware vào dongle (trên PC)

> **Dongle dùng bootloader UF2** (kiểu Adafruit, family `0xADA52840`, nạp từ `0x1000`). Đưa dongle vào bootloader cho tới khi nó hiện thành **ổ USB có file `INFO_UF2.TXT`**, rồi chạy `bash flash-dongle.sh [iso|lean|baseline]`. Script tự tìm ổ trong `/media/$USER` hoặc `/run/media/$USER` và chép `dist/radxa-hci-<biến thể>.uf2` vào đó; ổ ở chỗ khác thì đặt `UF2_DRIVE=<đường dẫn>`. Có thể chép tay cũng được.
> - File `.uf2` được tạo từ `zephyr.hex` bằng `uf2conv.py -c -f 0xADA52840`. Cách này cho ra file giống từng byte với `radxa-hci-iso.uf2` đã nạp thành công. Nếu tạo từ `.bin` thì sẽ khác 4 byte ở khe `tbss`, vì khe được lấp `ff` thay vì `00`.
> - Các bước 1–4 dưới đây chỉ áp dụng cho dongle PCA10059 còn bootloader Nordic nguyên bản.

1. Cắm dongle vào PC.
2. Nhấn nút **RESET (SW2)**. Nút này nằm ở **đầu đối diện với cổng USB** và **không quay lên trên**: ấn **từ ngoài vào, hướng về phía cổng USB**. Khi **LED đỏ sáng mờ dần (fade)** là dongle đang ở bootloader Nordic ("Open DFU Bootloader", `1915:521f`).
3. Nạp:
   ```bash
   bash ~/nrf-hci-iso/radxa-hci-iso/flash-dongle.sh              # tự tìm cổng bootloader
   # hoặc: bash flash-dongle.sh /dev/ttyACM0
   ```
4. Dongle tự khởi động lại với firmware mới. `lsusb` sẽ hiện `2fe3:0004`, và `dmesg` có dòng `cdc_acm … ttyACM0`, kèm tên `Radxa HCI ISO nRF52840`.

Ghi chú:
- User trên PC cần thuộc nhóm `dialout` (máy này đã có).
- Firmware nằm ở địa chỉ `0x1000`, **giữ nguyên bootloader Nordic**. Muốn nạp lại firmware khác thì chỉ cần nhấn RESET lần nữa.

Kiểm tra nhanh ngay trên PC trước khi mang sang board (không bắt buộc):
```bash
sudo btattach -B /dev/ttyACM0 -P h4 -S 1000000 &      # tạo hciX mới trên PC
sudo btmgmt --index <X> info                            # manufacturer 1521 (0x05F1), version 13 (5.4)
sudo kill %1
```

---

### 6.1. Ba biến thể firmware (khoanh lỗi kết nối tai nghe)

Bản `iso` mặc định ghép từ `overlay-all-bt_ll_sw_split.conf`. Đó là cấu hình "bật mọi thứ" mà Zephyr CI dùng để thử **biên dịch**. Với bản này, ở vai central, kết nối tới tai nghe Samsung rớt ngay khi mở (`0x3E` hoặc `0x08`, xem TEST_REPORT F56). Ở vai ngoại vi (GATT với laptop) thì chạy ổn.

| Biến thể | Thư mục | Nội dung | Build / nạp |
|---|---|---|---|
| `iso` | `firmware/` | Mọi tính năng ISO (CIS + BIS), +8 dBm, Coded PHY... | `bash build.sh` / `bash flash-dongle.sh` |
| `lean` | `firmware-lean/` | Cấu hình LE Audio CIS central **chính thức** của Zephyr (`hci_ipc/nrf5340_cpunet_iso_central-bt_ll_sw_split.conf`), chuyển sang dongle, và thêm **địa chỉ public cố định** (`firmware/src/public_addr.c`). Lý do: BlueZ 5.72 mở ISO socket với kiểu địa chỉ LE Public, nên với địa chỉ tĩnh thì báo `No route to host` (TEST_REPORT F57). Dùng chính sách CIS **RELIABILITY** thay cho LOW_LATENCY, vì với LOW_LATENCY, 2 tai × RTN 13 làm controller treo (F59). **Ghép đôi được tai nghe Samsung** | `bash build.sh lean` / `bash flash-dongle.sh lean` |
| `baseline` | `firmware-baseline/` | Nguyên bản `hci_uart`, không ISO, chỉ thêm `BT_EXT_ADV` (tai nghe chỉ quảng bá extended) | `bash build.sh baseline` / `bash flash-dongle.sh baseline` |

- Cả ba biến thể dùng cùng tên USB, nên trên Radxa không phải sửa gì. Địa chỉ Bluetooth (static random, lấy từ FICR) cũng không đổi.
- Đọc kết quả: `lean` ghép đôi được thì lỗi do các tùy chọn thừa của bản `iso`. Chỉ `baseline` ghép được thì lỗi do phần ISO của controller. Cả hai đều không ghép được thì lỗi tương thích link layer Zephyr ↔ tai nghe, không phụ thuộc cấu hình.

## 7. Cài đặt trên Radxa Dragon Q6A

```bash
# Từ PC: chép thư mục linux/ sang board
scp -r ~/nrf-hci-iso/radxa-hci-iso/linux radxa@radxa-dragon-q6a.local:~/radxa-hci-iso
# Trên board: cắm dongle vào cổng USB-A, rồi
sudo bash ~/radxa-hci-iso/install-nrf-hci.sh
```

| File | Đặt vào | Tác dụng |
|---|---|---|
| `90-radxa-nrf-hci.rules` | `/etc/udev/rules.d/` | Nhận dongle theo `2fe3:0004` + tên `Radxa HCI ISO nRF52840`, tạo `/dev/ttyRadxaHCI`, không cho ModemManager đụng vào cổng, gọi systemd chạy `radxa-nrf-hci.service` |
| `radxa-nrf-hci.service` | `/etc/systemd/system/` | `btattach -B /dev/ttyRadxaHCI -P h4`. Gắn với thiết bị: **cắm thì tự chạy, rút thì tự dừng** |
| `radxa-addr-res.sh` + `radxa-addr-res.service` | `/usr/local/sbin/`, `/etc/systemd/system/` | Dịch vụ **riêng**, do `radxa-nrf-hci.service` kéo lên (`Wants=`). Cứ 10 giây, nó bật lại cờ `ADDRESS_RESOLUTION` cho các tai nghe đã ghép có IRK (đọc từ `/var/lib/bluetooth`). Nếu thiếu cờ này thì với BlueZ 5.72 + kernel 6.18, dongle lọc bỏ quảng bá địa chỉ riêng tư của tai nghe, và tai nghe không tự nối lại (TEST_REPORT F58, F64). **Không** chạy trong `ExecStartPost`: chạy ở đó thì quá 90 giây systemd sẽ giết btattach, khiến hci0 được tạo lại mỗi 92 giây (F65). Xem kết quả: `sudo journalctl -t radxa-addr-res -b` |
| `disable-aic-bt.conf` | `/etc/modprobe.d/` | `blacklist aic_btusb_usb`: tắt Bluetooth của AIC, **WiFi không bị ảnh hưởng**. Nhờ vậy dongle thành **`hci0`**, và mọi script hiện có (`hci-socket:0`, `btmgmt --index 0`) chạy **không cần sửa** |

Tùy chọn:
- `sudo bash install-nrf-hci.sh --keep-aic-bt`: giữ Bluetooth AIC; khi đó dongle sẽ là `hci1`.
- `sudo bash install-nrf-hci.sh --uninstall`: gỡ bỏ, trả lại Bluetooth AIC.

Các file đã được kiểm tra trên board: `udevadm verify` → `Success: 1`; `systemd-analyze verify` không báo lỗi; tên unit `dev-ttyRadxaHCI.device` khớp với `systemd-escape -p /dev/ttyRadxaHCI`.

---

## 8. Kiểm tra trên Radxa (kết quả mong đợi)

| Bước | Lệnh | Mong đợi |
|---|---|---|
| Dongle được nhận | `ls -l /dev/ttyRadxaHCI; systemctl status radxa-nrf-hci` | Symlink tới `ttyACM0`; dịch vụ `active (running)` |
| Controller | `hciconfig -a hci0` hoặc `sudo btmgmt info` | `Bus: UART`, manufacturer `1521` (0x05F1), version 13 (5.4) |
| BlueZ | `bluetoothctl show` | `Powered: yes`, UUID PACS/ASCS/BASS, `ExperimentalFeatures: … ISO` |
| Bộ đệm ISO | `sudo btmon` + `sudo btmgmt power off; sudo btmgmt power on` | `LE Read Buffer v2 … ISO MTU: 255, ISO max packet: 18`. Controller Zephyr **khai báo đúng** lệnh v2, nên kernel dùng nó trực tiếp. |
| Kiểm kê | `sudo bash ~/hci-le-audit.sh` | Bit CIS Central/Peripheral, Isochronous Broadcaster, Synchronized Receiver = CO; các lệnh ISO `0x2062`, `0x2064`, `0x2068`, `0x206E`, `0x2070` = CO; mục đối chiếu "khớp" |
| **A1** (chip tự phát ISO) | `sudo bash ~/iso-a1a2/iso-a1a2.sh --a1-only` | `alive_during=5/5`, `testend=ok`, `alive_after=1`, `hwerr=0` |
| **A0** (host gửi ISO) | `sudo bash ~/iso-a1a2/iso-a1a2.sh` | `big=ok sent=4 completed=4 hwerr=0` (với chip AIC là `completed=0 hwerr=4`) |
| Phát liên tục | `cd ~/RF_Gateway && ./scripts/hci-iso-probe.sh --matrix` | Chip xác nhận khoảng 100 gói/s, không có Hardware Error |
| LE Audio thật | `bluetoothctl` ghép tai nghe LE Audio; `wpctl status`; `pw-play …` | btmon: `LE Set CIG Parameters` → `LE Create CIS` → `CIS Established` → `Setup ISO Data Path` → các gói `ISO Data TX` |

Nếu A0 và A1 đều đạt, đó là bằng chứng cuối cùng: kernel, driver và công cụ đều đúng, và lỗi trước đây chỉ nằm ở firmware AIC.

---

## 9. Khớp với kernel và chương trình hiện tại

| Thành phần | Trạng thái với dongle |
|---|---|
| Kernel `6.18.0-bt54` / `btle` | Dùng nguyên. Có sẵn `cdc-acm.ko`, `hci_uart.ko` (`CONFIG_BT_HCIUART_H4=y`); `btattach` có trong BlueZ 5.72 |
| Bản sửa `hci_sync.c` (đọc bộ đệm ISO v2) | Vô hại. Zephyr khai báo đúng bit lệnh, nên kernel đi nhánh chuẩn |
| Bản `bt54`: event mask 5.1–5.3 | Chỉ bật các event mà controller Zephyr báo có |
| `aic_btusb` đã vá ISO | Không dùng nữa (bị blacklist). Driver WiFi `aic8800_fdrv` vẫn chạy |
| `/etc/bluetooth/main.conf` (`Experimental`, `KernelExperimental` ISO) | Giữ nguyên, vẫn cần |
| `/etc/modprobe.d/radxa-bt-iso.conf` (`radxa_cis_max_rtn=2`) | Tham số không tồn tại trong `bluetooth.ko` hiện tại; kernel bỏ qua, vô hại |
| Script `iso-a1a2`, `hci-le-audit`, RF_Gateway `hci-iso-probe` | Chạy không cần sửa, vì dongle là `hci0` |
| Bumble (`hci-socket:0`) | Dùng được với `hci_uart`: User Channel hoạt động với mọi loại transport |

---

## 10. Xử lý sự cố

| Hiện tượng | Kiểm tra / cách xử lý |
|---|---|
| Không có `/dev/ttyRadxaHCI` | `lsusb \| grep 2fe3`; `dmesg \| grep cdc_acm`. Nếu dongle hiện `1915:521f` thì nó đang ở bootloader: rút cắm lại, hoặc nạp lại firmware |
| `radxa-nrf-hci.service` lỗi | `journalctl -u radxa-nrf-hci -b`. Kiểm tra `modprobe hci_uart`, và xem có tiến trình nào khác đang mở `ttyACM0` không |
| Vẫn thấy controller AIC (dongle thành `hci1`) | `lsmod \| grep aic_btusb`: nếu còn, chạy `sudo rmmod aic_btusb && sudo systemctl restart radxa-nrf-hci` (file DKMS tên `aic_btusb_usb.ko` nhưng module nạp vào tên `aic_btusb`, nên `modprobe -r` không gỡ được), hoặc khởi động lại để blacklist có hiệu lực |
| Event vendor `0xFF` (Fatal Error) trong btmon | Controller Zephyr bị assert: event có kèm địa chỉ lệnh (PC). Tra bằng `arm-zephyr-eabi-addr2line -e build/zephyr/zephyr.elf <PC>`. Rút cắm lại dongle để khởi động lại |
| Âm thanh giật hoặc mất gói | WiFi của AIC ở 2.4 GHz có thể gây nhiễu (hiện WiFi đang ở 5 GHz). Đặt dongle xa board bằng cáp nối dài USB |
| Muốn quay lại Bluetooth AIC | `sudo bash ~/radxa-hci-iso/install-nrf-hci.sh --uninstall`, rồi rút điện board |

---

## 11. Giới hạn cần biết

- **CIS trên nRF52840 với controller Zephyr** hoạt động nhưng **ít được kiểm chứng** bằng nRF5340; nền tảng LE Audio chính thức của Nordic là nRF5340. Phát và thu BIS (Auracast) là phần chắc chắn nhất. Nếu cần CIS ổn định cho sản phẩm, có thể build cùng cấu hình cho `nrf5340dk/nrf5340/cpunet` (dùng mẫu `hci_ipc`).
- Tối đa **2 luồng CIS trong 1 nhóm**, và **2 luồng BIS**. Muốn tăng thì sửa `CONN_ISO_STREAMS` và `ADV_ISO_STREAM_MAX`; RAM còn dư khoảng 74%.
- VID/PID `2fe3:0004` là mặc định của Zephyr, chỉ dùng cho phát triển. Sản phẩm thương mại cần VID/PID riêng (`CONFIG_CDC_ACM_SERIAL_VID/PID`).
- Firmware này **chưa chạy thử trên dongle thật**. Bảng ở mục 8 là kết quả mong đợi, cần xác nhận khi có phần cứng.

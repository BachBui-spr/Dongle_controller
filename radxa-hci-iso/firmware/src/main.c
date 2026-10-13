/*
 * Copyright (c) 2016 Nordic Semiconductor ASA
 * Copyright (c) 2015-2016 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/arch/cpu.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/drivers/uart.h>

#include <zephyr/net_buf.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/l2cap.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/buf.h>
#include <zephyr/bluetooth/hci_raw.h>
#include <zephyr/bluetooth/hci_vs.h>

#define LOG_MODULE_NAME hci_uart
LOG_MODULE_REGISTER(LOG_MODULE_NAME);

static const struct device *const hci_uart_dev =
	DEVICE_DT_GET(DT_CHOSEN(zephyr_bt_c2h_uart));
static K_THREAD_STACK_DEFINE(tx_thread_stack, CONFIG_BT_HCI_TX_STACK_SIZE);
static struct k_thread tx_thread_data;
static K_FIFO_DEFINE(tx_queue);

/* RX in terms of bluetooth communication */
static K_FIFO_DEFINE(uart_tx_queue);

#if defined(CONFIG_BT_HCI_VS_FATAL_ERROR)
/*
 * radxa-hci-iso (them vao mau goc): bao loi controller QUA LAN KHOI DONG LAI.
 * Mau goc gui su kien Fatal Error bang uart_poll_out() sau irq_lock(). Voi USB CDC-ACM viec truyen can ngat USB nen su
 * kien khong bao gio ra khoi dongle: dongle treo im lang, host chi thay "command 0x.... tx timeout" (TEST_REPORT F59).
 * O day: ghi ten file + dong (assert) hoac ly do + PC (fault CPU) vao RAM giu qua reset (__noinit), reset chip. Lan
 * khoi dong sau, ngay khi host gui goi HCI dau tien (HCI Reset cua kernel), gui lai su kien vendor Fatal Error (0xFF,
 * kieu Controller Assert) -> btmon ghi lai duoc. Fault CPU: file = "fatal-reason-N", dong = PC.
 */
#include <cmsis_core.h>

#define FAULT_MAGIC 0x46415554U	/* "FAUT" */

struct fault_record {
	uint32_t magic;
	uint32_t line;
	char file[64];
};

/* Dia chi co dinh giua RAM (0x20000000-0x2003FFFF): ung dung chi dung ~52 KB dau RAM; sau NVIC_SystemReset() bootloader
 * UF2 chay truoc, dung dau RAM cho bien cua no va dinh RAM cho stack, nen vung __noinit cua Zephyr (0x2000xxxx) co the
 * bi ghi de. RAM nRF52840 giu noi dung qua reset mem (khong phai mat dien).
 */
#define fault_rec (*(struct fault_record *)0x20030000UL)
static struct net_buf *fault_evt;	/* su kien cua lan chay truoc, cho host noi chuyen roi gui */

static int h4_send(struct net_buf *buf);

static FUNC_NORETURN void fault_save_and_reset(const char *file, uint32_t line)
{
	const char *name = file;
	size_t i = 0;

	if (file != NULL) {
		/* Chi giu ten file, bo duong dan */
		for (const char *c = file; *c != '\0'; c++) {
			if (*c == '/') {
				name = c + 1;
			}
		}
		for (; name[i] != '\0' && i < sizeof(fault_rec.file) - 1; i++) {
			fault_rec.file[i] = name[i];
		}
	}
	fault_rec.file[i] = '\0';
	fault_rec.line = line;
	fault_rec.magic = FAULT_MAGIC;

	NVIC_SystemReset();
	CODE_UNREACHABLE;
}
#endif /* CONFIG_BT_HCI_VS_FATAL_ERROR */

#define H4_CMD 0x01
#define H4_ACL 0x02
#define H4_SCO 0x03
#define H4_EVT 0x04
#define H4_ISO 0x05

/* Receiver states. */
#define ST_IDLE 0	/* Waiting for packet type. */
#define ST_HDR 1	/* Receiving packet header. */
#define ST_PAYLOAD 2	/* Receiving packet payload. */
#define ST_DISCARD 3	/* Dropping packet. */

/* Length of a discard/flush buffer.
 * This is sized to align with a Bluetooth HCI packet:
 * 1 byte H:4 header + 32 bytes ACL/event data
 * Bigger values might overflow the stack since this is declared as a local
 * variable, smaller ones will force the caller to call into discard more
 * often.
 */
#define H4_DISCARD_LEN 33

static int h4_read(const struct device *uart, uint8_t *buf, size_t len)
{
	int rx = uart_fifo_read(uart, buf, len);

	LOG_DBG("read %d req %d", rx, len);

	return rx;
}

static bool valid_type(uint8_t type)
{
	return (type == H4_CMD) | (type == H4_ACL) | (type == H4_ISO);
}

/* Function expects that type is validated and only CMD, ISO or ACL will be used. */
static uint32_t get_len(const uint8_t *hdr_buf, uint8_t type)
{
	switch (type) {
	case H4_CMD:
		return ((const struct bt_hci_cmd_hdr *)hdr_buf)->param_len;
	case H4_ISO:
		return bt_iso_hdr_len(
			sys_le16_to_cpu(((const struct bt_hci_iso_hdr *)hdr_buf)->len));
	case H4_ACL:
		return sys_le16_to_cpu(((const struct bt_hci_acl_hdr *)hdr_buf)->len);
	default:
		LOG_ERR("Invalid type: %u", type);
		return 0;
	}
}

/* Function expects that type is validated and only CMD, ISO or ACL will be used. */
static int hdr_len(uint8_t type)
{
	switch (type) {
	case H4_CMD:
		return sizeof(struct bt_hci_cmd_hdr);
	case H4_ISO:
		return sizeof(struct bt_hci_iso_hdr);
	case H4_ACL:
		return sizeof(struct bt_hci_acl_hdr);
	default:
		LOG_ERR("Invalid type: %u", type);
		return 0;
	}
}

/*
 * radxa-hci-iso (them vao mau goc): kep RTN cua LE Set CIG Parameters truoc khi controller nhan lenh.
 * BlueZ/PipeWire lay RTN 13 dung theo tai nghe Samsung khai bao (Max_Transport_Latency 100 ms). Voi controller Zephyr:
 * - chinh sach LOW_LATENCY: 2 tai x RTN 13 -> FT 3, NSE 5, ~8,8 ms song moi chu ky 10 ms -> Create CIS lam controller
 *   treo (TEST_REPORT F59);
 * - chinh sach RELIABILITY: FT 10, NSE 2, tre truyen 93,5 ms -> CIS chay, 200 goi/s, nhung tai nghe khong phat ra
 *   tieng (F60).
 * Kep RTN = 2 (gia tri tham so kernel radxa_cis_max_rtn=2 truoc day): LOW_LATENCY cho moi tai FT 1, NSE 3 -> 2 tai
 * ~5,3 ms moi 10 ms, tre truyen ~15 ms (1 tai RTN 13 voi FT 2 da nghe duoc, F58). RTN chi la goi y cho controller:
 * host van coi nhu RTN da xin; NSE/FT thuc te nam trong su kien CIS Established.
 */
#define RADXA_CIS_MAX_RTN 2

static void clamp_cig_rtn(struct net_buf *buf)
{
	/* Zephyr 4.x: bt_buf_get_tx() dat byte loai H4 vao dau buf->data (hci_raw.c), roi moi den header lenh */
	const struct bt_hci_cmd_hdr *hdr = (const void *)(buf->data + 1);
	struct bt_hci_cp_le_set_cig_params *cp;

	if (buf->len < 1 + sizeof(*hdr) + sizeof(*cp) || buf->data[0] != H4_CMD ||
	    sys_le16_to_cpu(hdr->opcode) != BT_HCI_OP_LE_SET_CIG_PARAMS) {
		return;
	}

	cp = (void *)(buf->data + 1 + sizeof(*hdr));
	if (buf->len < 1 + sizeof(*hdr) + sizeof(*cp) + cp->num_cis * sizeof(cp->cis[0])) {
		return;
	}

	for (uint8_t i = 0U; i < cp->num_cis; i++) {
		cp->cis[i].c_rtn = MIN(cp->cis[i].c_rtn, RADXA_CIS_MAX_RTN);
		cp->cis[i].p_rtn = MIN(cp->cis[i].p_rtn, RADXA_CIS_MAX_RTN);
	}
}

static void rx_isr(void)
{
	static struct net_buf *buf;
	static int remaining;
	static uint8_t state;
	static uint8_t type;
	static uint8_t hdr_buf[MAX(sizeof(struct bt_hci_cmd_hdr),
			sizeof(struct bt_hci_acl_hdr))];
	int read;

	do {
		switch (state) {
		case ST_IDLE:
			/* Get packet type */
			read = h4_read(hci_uart_dev, &type, sizeof(type));
			/* since we read in loop until no data is in the fifo,
			 * it is possible that read = 0.
			 */
			if (read) {
				if (valid_type(type)) {
					/* Get expected header size and switch
					 * to receiving header.
					 */
					remaining = hdr_len(type);
					state = ST_HDR;
				} else {
					LOG_WRN("Unknown header %d", type);
				}
			}
			break;
		case ST_HDR:
			read = h4_read(hci_uart_dev,
				       &hdr_buf[hdr_len(type) - remaining],
				       remaining);
			remaining -= read;
			if (remaining == 0) {
				/* Header received. Allocate buffer and get
				 * payload length. If allocation fails leave
				 * interrupt. On failed allocation state machine
				 * is reset.
				 */
				buf = bt_buf_get_tx(bt_buf_type_from_h4(type, BT_BUF_OUT),
						    K_NO_WAIT, NULL, 0);
				if (!buf) {
					LOG_ERR("No available command buffers!");
					state = ST_IDLE;
					return;
				}

				remaining = get_len(hdr_buf, type);

				net_buf_add_mem(buf, hdr_buf, hdr_len(type));
				if (remaining > net_buf_tailroom(buf)) {
					LOG_ERR("Not enough space in buffer");
					net_buf_unref(buf);
					state = ST_DISCARD;
				} else {
					state = ST_PAYLOAD;
				}

			}
			break;
		case ST_PAYLOAD:
			read = h4_read(hci_uart_dev, net_buf_tail(buf),
				       remaining);
			buf->len += read;
			remaining -= read;
			if (remaining == 0) {
				/* Packet received */
				LOG_DBG("putting RX packet in queue.");
				clamp_cig_rtn(buf);
				k_fifo_put(&tx_queue, buf);
				state = ST_IDLE;
			}
			break;
		case ST_DISCARD:
		{
			uint8_t discard[H4_DISCARD_LEN];
			size_t to_read = MIN(remaining, sizeof(discard));

			read = h4_read(hci_uart_dev, discard, to_read);
			remaining -= read;
			if (remaining == 0) {
				state = ST_IDLE;
			}

			break;

		}
		default:
			read = 0;
			__ASSERT_NO_MSG(0);
			break;

		}
	} while (read);
}

static void tx_isr(void)
{
	static struct net_buf *buf;
	int len;

	if (!buf) {
		buf = k_fifo_get(&uart_tx_queue, K_NO_WAIT);
		if (!buf) {
			uart_irq_tx_disable(hci_uart_dev);
			return;
		}
	}

	len = uart_fifo_fill(hci_uart_dev, buf->data, buf->len);
	net_buf_pull(buf, len);
	if (!buf->len) {
		net_buf_unref(buf);
		buf = NULL;
	}
}

static void bt_uart_isr(const struct device *unused, void *user_data)
{
	ARG_UNUSED(unused);
	ARG_UNUSED(user_data);

	while (uart_irq_update(hci_uart_dev) && uart_irq_is_pending(hci_uart_dev)) {
		if (!(uart_irq_rx_ready(hci_uart_dev) ||
		      uart_irq_tx_ready(hci_uart_dev))) {
			LOG_DBG("spurious interrupt");
		}

		if (uart_irq_tx_ready(hci_uart_dev)) {
			tx_isr();
		}

		if (uart_irq_rx_ready(hci_uart_dev)) {
			rx_isr();
		}
	}
}

#if defined(CONFIG_BT_CTLR_CONN_ISO)
/* Khai bao trong subsys/bluetooth/controller/include/ll.h (khong nam trong duong include cua ung dung) */
uint8_t ll_remove_iso_path(uint16_t handle, uint8_t path_dir);

/*
 * radxa-hci-iso (them vao mau goc): truoc LE Setup ISO Data Path, xoa duong du lieu cu cua dung handle + chieu do.
 * Zephyr chi xoa duong du lieu khi CIS ngat o vai PERIPHERAL (ull_conn_iso.c); o vai CENTRAL duong du lieu cu con nam
 * trong CIS. Kernel Linux coi duong du lieu mat theo CIS nen lan dung CIS sau gui lai Setup ISO Data Path -> Zephyr tra
 * Command Disallowed -> BlueZ "Device or resource busy", PipeWire Acquire loi, khong co tieng tu lan phat thu 2
 * (TEST_REPORT F61). Chay trong luong tx_thread, cung ngu canh voi xu ly lenh HCI cua controller (bt_send).
 */
static void drop_stale_iso_path(struct net_buf *buf)
{
	const struct bt_hci_cmd_hdr *hdr = (const void *)(buf->data + 1);	/* data[0] = loai H4 */
	const struct bt_hci_cp_le_setup_iso_path *cp;

	if (buf->len < 1 + sizeof(*hdr) + sizeof(*cp) || buf->data[0] != H4_CMD ||
	    sys_le16_to_cpu(hdr->opcode) != BT_HCI_OP_LE_SETUP_ISO_PATH) {
		return;
	}

	cp = (const void *)(buf->data + 1 + sizeof(*hdr));
	if (cp->path_dir <= BT_HCI_DATAPATH_DIR_CTLR_TO_HOST) {
		/* Khong co duong du lieu thi ham tra loi, bo qua */
		(void)ll_remove_iso_path(sys_le16_to_cpu(cp->handle), BIT(cp->path_dir));
	}
}
#endif /* CONFIG_BT_CTLR_CONN_ISO */

static void tx_thread(void *p1, void *p2, void *p3)
{
	while (1) {
		struct net_buf *buf;
		int err;

		/* Wait until a buffer is available */
		buf = k_fifo_get(&tx_queue, K_FOREVER);
#if defined(CONFIG_BT_CTLR_CONN_ISO)
		drop_stale_iso_path(buf);
#endif
		/* Pass buffer to the stack */
		err = bt_send(buf);
		if (err) {
			LOG_ERR("Unable to send (err %d)", err);
			net_buf_unref(buf);
		}

#if defined(CONFIG_BT_HCI_VS_FATAL_ERROR)
		/* radxa-hci-iso: host da noi chuyen (goi dau tien) -> gui bao loi cua lan chay truoc */
		if (fault_evt != NULL) {
			struct net_buf *evt = fault_evt;

			fault_evt = NULL;
			(void)h4_send(evt);
		}
#endif

		/* Give other threads a chance to run if tx_queue keeps getting
		 * new data all the time.
		 */
		k_yield();
	}
}

static int h4_send(struct net_buf *buf)
{
	LOG_DBG("buf %p type %u len %u", buf, buf->data[0], buf->len);

	k_fifo_put(&uart_tx_queue, buf);
	uart_irq_tx_enable(hci_uart_dev);

	return 0;
}

#if defined(CONFIG_BT_CTLR_ASSERT_HANDLER)
void bt_ctlr_assert_handle(char *file, uint32_t line)
{
	/* Disable interrupts, this is unrecoverable */
	(void)irq_lock();

#if defined(CONFIG_BT_HCI_VS_FATAL_ERROR)
	/* radxa-hci-iso: khong gui duoc qua CDC-ACM khi da khoa ngat -> luu lai, reset, bao o lan khoi dong sau */
	fault_save_and_reset(file, line);
#else
	LOG_ERR("Controller assert in: %s at %d", file, line);

	/* Flush the logs before locking the CPU */
	LOG_PANIC();

	while (true) {
		k_cpu_idle();
	};
#endif

	CODE_UNREACHABLE;
}
#endif /* CONFIG_BT_CTLR_ASSERT_HANDLER */

#if defined(CONFIG_BT_HCI_VS_FATAL_ERROR)
void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
	char name[] = "fatal-reason-0";

	/* Disable interrupts, this is unrecoverable */
	(void)irq_lock();

	/* radxa-hci-iso: nhu assert, luu ly do + PC roi reset (K_ERR_* chi co 1 chu so) */
	name[sizeof(name) - 2] = (char)('0' + (reason % 10U));
	fault_save_and_reset(name, (esf != NULL) ? esf->basic.pc : 0U);
}
#endif /* CONFIG_BT_HCI_VS_FATAL_ERROR */

static int hci_uart_init(void)
{
	LOG_DBG("");

	if (!device_is_ready(hci_uart_dev)) {
		LOG_ERR("HCI UART %s is not ready", hci_uart_dev->name);
		return -EINVAL;
	}

	uart_irq_rx_disable(hci_uart_dev);
	uart_irq_tx_disable(hci_uart_dev);

	uart_irq_callback_set(hci_uart_dev, bt_uart_isr);

	uart_irq_rx_enable(hci_uart_dev);

	return 0;
}

SYS_INIT(hci_uart_init, APPLICATION, CONFIG_KERNEL_INIT_PRIORITY_DEVICE);

int main(void)
{
	/* incoming events and data from the controller */
	static K_FIFO_DEFINE(rx_queue);
	int err;

	LOG_DBG("Start");
	__ASSERT(hci_uart_dev, "UART device is NULL");

	/* Enable the raw interface, this will in turn open the HCI driver */
	bt_enable_raw(&rx_queue);

#if defined(CONFIG_BT_HCI_VS_FATAL_ERROR)
	/* radxa-hci-iso: lan chay truoc bi assert/fault -> dung su kien bao loi, gui khi host bat dau noi chuyen */
	if (fault_rec.magic == FAULT_MAGIC) {
		fault_rec.magic = 0U;
		fault_rec.file[sizeof(fault_rec.file) - 1] = '\0';
		fault_evt = hci_vs_err_assert(fault_rec.file, fault_rec.line);
	}
#endif

	if (IS_ENABLED(CONFIG_BT_WAIT_NOP)) {
		/* Issue a Command Complete with NOP */
		int i;

		const struct {
			const uint8_t h4;
			const struct bt_hci_evt_hdr hdr;
			const struct bt_hci_evt_cmd_complete cc;
		} __packed cc_evt = {
			.h4 = H4_EVT,
			.hdr = {
				.evt = BT_HCI_EVT_CMD_COMPLETE,
				.len = sizeof(struct bt_hci_evt_cmd_complete),
			},
			.cc = {
				.ncmd = 1,
				.opcode = sys_cpu_to_le16(BT_OP_NOP),
			},
		};

		for (i = 0; i < sizeof(cc_evt); i++) {
			uart_poll_out(hci_uart_dev,
				      *(((const uint8_t *)&cc_evt)+i));
		}
	}

	/* Spawn the TX thread and start feeding commands and data to the
	 * controller
	 */
	k_thread_create(&tx_thread_data, tx_thread_stack,
			K_THREAD_STACK_SIZEOF(tx_thread_stack), tx_thread,
			NULL, NULL, NULL, K_PRIO_COOP(7), 0, K_NO_WAIT);
	k_thread_name_set(&tx_thread_data, "HCI uart TX");

	while (1) {
		struct net_buf *buf;

		buf = k_fifo_get(&rx_queue, K_FOREVER);
		err = h4_send(buf);
		if (err) {
			LOG_ERR("Failed to send");
		}
	}
	return 0;
}

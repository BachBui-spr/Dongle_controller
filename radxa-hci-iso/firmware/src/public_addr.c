/*
 * radxa-hci-iso: dat dia chi PUBLIC cho controller truoc khi bat Bluetooth (dung cho bien the "lean").
 *
 * Ly do (TEST_REPORT F57): khong co dia chi public thi kernel lay dia chi tinh (LE Random) lam danh tinh cua hci0.
 * BlueZ 5.72 (profiles/audio/bap.c) mo ISO socket cho CIS ma khong truyen kieu dia chi nguon; btio mac dinh LE Public
 * -> kernel hci_get_route() thay dia chi dung nhung kieu sai -> "No route to host" -> BlueZ huy ASE ngay sau Codec
 * Configuration, PipeWire khong co node bluez_output. Chip AIC co dia chi public nen khong gap. (BlueZ 5.87 da truyen
 * BT_IO_OPT_SOURCE_TYPE.) Co dia chi public thi danh tinh cua hci0 la LE Public, khop voi btio.
 *
 * Dia chi: 48 bit duy nhat cua chip (FICR DEVICEADDR, cung nguon voi dia chi tinh truoc day), octet cao nhat xoa bit
 * multicast va dat bit "locally administered" -> khong trung OUI cua hang nao, co dinh qua moi lan nap lai.
 * Goi truoc bt_enable_raw() trong main() (muc APPLICATION chay truoc main); HCI Reset khong xoa dia chi nay.
 */
#include <zephyr/init.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/bluetooth/controller.h>
#include <soc.h>

static int radxa_public_addr_init(void)
{
	uint8_t addr[6];

	sys_put_le32(NRF_FICR->DEVICEADDR[0], &addr[0]);
	sys_put_le16(NRF_FICR->DEVICEADDR[1] & 0xFFFF, &addr[4]);
	addr[5] = (addr[5] & 0xFC) | 0x02;

	bt_ctlr_set_public_addr(addr);

	return 0;
}

SYS_INIT(radxa_public_addr_init, APPLICATION, 0);

#include <zephyr.h>
#include <sys/printk.h>
#include <sys/util.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>

static bool parse_ad_data(struct bt_data *data, void *user_data)
{
	if (data->type == BT_DATA_MANUFACTURER_DATA) {
		printk("Manufacturer data received, length: %u\n", data->data_len);

		if (data->data_len >= 3) {
			uint16_t company_id = data->data[0] | (data->data[1] << 8);
			uint8_t counter = data->data[2];

			printk("Company ID: 0x%04X, Counter: 0x%02X\n",
			       company_id, counter);
		} else {
			printk("Manufacturer data too short\n");
		}
	}

	return true;
}

static void device_found(const bt_addr_le_t *addr, int8_t rssi,
			 uint8_t type, struct net_buf_simple *ad)
{
	char addr_str[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(addr, addr_str, sizeof(addr_str));

	printk("Device found: %s, RSSI: %d, type: %u\n",
	       addr_str, rssi, type);

	bt_data_parse(ad, parse_ad_data, NULL);
}

int main(void)
{
	int err;

	printk("Starting Observer\n");

	err = bt_enable(NULL);
	if (err) {
		printk("Bluetooth init failed (err %d)\n", err);
		return 0;
	}

	printk("Bluetooth initialized\n");

	err = bt_le_scan_start(BT_LE_SCAN_PASSIVE, device_found);
	if (err) {
		printk("Scanning failed to start (err %d)\n", err);
		return 0;
	}

	printk("Scanning started\n");

	while (1) {
		k_msleep(1000);
	}

	return 0;
}
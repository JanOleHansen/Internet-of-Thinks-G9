#include <zephyr.h>
#include <zephyr/types.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <sys/printk.h>
#include <sys/byteorder.h>
#include <kernel.h>

#include <settings/settings.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/conn.h>
#include <bluetooth/uuid.h>
#include <bluetooth/gatt.h>
#include <bluetooth/services/bas.h>
//#include <bluetooth/services/cts.h>
#include <bluetooth/services/hrs.h>
//#include <bluetooth/services/ias.h>
#include <bluetooth/gatt.h>


/* Custom Service Variables */
// Define the custom service UUID (128-bit) 12345678-1234-5678-1234-56789abcdef0
#define BT_UUID_HELLO_PRIMARY_VAL \
	BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x1234, 0x56789abcdef0)
#define BT_UUID_SENSOR_PRIMARY_VAL \
    BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x1234, 0x56789abcdef3)

#define VND_MAX_LEN 20

// vnd_value is the value of the custom characteristic, initialized with "Hello World"
static uint8_t hello1_value[VND_MAX_LEN + 1] = { 'H', 'e', 'l', 'l', 'o', ' ', 
    'W', 'o', 'r', 'l', 'd'};

static uint8_t hello2_value[VND_MAX_LEN + 1] = { 'Z', 'e', 'p', 'h', 'y', 'r', ' ', 
    'R', 'T', 'O', 'S'};

static uint8_t sensor_value[1] = {42}; // Example sensor value
// Custom Characteristic UUID
static struct bt_uuid_128 hello1_uuid = BT_UUID_INIT_128(
	BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x1234, 0x56789abcdef1));

static struct bt_uuid_128 hello2_uuid = BT_UUID_INIT_128(
	BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x1234, 0x56789abcdef2));

static struct bt_uuid_128 sensor_uuid = BT_UUID_INIT_128( 
    BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x1234, 0x56789abcdef4));

// Advertisement data: Send name and flags 
static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME, sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

// Callback function for reading the custom characteristic value
static ssize_t read_vnd(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			void *buf, uint16_t len, uint16_t offset)
{
	// The value of the characteristic is stored in the user_data field of the attribute
    const char *value = attr->user_data;
    // Helper function to read the characteristic value and return it in the buffer
    // provided by the caller
	return bt_gatt_attr_read(conn, attr, buf, len, offset, value,
				 strlen(value));
}

static ssize_t read_sensor(struct bt_conn *conn, const struct bt_gatt_attr *attr,
            void *buf, uint16_t len, uint16_t offset)
{
    const uint8_t *value = attr->user_data;
    return bt_gatt_attr_read(conn, attr, buf, len, offset, value,
                 sizeof(sensor_value));
}

// Vendor Primary Service Declaration
BT_GATT_SERVICE_DEFINE(hello_svc,
    BT_GATT_PRIMARY_SERVICE(BT_UUID_DECLARE_128(BT_UUID_HELLO_PRIMARY_VAL)),
    // Characteristic belongs to the primary service, has read property and permission, 
    // and uses read_vnd as the read callback function
    BT_GATT_CHARACTERISTIC(&hello1_uuid.uuid, BT_GATT_CHRC_READ, BT_GATT_PERM_READ, 
        read_vnd, NULL, hello1_value),
    BT_GATT_CHARACTERISTIC(&hello2_uuid.uuid, BT_GATT_CHRC_READ, BT_GATT_PERM_READ,
        read_vnd, NULL, hello2_value),
);

BT_GATT_SERVICE_DEFINE(sensor_svc,
    BT_GATT_PRIMARY_SERVICE(BT_UUID_DECLARE_128(BT_UUID_SENSOR_PRIMARY_VAL)),
    // Characteristic belongs to the primary service, has read property and permission, 
    // and uses read_vnd as the read callback function
    BT_GATT_CHARACTERISTIC(&sensor_uuid.uuid, BT_GATT_CHRC_READ, BT_GATT_PERM_READ,
        read_sensor, NULL, sensor_value),
);

// Callback function for connection events
static void connected(struct bt_conn *conn, uint8_t err)
{
    if (err) {
        printk("Connection failed (err %u)\n", err);
        return;
    }

    printk("Central connected\n");
}

// Callback function for disconnection events
static void disconnected(struct bt_conn *conn, uint8_t reason)
{
    printk("Central disconnected (reason %u)\n", reason);
}

// Define connection callbacks
static struct bt_conn_cb conn_callbacks = {
    .connected = connected,
    .disconnected = disconnected,
};


// Function if the Bluetooth stack is ready to use
static void bt_ready(void)
{
	int err;
    // Start advertising with connectivity
	err = bt_le_adv_start(BT_LE_ADV_CONN, ad, ARRAY_SIZE(ad), NULL, 0);
	if (err) {
		printk("Advertising failed to start (err %d)\n", err);
		return;
	}

	printk("Advertising successfully started\n");
}


int main(void)
{    
    int err;
    // Initialize the Bluetooth Subsystem (synchronous)
    err = bt_enable(NULL);
    if (err) {
        printk("Bluetooth init failed (err %d)\n", err);
        return 0;
    }
    printk("Bluetooth initialized\n");
	bt_conn_cb_register(&conn_callbacks);
    bt_ready();
    while (1) {
		k_msleep(1000);
	}   
    return 0;
}

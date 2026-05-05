#include <zephyr.h>
#include <zephyr/types.h>
#include <stddef.h>
#include <errno.h>
#include <kernel.h>
#include <sys/printk.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/conn.h>
#include <bluetooth/uuid.h>
#include <bluetooth/gatt.h>
#include <bluetooth/addr.h>
#include <sys/byteorder.h>

#define MAX_CHRCS 5
/*
#define BT_UUID_CUSTOM_SERVICE_VAL \
    BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x1234, 0x56789abcdef0)

static struct bt_uuid_128 discover_uuid =
    BT_UUID_INIT_128(BT_UUID_CUSTOM_SERVICE_VAL);
*/
static uint16_t chrc_handles[MAX_CHRCS];
static uint8_t chrc_count;
static uint8_t read_index;

static bool peripheral_found = false;
static bt_addr_le_t peripheral_addr;
static struct bt_uuid_128 characteristic_uuid;
static struct bt_uuid_128 discover_uuid;
static struct bt_conn *default_conn;
static bool connecting = false;

// Define GATT Discover Attribute Parameters
static struct bt_gatt_discover_params discover_params;

// Define GATT Read Parameters
static struct bt_gatt_read_params read_params;

// Define Read Callback Function
static uint8_t read_characteristics(struct bt_conn *conn, uint8_t err,
                    struct bt_gatt_read_params *params,
                    const void *data, uint16_t length);


// Function to read all characteristics of a service one by one
static void read_next_characteristic(struct bt_conn *conn)
{
    if (read_index >= chrc_count) {
        printk("All characteristics read\n");
        return;
    }

    read_params.func = read_characteristics;
    read_params.handle_count = 1;
    read_params.single.handle = chrc_handles[read_index];
    read_params.single.offset = 0;

    bt_gatt_read(conn, &read_params);
}
// Define GATT Read Callback Function
static uint8_t read_characteristics(struct bt_conn *conn, uint8_t err,
                    struct bt_gatt_read_params *params,
                    const void *data, uint16_t length) {
    if (err) {
        printk("Read failed (err %d)\n", err);
        return BT_GATT_ITER_STOP;
    }
    if (!data) {
        printk("Read complete\n");
        read_index++;
        read_next_characteristic(conn);
        return BT_GATT_ITER_STOP;
    }
    // Print the read value in a readable format
    printk("Read characteristic value: ");
    printk("%.*s ", length,((uint8_t *)data));
    printk("\n");
    return BT_GATT_ITER_CONTINUE;
}

// Define GATT Discover Function
static uint8_t discover_func(struct bt_conn *conn,
			     const struct bt_gatt_attr *attr,
			     struct bt_gatt_discover_params *params)
{
	int err;

	if (!attr) {
	    if (params->type == BT_GATT_DISCOVER_PRIMARY) {
            printk("Primary service discovery complete\n");
        } else if (params->type == BT_GATT_DISCOVER_CHARACTERISTIC) {
            printk("Characteristic discovery complete, found %u characteristics\n", chrc_count);
            read_index = 0;
            read_next_characteristic(conn);
        }
        return BT_GATT_ITER_STOP;
	}
    // Check if Primary Service is found
    if (params->type == BT_GATT_DISCOVER_PRIMARY) {
        printk("Primary Service found at handle %u\n", attr->handle);
        // Update Discover Parameters to search for Characteristics within the discovered Primary Service
        struct bt_gatt_service_val *service = attr->user_data;
        discover_params.type = BT_GATT_DISCOVER_CHARACTERISTIC;
        discover_params.start_handle = attr->handle + 1;
        discover_params.end_handle = service->end_handle;
        discover_params.uuid = NULL; // Search for all characteristics within the service
        chrc_count = 0;
        err = bt_gatt_discover(conn, &discover_params);
        if (err) {
            printk("Discover failed(err %d)\n", err);
        } else {
            printk("Discover of characteristic started successfully\n");
        }
    } else if (params->type == BT_GATT_DISCOVER_CHARACTERISTIC) {
        struct bt_gatt_chrc *chrc = attr->user_data;
        printk("Characteristic found at handle %u\n", chrc->value_handle);
        if (chrc_count < MAX_CHRCS) {
        chrc_handles[chrc_count++] = chrc->value_handle;
    }

    return BT_GATT_ITER_CONTINUE;
    }
    return BT_GATT_ITER_STOP;
}

// Callback function to handle connection events
static void connected(struct bt_conn *conn, uint8_t err)
{
    if (err) {
        printk("Connection failed (err %u)\n", err);
        return;
    }

    printk("Connected\n");
    // Initialize GATT Discover Parameters => Read from Primary Service
    discover_params.uuid = &discover_uuid.uuid;
    discover_params.func = discover_func;
    discover_params.type = BT_GATT_DISCOVER_PRIMARY;
    discover_params.start_handle = 0x0001;
    discover_params.end_handle = 0xffff;
    // Search for Primary Service with the discovered UUID
    int error = bt_gatt_discover(conn, &discover_params);
    if (error) {
        printk("Discover failed(err %d)\n", error);
    } else {
        printk("Discover started successfully\n");
    }

}

// Callback function to handle disconnection events
static void disconnected(struct bt_conn *conn, uint8_t reason)
{
    printk("Disconnected (reason %u)\n", reason);

    if (default_conn) {
        bt_conn_unref(default_conn);
        default_conn = NULL;
    }
}

// Define connection callbacks
static struct bt_conn_cb conn_callbacks = {
    .connected = connected,
    .disconnected = disconnected,
};

// Callback function to parse and print received advertising data and scan response data
bool parse_adv_data(struct bt_data *data, void *user_data) {
    // Print data in readable format
    // Check if scan response data is received
    if (data->type == BT_DATA_NAME_COMPLETE) {
        printk("Device Name: %.*s\n", data->data_len, data->data);
        // Check if the device name matches "peripheral"
        if (strncmp((char *)data->data, "peripheral", data->data_len) == 0) {
            printk("Found target device: %.*s\n", data->data_len, data->data);
            peripheral_found = true;
        }
    } else if (data->type == BT_DATA_UUID128_ALL) {
        if (data->data_len == 16) {
        discover_uuid.uuid.type = BT_UUID_TYPE_128;
        memcpy(discover_uuid.val, data->data, 16);
        char uuid_str[BT_UUID_STR_LEN];
        bt_uuid_to_str(&discover_uuid.uuid, uuid_str, sizeof(uuid_str));
        printk("Parsed Service UUID: %s\n", uuid_str);
        } else {
            printk("Unexpected UUID length: %u\n", data->data_len);
            return true;
        }
    } else if (data->type == BT_DATA_FLAGS){
        printk("Flags: 0x%02X\n", data->data[0]);
    } else {
        printk("Data type: 0x%02X, Data length: %d, Data: ", data->type, data->data_len);
        for (int i = 0; i < data->data_len; i++) {
            printk("%02X ", data->data[i]);
        }
        printk("\n");
    }
    return true;
}

// Callback function to handle received advertising data and scan response data
static void scan_callback(
        const bt_addr_le_t *addr, 
        int8_t rssi, 
        uint8_t adv_type, 
        struct net_buf_simple *buf) {
            char str[BT_UUID_STR_LEN];
            bt_addr_le_to_str(addr, str, sizeof(str));
            if (adv_type == BT_GAP_ADV_TYPE_SCAN_RSP) {
                printk("Received scan response from: %s (RSSI: %d dBm)\n", str, rssi);
            } else {
                printk("Received advertising data from: %s (RSSI: %d dBm)\n", str, rssi);
            }
            bt_data_parse(buf, parse_adv_data, NULL);
            if (peripheral_found && !connecting) {
                bt_addr_le_copy(&peripheral_addr, addr);
                printk("Stopping scan since target device is found: %s\n", bt_addr_le_to_str(&peripheral_addr, str, sizeof(str)));
                bt_le_scan_stop();
                connecting = true;
                int err = bt_conn_le_create(&peripheral_addr, BT_CONN_LE_CREATE_CONN, BT_LE_CONN_PARAM_DEFAULT, &default_conn);
                if (err) {
                    printk("Connection failed (err %d)\n", err);
                    connecting = false; 
                } else {
                    printk("Connection initiated successfully\n");
                }
            }

};

static void start_scan(void){
    // Start scanning for Bluetooth devices
    int err = bt_le_scan_start(BT_LE_SCAN_ACTIVE, scan_callback);
    if (err) {
        printk("Scanning failed to start (err %d)\n", err);
        return;
    }
    return;
};

int main(void)
{
    // Initialize the Bluetooth Subsystem (synchronous)
    int err = bt_enable(NULL);
    if (err) {
        printk("Bluetooth init failed (err %d)\n", err);
        return 0;
    }
    printk("Bluetooth initialized\n");
    bt_conn_cb_register(&conn_callbacks);
    start_scan();
    while (1) {
        k_msleep(1000);
    }
    return 0;
}

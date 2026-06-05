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

#define MAX_CHRCS 2
#define MAX_SERVICES 2
static uint16_t chrc_hello_handles[MAX_CHRCS];
static uint16_t chrc_sensor_handles[MAX_CHRCS];
static uint8_t chrc_hello_count = 0;
static uint8_t chrc_sensor_count = 0;
static uint8_t read_index = 0;

static bool peripheral_found = false;
static bt_addr_le_t peripheral_addr;
static struct bt_uuid_128 sensor_uuid = BT_UUID_INIT_128( 
    BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x1234, 0x56789abcdef3)); ;
static struct bt_uuid_128 hello_uuid = BT_UUID_INIT_128( 
    BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x1234, 0x56789abcdef0));
static bool hello_service_found = false;
static bool sensor_service_found = false;
static struct bt_conn *default_conn;
static bool connecting = false;

struct service_range {
    uint16_t start;
    uint16_t end;
};
static struct service_range service_ranges[MAX_SERVICES];
static uint8_t service_index;
static bool first_call = true;

// Define GATT Discover Attribute Parameters
static struct bt_gatt_discover_params discover_params;

// Define GATT Read Parameters
static struct bt_gatt_read_params read_params;

// Define Read Callback Function
static uint8_t read_characteristics(struct bt_conn *conn, uint8_t err,
                    struct bt_gatt_read_params *params,
                    const void *data, uint16_t length);

// Define GATT Discover Function
static uint8_t discover_func(struct bt_conn *conn,
                             const struct bt_gatt_attr *attr,
                             struct bt_gatt_discover_params *params);

static void discover_next_service_characteristics(struct bt_conn *conn);

// Function to read all characteristics of a service one by one
static void read_next_characteristic(struct bt_conn *conn)
{
    if (service_index == 0) {
        // Hello service is found, read its characteristics
        if (read_index >= chrc_hello_count) {
            printk("All characteristics of the hello service read\n");
            service_index++;
            discover_next_service_characteristics(conn);
            return;
        }
        read_params.func = read_characteristics;
        read_params.handle_count = 1;
        read_params.single.handle = chrc_hello_handles[read_index];
        read_params.single.offset = 0;
        int err = bt_gatt_read(conn, &read_params);
        if (err) {
            printk("Read failed (err %d)\n", err);
        } else {
            printk("Read started successfully\n");
        }

    // Sensor service is found, read its characteristics    
    } else if (sensor_service_found) {
        if (read_index >= chrc_sensor_count) {
            printk("All characteristics of the sensor service read\n");
            return;
        }
        read_params.func = read_characteristics;
        read_params.handle_count = 1;
        read_params.single.handle = chrc_sensor_handles[read_index];
        read_params.single.offset = 0;
        int err = bt_gatt_read(conn, &read_params);
        if (err) {
            printk("Read failed (err %d)\n", err);
        } else {
            printk("Read started successfully\n");
        }

    } else {
        printk("Invalid service index\n");
        return;
    }
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
         // Read complete for the current characteristic, move to the next one
        printk("Read complete\n");
        read_index++;
        read_next_characteristic(conn);
        return BT_GATT_ITER_STOP;
    }
    // Print the read value in a readable format
    printk("Read characteristic value: ");
    if (service_index == 0){
        printk("%.*s ", length,((uint8_t *)data));
        printk("\n");   
    } else {
        for (int i = 0; i < length; i++) {
            printk("%d Celsius", ((uint8_t *)data)[i]);
        }
        printk("\n");
    }
    return BT_GATT_ITER_CONTINUE;
}

static void discover_next_service_characteristics(struct bt_conn *conn) {
    if (service_index >= MAX_SERVICES) {
        printk("All services discovered\n");
        return;
    }
    
    if (hello_service_found && first_call){
        service_index = 0;
        first_call = false;
    } else if (sensor_service_found) {
        service_index = 1;
    }
    discover_params.func = discover_func;
    discover_params.type = BT_GATT_DISCOVER_CHARACTERISTIC;
    discover_params.start_handle = service_ranges[service_index].start;
    discover_params.end_handle = service_ranges[service_index].end;
    int err = bt_gatt_discover(conn, &discover_params);
    if (err) {
        printk("Discover failed(err %d)\n", err);
    } else {
        printk("Discover of characteristics started successfully\n");
    }
}

// Define GATT Discover Function
static uint8_t discover_func(struct bt_conn *conn,
			     const struct bt_gatt_attr *attr,
			     struct bt_gatt_discover_params *params)
{
	int err;

	if (!attr) {
        if (params->type == BT_GATT_DISCOVER_PRIMARY) {
            // All services are found, start reading characteristics of the discovered service if the target service is found
            printk("Primary service discovery completed\n");
            discover_next_service_characteristics(conn);
            return BT_GATT_ITER_STOP;
        } else if (params->type == BT_GATT_DISCOVER_CHARACTERISTIC) {
            // All characteristics of the current service are found, start reading them one by one
            read_index = 0;
            read_next_characteristic(conn);
            return BT_GATT_ITER_STOP;
        
        }
    }
    // Check if Primary Service is found
    if (params->type == BT_GATT_DISCOVER_PRIMARY) {
        printk("Primary Service found at handle %u\n", attr->handle);        
        // UUID matches the expected hello service UUID or sensor service UUID
        // Save range for the search of characteristics
        struct bt_gatt_service_val *service = attr->user_data;
        if (bt_uuid_cmp(service->uuid, &hello_uuid.uuid) == 0) {
            hello_service_found = true;
            printk("Discovered service is the hello service\n");
            service_ranges[0].start = attr->handle + 1;
            service_ranges[0].end = service->end_handle;
        } else if (bt_uuid_cmp(service->uuid, &sensor_uuid.uuid) == 0) {
            sensor_service_found = true;
            printk("Discovered service is the sensor service\n");
            service_ranges[1].start = attr->handle + 1;
            service_ranges[1].end = service->end_handle;
        } else {
            printk("Discovered service does not match the target service UUID\n");
        }
    // Check if Characteristic is found
    } else if (params->type == BT_GATT_DISCOVER_CHARACTERISTIC) {
        // Save the characteristic handle for later reading
        if (service_index == 0){
            struct bt_gatt_chrc *chrc_hello = attr->user_data;
            chrc_hello_handles[chrc_hello_count++] = chrc_hello->value_handle;
            return BT_GATT_ITER_CONTINUE;
        } else if (service_index == 1) {
            struct bt_gatt_chrc *chrc_sensor = attr->user_data;
            chrc_sensor_handles[chrc_sensor_count++] = chrc_sensor->value_handle;
            return BT_GATT_ITER_CONTINUE;
        } else {
            printk("Invalid service index\n");
            return BT_GATT_ITER_STOP;
        }
    };
    return BT_GATT_ITER_CONTINUE;
}

// Callback function to handle connection events
static void connected(struct bt_conn *conn, uint8_t err)
{
    if (err) {
        printk("Connection failed (err %u)\n", err);
        return;
    }

    printk("Connected\n");
    // Initialize GATT Discover Parameters to search for Services
    discover_params.uuid = NULL; // Search for all services
    discover_params.func = discover_func;
    discover_params.type = BT_GATT_DISCOVER_PRIMARY;
    discover_params.start_handle = 0x0001;
    discover_params.end_handle = 0xffff;
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
// Print data in readable format
// Check if the device name matches "peripheral"
bool parse_adv_data(struct bt_data *data, void *user_data) {
    if (data->type == BT_DATA_NAME_COMPLETE) {
        printk("Device Name: %.*s\n", data->data_len, data->data);
        if (strncmp((char *)data->data, "peripheral", data->data_len) == 0) {
            printk("Found target device: %.*s\n", data->data_len, data->data);
            peripheral_found = true;
        }
    } else if (data->type == BT_DATA_FLAGS){
        printk("Flags: 0x%02X\n", data->data[0]);
    } else {
        printk("Received irrelevant advertising data type: 0x%02X\n", data->type);
    }
    return true;
}

// Callback function to handle received advertising data and scan response data
static void scan_callback(const bt_addr_le_t *addr, int8_t rssi, 
    uint8_t adv_type, struct net_buf_simple *buf) {
            char str[BT_UUID_STR_LEN];
            bt_addr_le_to_str(addr, str, sizeof(str));
            if (adv_type == BT_GAP_ADV_TYPE_ADV_IND) {
                // Received advertising data, print it in a readable format
                printk("Received advertising data from: %s (RSSI: %d dBm)\n", str, rssi);
                bt_data_parse(buf, parse_adv_data, NULL);
                if (peripheral_found && !connecting) {
                    bt_addr_le_copy(&peripheral_addr, addr);
                    char str1[BT_ADDR_LE_STR_LEN];
                    printk("Stopping scan since target device is found: %s\n", 
                        bt_addr_le_to_str(&peripheral_addr, str1, sizeof(str1)));
                    bt_le_scan_stop();
                    connecting = true;
                    // Connect to the discovered peripheral device
                    int err = bt_conn_le_create(&peripheral_addr, BT_CONN_LE_CREATE_CONN, 
                        BT_LE_CONN_PARAM_DEFAULT, &default_conn);
                    if (err) {
                        printk("Connection failed (err %d)\n", err);
                        connecting = false; 
                    } else {
                        printk("Connection initiated successfully\n");
                    }
                }
            } else {
                printk("Received irrelevant advertising event from: %s (RSSI: %d dBm), type: 0x%02X\n", str, rssi, adv_type);
            }
};

static void start_scan(void){
    // Start scanning for Bluetooth devices
    int err = bt_le_scan_start(BT_LE_SCAN_PASSIVE, scan_callback);
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

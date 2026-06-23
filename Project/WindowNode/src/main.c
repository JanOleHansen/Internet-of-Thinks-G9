#include <zephyr.h>
#include <device.h>
#include <devicetree.h>
#include <drivers/gpio.h>
#include <sys/printk.h>
#include <stdint.h>
#include <errno.h>
#include <drivers/pwm.h>
#include <bluetooth/addr.h>
#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/conn.h>
#include <bluetooth/uuid.h>
#include <bluetooth/gatt.h>
#include <bluetooth/services/bas.h>
#include <bluetooth/services/hrs.h>

// WindowNode reads Data from Magnet Sensor

#define WIN_NODE 2
// Here are the MSG_TYPES
#define SENSOR_DATA 1
#define COMMAND 2
#define ACK 3
#define HEARTBEAT 4
#define ERROR 5

// Here are the SENSOR_TYPES
#define TEMP_SENSOR 1
#define HUM_SENSOR 2
#define LIGHT_SENSOR 3
#define MOTION_SENSOR 4
#define WINDOW_SENSOR 5

uint16_t seq_num = 0;

// Because we dont have a Magnet Sensor yet, we simulate the Sensor via Button Press
#define BUTTON0_NODE DT_ALIAS(sw0)
#if DT_NODE_HAS_STATUS(BUTTON0_NODE, okay)
#define BUTTON0	DT_GPIO_LABEL(BUTTON0_NODE, gpios)
#define BUTTON_PIN DT_GPIO_PIN(BUTTON0_NODE, gpios)
#define BUTTON_FLAGS DT_GPIO_FLAGS(BUTTON0_NODE, gpios)
#else
#error "Unsupported board: button0 devicetree alias is not defined"
#define BUTTON0 ""
#define BUTTON_PIN 0
#define BUTTON_FLAGS 0
#endif
volatile bool button_is_on = false;
volatile bool already_sent = false;

static bool central_connected = false;
static bool sensor_notify_enabled;
static bool heartbeat_notify_enabled;
struct bt_conn *conn_hub;

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME, sizeof(CONFIG_BT_DEVICE_NAME)),
};

struct bt_gatt_data  {
    uint8_t node_id;
    uint8_t msg_type;
    uint16_t sequence_number;
    uint16_t payload[3];
};

static void heartbeat_ccc_cfg_changed(const struct bt_gatt_attr *attr,
                                   uint16_t value)
{
    heartbeat_notify_enabled = (value == BT_GATT_CCC_NOTIFY);

    printk("Heartbeat notifications %s\n",
           heartbeat_notify_enabled ? "enabled" : "disabled");
}

static void sensor_ccc_cfg_changed(const struct bt_gatt_attr *attr,
                                   uint16_t value)
{
    sensor_notify_enabled = (value == BT_GATT_CCC_NOTIFY);

    printk("Sensor notifications %s\n",
           sensor_notify_enabled ? "enabled" : "disabled");
}

// UUID for Sensor Service (primary)
#define BT_UUID_SENSOR_PRIMARY_VAL \
    BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x1234, 0x56789abcdef0)
// UUID for Sensor Data Characteristic
#define BT_UUID_SENSOR_CHAR \
    BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x1234, 0x56789abcdef1)
static struct bt_uuid_128 sensor_uuid = BT_UUID_INIT_128(BT_UUID_SENSOR_CHAR);
#define BT_UUID_HEARTBEAT_CHAR \
    BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x1234, 0x56789abcdef2)
static struct bt_uuid_128 heartbeat_uuid = BT_UUID_INIT_128(BT_UUID_HEARTBEAT_CHAR);
// Sensor Service Declarationn
BT_GATT_SERVICE_DEFINE(sensor_svc,
    BT_GATT_PRIMARY_SERVICE(BT_UUID_DECLARE_128(BT_UUID_SENSOR_PRIMARY_VAL)),
    BT_GATT_CHARACTERISTIC(&sensor_uuid.uuid, BT_GATT_CHRC_NOTIFY, BT_GATT_PERM_NONE,
        NULL, NULL, NULL),
    BT_GATT_CCC(sensor_ccc_cfg_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
    BT_GATT_CHARACTERISTIC(&heartbeat_uuid.uuid, BT_GATT_CHRC_NOTIFY, BT_GATT_PERM_NONE,
        NULL, NULL, NULL),
    BT_GATT_CCC(heartbeat_ccc_cfg_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
);

// Callback handler Function for button interrupt
void button_cb(const struct device *port, struct gpio_callback *cb, gpio_port_pins_t pins){
	if (pins & BIT(BUTTON_PIN)){
		uint8_t val = gpio_pin_get(port, BUTTON_PIN);
		if (val == 1) {
			// Active low, so pressed = 0
			printk("Button was pressed!\n");
			button_is_on = true;
            
         } else {
			printk("Button was released!\n");
			button_is_on = false;
	    }
	}
    return;
}

static int send_heartbeat()
{
    if (!conn_hub || !heartbeat_notify_enabled) {
        printk("No connection or notifications disabled\n");
        return -ENOTCONN;
    }

    struct bt_gatt_data pkt;
    pkt.node_id = WIN_NODE;
    pkt.msg_type = HEARTBEAT;
    pkt.sequence_number = seq_num;
    pkt.payload[0] = 0;
    pkt.payload[1] = 0;
    pkt.payload[2] = 0;
    seq_num ++;
    return bt_gatt_notify(conn_hub,
                          &sensor_svc.attrs[5],
                          &pkt,
                          sizeof(pkt));
}

static int send_sensor_packet(struct bt_gatt_data *pkt)
{
    if (!conn_hub || !sensor_notify_enabled) {
        printk("No connection or notifications disabled\n");
        return -ENOTCONN;
    }

    return bt_gatt_notify(conn_hub,
                          &sensor_svc.attrs[2],
                          pkt,
                          sizeof(*pkt));
}

static void createDataPacket(struct bt_gatt_data* packet, uint16_t sensor_type, uint16_t val1, uint16_t val2)
{
    packet->node_id = WIN_NODE;
    packet->msg_type = SENSOR_DATA;
    packet->sequence_number = seq_num;
    packet->payload[0] = sensor_type;
    packet->payload[1] = val1;
    packet->payload[2] = val2;
    seq_num++;
}

// Callback function for connection events
static void connected(struct bt_conn *conn, uint8_t err)
{
    if (err) {
        printk("Connection failed (err %u)\n", err);
        return;
    }
    central_connected = true;
    printk("Central connected\n");
    conn_hub = bt_conn_ref(conn);
    err = bt_le_adv_stop();
    if (err != 0){
        printk("Stopping Advertising failed!\n");
        return;
    }

}

// Callback function for disconnection events
static void disconnected(struct bt_conn *conn, uint8_t reason)
{
    printk("Central disconnected (reason %u)\n", reason);
    if (conn_hub) {
        bt_conn_unref(conn_hub);
        conn_hub = NULL;
    }
    central_connected = false;
    sensor_notify_enabled = false;
    heartbeat_notify_enabled = false;
}

// Define connection callbacks
static struct bt_conn_cb conn_callbacks = {
    .connected = connected,
    .disconnected = disconnected,
};

bool bt_is_ready(){
    int err = bt_enable(NULL);
    if (err){
        printk("Enabling BLE failed\n");
        return false;
    }
    printk("Bluetooth initialized\n");
	bt_conn_cb_register(&conn_callbacks);
    return true;
}


// GPIO callback structure
struct gpio_callback button_cb_struct;
const struct device *dev_button;
int ret_button;
int ret_interrupt_button;

void main(void)
{
    if (!bt_is_ready()){
        return;
    }
    
    int err = bt_le_adv_start(BT_LE_ADV_CONN, ad, ARRAY_SIZE(ad), NULL, NULL);
    if (err < 0){
        printk("Advertising Data failed\n");
        return;
    }


   
    // search for struct of gpio0 device
	dev_button = device_get_binding(BUTTON0);
	if (dev_button == NULL) {
		printk("Device BUTTON0 wasn't found!\n");
		return;
	}

    // configure for gpio0 the pin 11 as an gpio input for the button
	ret_button = gpio_pin_configure(dev_button, BUTTON_PIN, GPIO_INPUT | BUTTON_FLAGS);
	if (ret_button != 0) {
		printk("Configuration of Button Pin failed!\n");
		return;
	}
    // configure for gpio0 the pin 11 as an interrupt pin
	ret_interrupt_button = gpio_pin_interrupt_configure(dev_button, BUTTON_PIN, GPIO_INT_EDGE_BOTH);
	if (ret_interrupt_button != 0) {
		printk("Configuration of interrupt failed!\n");
		return;
	}
    
    // Initialize GPIO callback structure for interrupts
	gpio_init_callback(&button_cb_struct, button_cb, BIT(BUTTON_PIN));
	gpio_add_callback(dev_button, &button_cb_struct);

    // Send Advertising Data to give Central opportunity to connect with EnvironmentNode
    while (!central_connected || !sensor_notify_enabled || 
        !heartbeat_notify_enabled){
        k_sleep(K_MSEC(200));
    }

    while(1){
        if (button_is_on && !already_sent){
            already_sent = true;
            // Save Window Data in packet
            struct bt_gatt_data window_packet;
            createDataPacket(&window_packet, WINDOW_SENSOR, 1, 0);
            // Send packet via Notification
            err = send_sensor_packet(&window_packet);
            if (err != 0){
                printk("Sending Windowpacket failed\n");
            }
            printk("Window packet OPEN was send");
        } else if (!button_is_on && already_sent) {
            already_sent = false;
            // Save Window Data in packet
            struct bt_gatt_data window_packet;
            createDataPacket(&window_packet, WINDOW_SENSOR, 0, 0);
            // Send packet via Notification
            err = send_sensor_packet(&window_packet);
            if (err != 0){
                printk("Sending Windowpacket failed\n");
            }
            printk("Window packet CLOSED was send");
        }
        // Send Heartbeat via Notification
        err = send_heartbeat();
        if (err != 0){
            printk("Sending Heartbeat failed\n");
        }
        k_sleep(K_SECONDS(1));
    }
}

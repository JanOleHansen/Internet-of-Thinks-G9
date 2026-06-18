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
//#include <bluetooth/services/cts.h>
#include <bluetooth/services/hrs.h>
//#include <bluetooth/services/ias.h>
#include <bluetooth/gatt.h>

#define ENV_NODE 1
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

/* Part for Temp- & Humiditysensor 
https://www.mouser.com/datasheet/2/758/DHT11-Technical-Data-Sheet-Translated-Version-1143054.pdf?srsltid=AfmBOop9QYyarqOQ7LcXVN-u1Wop3ConD42_Do83c4glCUWUm1D73HyP */
#define DHT_PIN 13

/* Part for Lightsensor
https://www.berrybase.de/es/product-datasheet/018deef3df7b72c7a37e653c07583a1d/create?srsltid=AfmBOoob8iPsz877Es7JHAZs6cUjDZxpCms4U8A4gsVSBR18xQOwl5Cj */
#define LIGHT_PIN 14

/* Part for Movementsensor
https://cdn-reichelt.de/documents/datenblatt/A300/SEN-HC-SR501-ANLEITUNG-23.09.2020.pdf */
#define MOVEMENT_PIN 10
bool action = false;

/* 
Advertisin Data:
		Name: ENV_NODE
		Flags: LE GENERAL DISCOVERABLE
*/

static const struct device *gpio1 = DEVICE_DT_GET(DT_NODELABEL(gpio1));

static bool connected = false;

static struct mfg_data {
    uint8_t name_id;
};
static struct mfg_data my_mfg_data = {
    .name_id = ENV_NODE
};
static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA(BT_DATA_MANUFACTURER_DATA, &my_mfg_data, sizeof(&my_mfg_data)),
};
struct dht11_data {
    uint8_t humidity_int;
    uint8_t humidity_dec;
    uint8_t temp_int;
    uint8_t temp_dec;
    uint8_t checksum;
};

struct bt_gatt_data  {
    uint8_t node_id;
    uint8_t msg_type;
    uint16_t sequence_number;
    uint16_t payload[3];
};
// UUID for Sensor Service (primary)
#define BT_UUID_SENSOR_PRIMARY_VAL \
    BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x1234, 0x56789abcdef0)
// UUID for Sensor Data Characteristic
#define BT_UUID_SENSOR_CHAR \
    BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x1234, 0x56789abcdef1)
static struct bt_uuid_128 sensor_uuid = BT_UUID_INIT_128(BT_UUID_SENSOR_CHAR);
// Sensor Service Declarationn
BT_GATT_SERVICE_DEFINE(sensor_svc,
    BT_GATT_PRIMARY_SERVICE(BT_UUID_DECLARE_128(BT_UUID_SENSOR_PRIMARY_VAL)),
    BT_GATT_CHARACTERISTIC(&sensor_uuid.uuid, BT_GATT_CHRC_READ, BT_GATT_PERM_READ,
        NULL, NULL, NULL),
);

// Callback function for connection events
static void connected(struct bt_conn *conn, uint8_t err)
{
    if (err) {
        printk("Connection failed (err %u)\n", err);
        return;
    }
    printk("Central connected\n");
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
}

// Define connection callbacks
static struct bt_conn_cb conn_callbacks = {
    .connected = connected,
    .disconnected = disconnected,
};

static int wait_until_level(int level, uint32_t timeout_us)
{
    uint32_t start = k_cycle_get_32();
    uint32_t timeout_cycles = k_us_to_cyc_ceil32(timeout_us);

    while (gpio_pin_get(gpio1, DHT_PIN) != level) {
        if ((uint32_t)(k_cycle_get_32() - start) > timeout_cycles) {
            return -ETIMEDOUT;
        }
    }

    return 0;
}

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
static int measure_level_duration_us(int level, uint32_t timeout_us)
{
    uint32_t start = k_cycle_get_32();
    uint32_t timeout_cycles = k_us_to_cyc_ceil32(timeout_us);

    while (gpio_pin_get(gpio1, DHT_PIN) == level) {
        if ((uint32_t)(k_cycle_get_32() - start) > timeout_cycles) {
            return -ETIMEDOUT;
        }
    }

    uint32_t elapsed_cycles = k_cycle_get_32() - start;
    return (int)k_cyc_to_us_floor32(elapsed_cycles);
}
int dht11_read(struct dht11_data *out)
{
    uint8_t data[5] = {0};
    int ret;

    if (!device_is_ready(gpio1)) {
        return -ENODEV;
    }

    ret = gpio_pin_configure(gpio1, DHT_PIN, GPIO_INPUT | GPIO_PULL_UP);
    if (ret < 0) {
        return ret;
    }

    k_msleep(2);

    ret = gpio_pin_configure(gpio1, DHT_PIN, GPIO_OUTPUT_LOW);
    if (ret < 0) {
        return ret;
    }

    k_msleep(20);

    ret = gpio_pin_configure(gpio1, DHT_PIN, GPIO_INPUT | GPIO_PULL_UP);
    if (ret < 0) {
        return ret;
    }

    k_busy_wait(30);

    ret = wait_until_level(0, 200);
    if (ret < 0) {
        return -EIO;
    }

    ret = measure_level_duration_us(0, 120);
    if (ret < 0) {
        return -EIO;
    }

    ret = measure_level_duration_us(1, 120);
    if (ret < 0) {
        return -EIO;
    }

    for (int i = 0; i < 40; i++) {
        ret = wait_until_level(0, 100);
        if (ret < 0) {
            return -EIO;
        }

        ret = measure_level_duration_us(0, 100);
        if (ret < 0) {
            return -EIO;
        }

        ret = measure_level_duration_us(1, 120);
        if (ret < 0) {
            return -EIO;
        }

        int high_us = ret;

        data[i / 8] <<= 1;

        if (high_us > 50) {
            data[i / 8] |= 1;
        }
    }

    uint8_t checksum = (uint8_t)(data[0] + data[1] + data[2] + data[3]);

    if (checksum != data[4]) {
        printk("Checksum failed: calc=%u received=%u\n", checksum, data[4]);
        printk("Raw: %u %u %u %u %u\n",
               data[0], data[1], data[2], data[3], data[4]);
        return -EBADMSG;
    }

    out->humidity_int = data[0];
    out->humidity_dec = data[1];
    out->temp_int = data[2];
    out->temp_dec = data[3];
    out->checksum = data[4];

    printk("Raw: %u %u %u %u %u\n",
           data[0], data[1], data[2], data[3], data[4]);

    return 0;
}
struct bt_gatt_data* createDataPacket(uint16_t sensor_type, uint16_t val1, uint16_t val2){
    uint16_t payload[3] = {sensor_type, val1, val2};
    struct bt_gatt_data packet = {
        .node_id = ENV_NODE,
        .msg_type = SENSOR_DATA,
        .sequence_number = seq_num,
        .payload = payload,
    };
    seq_num++;
    return &packet;
}
void main(void)
{
    struct dht11_data dht;
    
    if (!bt_is_ready()){
        return;
    }
    
    int err = bt_le_adv_start(BT_LE_ADV_CONN, ad, ARRAY_SIZE(ad), NULL, NULL);
    if (err < 0){
        printk("Advertising Data failed\n");
        return;
    }


    if (!device_is_ready(gpio1)){
        printk("GPIO1 is not ready\n");
        return;
    }

    int err = gpio_pin_configure(gpio1, LIGHT_PIN, GPIO_INPUT);
    if (err != 0){
        printk("Configuration of Light-Pin failed\n");
        return;
    }

    err = gpio_pin_configure(gpio1, MOVEMENT_PIN, GPIO_INPUT);
    if (err != 0){
        printk("Movement Pin Configuration failed\n");
        return;
    }

    // Send Advertising Data to give Central opportunity to connect with EnvironmentNode
    while (!connected){
        k_sleep(K_MSEC(200));
    }

    while (1) {

        // Read DHT11
        int ret = dht11_read(&dht);
        if (ret == 0) {
            printk("Temp: %u.%u C, Humidity: %u.%u %%\n",
                   dht.temp_int,
                   dht.temp_dec,
                   dht.humidity_int,
                   dht.humidity_dec);
        } else {
            printk("DHT11 read failed: %d\n", ret);
        }
        // Save DHT11 Data in two packets
        struct bt_gatt_data *temp_packet = createDataPacket(TEMP_SENSOR, dht.temp_int, dht.temp_dec);
        struct bt_gatt_data *hum_packet = createDataPacket(HUM_SENSOR, dht.humidity_int, dht.humidity_dec);
        // TODO: Send packets via Notification

        // Read Light Sensor
        int value = gpio_pin_get(gpio1, LIGHT_PIN);
        if (value == 0){
            printk("Es ist hell\n");
        } else {
            printk("Es ist dunkel\n");
        }
        // Save Light Data in packet
        struct bt_gatt_data *light_packet = createDataPacket(LIGHT_SENSOR, value, NULL);
        // TODO: Send packet via Notification

        // Read Movement Sensor
        int val = gpio_pin_get(gpio1, MOVEMENT_PIN);
        if (val == 1 && !action){
            action = true;
            printk("Movement was detected\n");
        } else if (val == 0 && action){
            action = false;
            printk("No Movement was detected\n");
        }
        // Save Movement Data in packet
        struct bt_gatt_data *movement_packet = createDataPacket(MOTION_SENSOR, val, NULL);
        // TODO: Send packet via Notification
        
        k_sleep(K_SECONDS(1));
    }
}
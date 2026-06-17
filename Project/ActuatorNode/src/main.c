#include <zephyr.h>
#include <zephyr/types.h>
#include <sys/printk.h>
#include <device.h>
#include <drivers/gpio.h>
#include <devicetree.h>
#include <string.h>
#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/gatt.h>

// Define constants for node ID, message types, and commands
#define ACT_NODE_ID 3
#define MSG_TYPE_COMMAND 2
#define COMMAND_LIGHT_ON 1
#define COMMAND_LIGHT_OFF 2
#define COMMAND_HEATING_ON 3
#define COMMAND_HEATING_OFF 4

// Define the blink interval in milliseconds
#define BLINK_INTERVAL_MS 500

#define SERVO_PIN 11
#define SERVO_PERIOD_US 20000
#define SERVO_MIN_US 1000
#define SERVO_MID_US 1500
#define SERVO_MAX_US 2000

// Declare global variables for the LED device, pin, and label
static const struct device *led_dev;
static const gpio_pin_t led_pin = DT_GPIO_PIN(DT_ALIAS(led0), gpios);
static const char *led_label = DT_GPIO_LABEL(DT_ALIAS(led0), gpios);
static volatile bool blink_enabled;
static struct k_timer blink_timer;
static struct k_work led_toggle_work;

// Declare global variables for the servo control
static const struct device *gpio1 = DEVICE_DT_GET(DT_NODELABEL(gpio1));
static struct k_timer servo_timer;

static uint32_t servo_pulse_us = SERVO_MID_US;
static bool servo_running = false;
static bool servo_pin_high = false;


struct msg_frame {
    uint8_t node_id;
    uint8_t msg_type;
    uint16_t sequence_number;
    uint16_t command;
} __packed;

static void led_toggle_work_handler(struct k_work *work)
{
    if (blink_enabled) {
        gpio_pin_toggle(led_dev, led_pin);
    }
}

static void blink_timer_expiry(struct k_timer *timer_id)
{
    k_work_submit(&led_toggle_work);
}

static void servo_timer_handler(struct k_timer *timer)
{
    ARG_UNUSED(timer);

    if (!servo_running) {
        gpio_pin_set(gpio1, SERVO_PIN, 0);
        servo_pin_high = false;
        return;
    }

    if (!servo_pin_high) {
        gpio_pin_set(gpio1, SERVO_PIN, 1);
        servo_pin_high = true;

        k_timer_start(&servo_timer,
                      K_USEC(servo_pulse_us),
                      K_NO_WAIT);
    } else {
        gpio_pin_set(gpio1, SERVO_PIN, 0);
        servo_pin_high = false;

        k_timer_start(&servo_timer,
                      K_USEC(SERVO_PERIOD_US - servo_pulse_us),
                      K_NO_WAIT);
    }
}

static void servo_start(uint32_t pulse_us)
{
    servo_pulse_us = pulse_us;
    servo_running = true;
    servo_pin_high = false;

    k_timer_start(&servo_timer, K_NO_WAIT, K_NO_WAIT);
}

static void __attribute__((unused)) servo_stop(void)
{
    servo_running = false;
    gpio_pin_set(gpio1, SERVO_PIN, 0);
    k_timer_stop(&servo_timer);
}

static void servo_set_angle(int angle)
{
    if (angle < -90) {
        angle = -90;
    }

    if (angle > 90) {
        angle = 90;
    }

    uint32_t pulse_us = 1500 + ((angle * 500) / 90);

    servo_start(pulse_us);
}

static ssize_t write_command(struct bt_conn *conn,
                             const struct bt_gatt_attr *attr,
                             const void *buf,
                             uint16_t len,
                             uint16_t offset,
                             uint8_t flags)
{
    if (offset != 0) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }

    if (len < sizeof(struct msg_frame)) {
        printk("Invalid command frame length: %u\n", len);
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }

    struct msg_frame msg;
    if (len > sizeof(msg)) {
        len = sizeof(msg);
    }
    memcpy(&msg, buf, len);

    if (msg.node_id != ACT_NODE_ID) {
        printk("Ignoring message for node %u\n", msg.node_id);
        return len;
    }

    if (msg.msg_type != MSG_TYPE_COMMAND) {
        printk("Unsupported message type %u\n", msg.msg_type);
        return len;
    }

    uint16_t command = msg.command;
    switch (command) {
    case COMMAND_LIGHT_ON:
        blink_enabled = true;
        gpio_pin_set(led_dev, led_pin, 1);
        k_timer_start(&blink_timer, K_MSEC(BLINK_INTERVAL_MS), K_MSEC(BLINK_INTERVAL_MS));
        printk("ACT_NODE: LIGHT_ON received\n");
        break;
    case COMMAND_LIGHT_OFF:
        blink_enabled = false;
        k_timer_stop(&blink_timer);
        gpio_pin_set(led_dev, led_pin, 0);
        printk("ACT_NODE: LIGHT_OFF received\n");
        break;
    case COMMAND_HEATING_ON:
        servo_set_angle(90);
        printk("ACT_NODE: HEATING_ON received\n");
        break;
    case COMMAND_HEATING_OFF:
        servo_set_angle(-90);
        printk("ACT_NODE: HEATING_OFF received\n");
        break;
    default:
        printk("Unknown command %u\n", command);
        break;
    }

    return len;
}

BT_GATT_SERVICE_DEFINE(actuator_svc,
    BT_GATT_PRIMARY_SERVICE(BT_UUID_DECLARE_16(0xFFF0)),
    BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_16(0xFFF1),
                           BT_GATT_CHRC_WRITE,
                           BT_GATT_PERM_WRITE,
                           NULL,
                           write_command,
                           NULL)
);

static const struct bt_data ad[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR),
    BT_DATA(BT_DATA_NAME_COMPLETE, "ACT_NODE", 8),
};

static void bt_ready(int err)
{
    if (err) {
        printk("Bluetooth init failed (err %d)\n", err);
        return;
    }

    printk("Bluetooth initialized\n");

    err = bt_le_adv_start(BT_LE_ADV_CONN, ad, ARRAY_SIZE(ad), NULL, 0);
    if (err) {
        printk("Advertising failed to start (err %d)\n", err);
        return;
    }

    printk("Advertising as ACT_NODE\n");
}

void main(void)
{
    int err;

    printk("Starting ACT_NODE firmware\n");

    led_dev = device_get_binding(led_label);
    if (!led_dev) {
        printk("Failed to bind LED device %s\n", led_label);
        return;
    }

    err = gpio_pin_configure(led_dev, led_pin, GPIO_OUTPUT_INACTIVE);
    if (err) {
        printk("Failed to configure LED pin %d (err %d)\n", led_pin, err);
        return;
    }

    if (!device_is_ready(gpio1)) {
        printk("Failed to get gpio1 device\n");
        return;
    }

    err = gpio_pin_configure(gpio1, SERVO_PIN, GPIO_OUTPUT_INACTIVE);
    if (err) {
        printk("Failed to configure servo pin %d (err %d)\n", SERVO_PIN, err);
        return;
    }

    k_work_init(&led_toggle_work, led_toggle_work_handler);
    k_timer_init(&blink_timer, blink_timer_expiry, NULL);
    k_timer_init(&servo_timer, servo_timer_handler, NULL);
    blink_enabled = false;

    err = bt_enable(bt_ready);
    if (err) {
        printk("Bluetooth enable failed (err %d)\n", err);
        return;
    }

    while (1) {
        k_sleep(K_SECONDS(1));
    }
}
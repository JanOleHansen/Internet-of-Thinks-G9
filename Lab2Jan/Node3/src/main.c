/*
 * IoT Lab 2 – Part 1: Multi-hop Network Formation
 *
 * Line topology: device_1 -- device_2 -- device_3 -- device_4
 * Each node is 10 m apart; BLE range = 11 units → only direct neighbours
 * communicate.  All four nodes run this same firmware.
 *
 * Protocol (non-connectable BLE advertising + passive scanning):
 *
 *   MSG_NET_FORM (0x01) – first button press on any node:
 *       that node becomes the INITIATOR, begins advertising, LED turns on.
 *   MSG_LED_ON   (0x02) – initiator button held:  all LEDs on.
 *   MSG_LED_OFF  (0x03) – initiator button released: all LEDs off.
 *
 * Each message carries an 8-byte manufacturer-data payload:
 *   [company_id:2][msg_type:1][seq:1][net_id:3]
 *
 * net_id   = lower 3 bytes of the INITIATOR's BLE address – used to
 *            identify "our" network and prevent cross-network noise.
 * seq      = monotonically increasing counter; prevents relay loops
 *            (each relay node ignores a seq it has already forwarded).
 *
 * States:
 *   IDLE      – scanning; first button press → INITIATOR.
 *   INITIATOR – advertises continuously; button events update LED + payload.
 *               Does NOT scan (it is the source).
 *   MEMBER    – received a message from the initiator; scans and, when a
 *               new-seq message arrives, sets its own LED and re-advertises
 *               the same packet for 500 ms so the next hop can receive it.
 *
 * Button presses on MEMBER nodes are silently ignored.
 *
 * To simulate a button press in Renode Monitor:
 *   mach set "device_1"
 *   gpio0 OnGPIO 11 false   # SW0 pressed  (active-low)
 *   gpio0 OnGPIO 11 true    # SW0 released
 */

#include <zephyr.h>
#include <drivers/gpio.h>
#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <sys/printk.h>
#include <sys/atomic.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Protocol constants                                                  */
/* ------------------------------------------------------------------ */
#define COMPANY_ID    0xFFFFU   /* test/reserved company ID */
#define MSG_NET_FORM  0x01U     /* network formation (implies LED on)  */
#define MSG_LED_ON    0x02U
#define MSG_LED_OFF   0x03U

/* ------------------------------------------------------------------ */
/*  Node state (atomic for safe access across BLE and work-queue threads) */
/* ------------------------------------------------------------------ */
#define STATE_IDLE      0
#define STATE_INITIATOR 1
#define STATE_MEMBER    2

static atomic_t node_state = ATOMIC_INIT(STATE_IDLE);

/* network identifier: 3 LSBs of initiator's BLE address */
static uint8_t my_net_id[3];

/* last sequence number processed by this MEMBER node (dedup) */
static uint8_t last_fwd_seq = 0xFFU;

/* sequence counter owned by the INITIATOR */
static uint8_t tx_seq;

/* ------------------------------------------------------------------ */
/*  GPIO                                                                */
/* ------------------------------------------------------------------ */
#define SW0_NODE   DT_ALIAS(sw0)
#define LED0_NODE  DT_ALIAS(led0)

static const struct gpio_dt_spec btn = GPIO_DT_SPEC_GET(SW0_NODE, gpios);
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);
static struct gpio_callback button_cb_data;

/* raw active level captured in ISR, consumed by button_work */
static volatile int btn_active;

/* ------------------------------------------------------------------ */
/*  BLE payload (manufacturer-specific data, 8 bytes)                  */
/* ------------------------------------------------------------------ */
struct __packed msg_payload {
    uint16_t company_id;
    uint8_t  msg_type;
    uint8_t  seq;
    uint8_t  net_id[3];
    uint8_t  padding;    // 1 byte (Adds up to 8!)
};

BUILD_ASSERT(sizeof(struct msg_payload) == 8,
             "msg_payload size mismatch");

/* Live advertisement payload – updated before each adv start/update */
static struct msg_payload adv_payload;

/* Staging area: scan callback writes here before submitting relay_work */
static struct msg_payload relay_stage;

/* BLE data descriptor pointing into adv_payload */
static struct bt_data ad[] = {
    BT_DATA(BT_DATA_MANUFACTURER_DATA,
            (const uint8_t *)&adv_payload,
            sizeof(adv_payload)),
};

/* Non-connectable, identity address, 100-150 ms interval */
static const struct bt_le_adv_param adv_param =
    BT_LE_ADV_PARAM_INIT(BT_LE_ADV_OPT_USE_IDENTITY,
                          BT_GAP_ADV_FAST_INT_MIN_2,
                          BT_GAP_ADV_FAST_INT_MAX_2,
                          NULL);

/* Passive scan: 50 ms interval, 30 ms active window */
static struct bt_le_scan_param scan_param = {
    .type     = BT_LE_SCAN_TYPE_PASSIVE,
    .options  = BT_LE_SCAN_OPT_NONE,
    .interval = 0x0050U,   /* 50 ms in 0.625-ms units */
    .window   = 0x0030U,   /* 30 ms */
};

static bool is_advertising;
static bool is_scanning;

/* ------------------------------------------------------------------ */
/*  Forward declarations                                                */
/* ------------------------------------------------------------------ */
static void scan_cb(const bt_addr_le_t *addr, int8_t rssi,
                    uint8_t adv_type, struct net_buf_simple *buf);
static void button_work_fn(struct k_work *w);
static void relay_work_fn(struct k_work *w);
static void adv_stop_work_fn(struct k_work *w);

K_WORK_DEFINE(button_work, button_work_fn);
K_WORK_DEFINE(relay_work,  relay_work_fn);
K_WORK_DELAYABLE_DEFINE(adv_stop_work, adv_stop_work_fn);

/* ------------------------------------------------------------------ */
/*  BLE helpers                                                         */
/* ------------------------------------------------------------------ */
static void ble_stop_scan(void)
{
    if (is_scanning) {
        bt_le_scan_stop();
        is_scanning = false;
    }
}

static void ble_start_scan(void)
{
    if (!is_scanning) {
        int err = bt_le_scan_start(&scan_param, scan_cb);
        if (err) {
            printk("scan start err %d\n", err);
        } else {
            is_scanning = true;
        }
    }
}

static void ble_start_adv(void)
{
    if (is_advertising) {
        /* Update payload while advertising is already running */
        bt_le_adv_update_data(ad, ARRAY_SIZE(ad), NULL, 0);
    } else {
        int err = bt_le_adv_start(&adv_param, ad, ARRAY_SIZE(ad), NULL, 0);
        if (err) {
            printk("adv start err %d\n", err);
        } else {
            is_advertising = true;
        }
    }
}

static void ble_stop_adv(void)
{
    if (is_advertising) {
        bt_le_adv_stop();
        is_advertising = false;
    }
}

/* ------------------------------------------------------------------ */
/*  Button ISR  (interrupt context – keep minimal)                      */
/* ------------------------------------------------------------------ */
static void button_isr(const struct device *dev,
                        struct gpio_callback *cb,
                        uint32_t pins)
{
    /* gpio_pin_get_dt returns 1 when SW0 is pressed (active level) */
    btn_active = gpio_pin_get_dt(&btn);
    k_work_submit(&button_work);
}

/* ------------------------------------------------------------------ */
/*  Button work handler  (system work queue)                            */
/* ------------------------------------------------------------------ */
static void button_work_fn(struct k_work *w)
{
    bool pressed        = (btn_active == 1);
    atomic_val_t state  = atomic_get(&node_state);

    /* ---- IDLE + press → become INITIATOR ---- */
    if (state == STATE_IDLE && pressed) {
        if (!atomic_cas(&node_state, STATE_IDLE, STATE_INITIATOR)) {
            return; /* another context already changed state */
        }

        /* Derive net_id from this node's own BLE address */
        bt_addr_le_t addr;
        size_t count = 1U;
        bt_id_get(&addr, &count);
        memcpy(my_net_id, addr.a.val, sizeof(my_net_id));

        printk("[INIT] Forming network  net_id=%02x:%02x:%02x\n",
               my_net_id[0], my_net_id[1], my_net_id[2]);

        ble_stop_scan();
        gpio_pin_set_dt(&led, 1);

        adv_payload.company_id = COMPANY_ID;
        adv_payload.msg_type   = MSG_NET_FORM;
        adv_payload.seq        = tx_seq++;
        memcpy(adv_payload.net_id, my_net_id, sizeof(my_net_id));

        ble_start_adv();
        return;
    }

    /* ---- INITIATOR: track button and update advertisement ---- */
    if (state == STATE_INITIATOR) {
        if (pressed) {
            gpio_pin_set_dt(&led, 1);
            adv_payload.msg_type = MSG_LED_ON;
            printk("[INIT] LED_ON  seq=%u\n", tx_seq);
        } else {
            gpio_pin_set_dt(&led, 0);
            adv_payload.msg_type = MSG_LED_OFF;
            printk("[INIT] LED_OFF seq=%u\n", tx_seq);
        }
        adv_payload.seq = tx_seq++;
        ble_start_adv();   /* bt_le_adv_update_data if already advertising */
    }

    /* MEMBER: button has no effect */
}

/* ------------------------------------------------------------------ */
/*  Relay work handler  (system work queue)                             */
/* ------------------------------------------------------------------ */
static void relay_work_fn(struct k_work *w)
{
    /*
     * Stop scanning, copy the staged payload into the advertisement
     * buffer, start advertising, then schedule a stop after 500 ms.
     * The 500-ms window is long enough for the next hop to detect
     * the packet with high probability (30-ms scan window, 50-ms cycle).
     */
    ble_stop_scan();
    memcpy(&adv_payload, &relay_stage, sizeof(adv_payload));
    ble_start_adv();
    k_work_reschedule(&adv_stop_work, K_MSEC(500));
}

/* ------------------------------------------------------------------ */
/*  Adv-stop work handler  (runs 500 ms after relay starts)            */
/* ------------------------------------------------------------------ */
static void adv_stop_work_fn(struct k_work *w)
{
    ble_stop_adv();

    if (atomic_get(&node_state) == STATE_MEMBER) {
        ble_start_scan();
        printk("[MEMBER] scanning resumed\n");
    }
}

/* ------------------------------------------------------------------ */
/*  BLE scan – advertisement data parser                               */
/* ------------------------------------------------------------------ */
static bool parse_data(struct bt_data *data, void *user_data)
{
    if (data->type != BT_DATA_MANUFACTURER_DATA ||
        data->data_len < (uint8_t)sizeof(struct msg_payload)) {
        return true; /* keep parsing other AD structures */
    }

    const struct msg_payload *msg =
        (const struct msg_payload *)data->data;

    if (msg->company_id != COMPANY_ID) {
        return true;
    }

    atomic_val_t state = atomic_get(&node_state);

    /* ---- IDLE: join on any message from this protocol ---- */
    if (state == STATE_IDLE) {
        if (!atomic_cas(&node_state, STATE_IDLE, STATE_MEMBER)) {
            return false; /* someone else already changed state */
        }
        memcpy(my_net_id, msg->net_id, sizeof(my_net_id));
        last_fwd_seq = 0xFFU;
        printk("[MEMBER] Joined network  net_id=%02x:%02x:%02x\n",
               my_net_id[0], my_net_id[1], my_net_id[2]);
        state = STATE_MEMBER;
    }

    /* ---- MEMBER: process messages that belong to our network ---- */
    if (state == STATE_MEMBER &&
        memcmp(msg->net_id, my_net_id, sizeof(my_net_id)) == 0 &&
        msg->seq != last_fwd_seq)
    {
        last_fwd_seq = msg->seq;

        /* Set local LED */
        if (msg->msg_type == MSG_NET_FORM ||
            msg->msg_type == MSG_LED_ON) {
            gpio_pin_set_dt(&led, 1);
            printk("[MEMBER] LED_ON  type=%u seq=%u\n",
                   msg->msg_type, msg->seq);
        } else if (msg->msg_type == MSG_LED_OFF) {
            gpio_pin_set_dt(&led, 0);
            printk("[MEMBER] LED_OFF seq=%u\n", msg->seq);
        }

        /* Stage and relay to the next hop */
        memcpy(&relay_stage, msg, sizeof(relay_stage));
        k_work_submit(&relay_work);
    }

    return false; /* stop after the first manufacturer-data entry */
}

/* ------------------------------------------------------------------ */
/*  BLE scan callback                                                   */
/* ------------------------------------------------------------------ */
static void scan_cb(const bt_addr_le_t *addr, int8_t rssi,
                    uint8_t adv_type, struct net_buf_simple *buf)
{
    bt_data_parse(buf, parse_data, NULL);
}

/* ------------------------------------------------------------------ */
/*  main                                                                */
/* ------------------------------------------------------------------ */
int main(void)
{
    printk("IoT Lab 2 – Part 1: Multi-hop Network Formation\n");

    /* Configure LED (output, initially off) */
    if (!device_is_ready(led.port)) {
        printk("LED device not ready\n");
        return 0;
    }
    gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);

    /* Configure button (input, interrupt on both edges) */
    if (!device_is_ready(btn.port)) {
        printk("Button device not ready\n");
        return 0;
    }
    gpio_pin_configure_dt(&btn, GPIO_INPUT);
    gpio_pin_interrupt_configure_dt(&btn, GPIO_INT_EDGE_BOTH);
    gpio_init_callback(&button_cb_data, button_isr, BIT(btn.pin));
    gpio_add_callback(btn.port, &button_cb_data);

    /* Initialise Bluetooth (synchronous) */
    int err = bt_enable(NULL);
    if (err) {
        printk("BT init failed (err %d)\n", err);
        return 0;
    }
    printk("Bluetooth initialised\n");

    /* Begin passive scanning */
    err = bt_le_scan_start(&scan_param, scan_cb);
    if (err) {
        printk("Scan start failed (err %d)\n", err);
        return 0;
    }
    is_scanning = true;
    printk("Ready – press SW0 on any node to form the network\n");

    /*
     * Poll the button every 50 ms as the primary input mechanism for
     * Renode simulation (gpio0 OnGPIO does not reliably fire GPIOTE
     * interrupts in the emulator).  The ISR path still works on real
     * hardware.  Polling detects any change in the pin's active level
     * and reuses the same button_work handler.
     */
    int prev_btn = -1;
    while (1) {
        int cur = gpio_pin_get_dt(&btn);
        if (cur >= 0 && cur != prev_btn) {
            prev_btn  = cur;
            btn_active = cur;
            k_work_submit(&button_work);
        }
        k_msleep(50);
    }

    return 0;
}

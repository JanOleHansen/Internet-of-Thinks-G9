/*
 * IoT Lab 2 – Part 3: Measurement Collection
 * ============================================
 *
 * All four nodes run this identical firmware.
 *
 * Topology (line, BLE range 11 units, 10 m spacing):
 *   device_1 -- device_2 -- device_3 -- device_4
 *
 * ── Network formation ────────────────────────────────────────────────────
 * Pressing SW0 on the first node to respond makes that node the SINK.
 * The SINK broadcasts MSG_NET_FORM for 1 s so that every hop in the line
 * has time to join.  Nodes that overhear MSG_NET_FORM become RELAY nodes
 * and re-broadcast it so the next hop can also join.  After 1 s the SINK
 * switches from advertising to scanning.
 *
 * Button presses on any node after the network is formed are ignored.
 *
 * ── Measurement collection ───────────────────────────────────────────────
 * After formation every node generates a random measurement every 200 ms
 * and timestamps it with k_uptime_get() (ms since boot).
 *
 *   Temperature : −25.0 … 200.0 °C   (uniform, one decimal place)
 *   Humidity    :   0.0 … 100.0 %    (uniform, one decimal place)
 *
 * RELAY nodes advertise their measurements as MSG_MEAS.  When a RELAY
 * receives a MSG_MEAS from a neighbour it re-advertises it so the next
 * hop can pick it up (hop-by-hop forwarding toward the SINK).
 * A per-source counter deduplicates measurements that arrive via more
 * than one relay path.
 *
 * The SINK scans continuously, receives all measurements, and prints
 * each one – including its own – in CSV format to the UART:
 *
 *   <nodeID>;<counter>;<temp>;<humidity>;<timestamp_ms>;<tx_time_ms>
 *
 *   nodeID      – low byte of the source node's BLE address (decimal)
 *   counter     – per-node measurement counter, starts at 0
 *   temp        – °C with one decimal place, e.g. -0.5 or 25.3
 *   humidity    – % with one decimal place, e.g. 67.8
 *   timestamp   – k_uptime_get() at measurement time (ms since boot)
 *   tx_time     – reception time at SINK minus timestamp (ms); 0 for
 *                 the SINK's own measurements
 *
 * ── Time synchronisation ─────────────────────────────────────────────────
 * All nodes start at simulation time 0 when the Renode 'start' command
 * is issued, so k_uptime_get() is implicitly synchronised.  On real
 * hardware, press the reset button on all boards simultaneously.
 *
 * ── Renode Monitor – simulate button press ───────────────────────────────
 *   mach set "device_1"
 *   gpio0 OnGPIO 11 false   # SW0 pressed  (active-low)
 *   gpio0 OnGPIO 11 true    # SW0 released
 */

#include <zephyr.h>
#include <drivers/gpio.h>
#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <random/rand32.h>
#include <sys/printk.h>
#include <sys/atomic.h>
#include <string.h>
#include <stdio.h>   /* snprintf */

/* =========================================================================
 *  Protocol constants
 * ========================================================================= */

#define COMPANY_ID   0xFFFFU   /* reserved test/prototype company ID         */
#define MSG_NET_FORM 0x01U     /* network-formation broadcast                */
#define MSG_MEAS     0x04U     /* measurement packet (source → SINK)         */

/* =========================================================================
 *  Node-state machine
 *
 *  IDLE  → (SW0 pressed here)   → STATE_SINK
 *  IDLE  → (MSG_NET_FORM heard) → STATE_RELAY
 *
 *  Stored in an atomic so it can be read safely from both the BLE RX
 *  thread and the system work-queue thread.
 * ========================================================================= */

#define STATE_IDLE  0
#define STATE_SINK  1   /* this node is the data sink / collection point     */
#define STATE_RELAY 2   /* this node is a source and a multi-hop relay       */

static atomic_t node_state = ATOMIC_INIT(STATE_IDLE);

/* Lower 3 bytes of the SINK's BLE address – used as a network identifier
 * so that measurements from two parallel groups don't mix.              */
static uint8_t my_net_id[3];

/* Lower byte of THIS node's BLE address – used as the per-node ID in
 * the measurement CSV output.                                           */
static uint8_t my_node_id;

/* =========================================================================
 *  GPIO – button (SW0 / P0.11, active-low) and LED (led0 / P0.13)
 * ========================================================================= */

#define SW0_NODE   DT_ALIAS(sw0)
#define LED0_NODE  DT_ALIAS(led0)

static const struct gpio_dt_spec btn = GPIO_DT_SPEC_GET(SW0_NODE, gpios);
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);
static struct gpio_callback       button_cb_data;

/* Raw active level captured in the ISR; consumed by button_work.        */
static volatile int btn_active;

/* =========================================================================
 *  BLE payload structures
 *
 *  All messages begin with msg_hdr (6 bytes).  company_id filters out
 *  unrelated BLE advertisements.  net_id prevents cross-network mixing.
 * ========================================================================= */

/*
 * Common 6-byte header at the start of every advertisement payload.
 */
struct __packed msg_hdr {
    uint16_t company_id;  /* 0xFFFF – identifies our lab protocol            */
    uint8_t  msg_type;    /* MSG_NET_FORM or MSG_MEAS                        */
    uint8_t  net_id[3];   /* lower 3 bytes of the SINK's BLE address         */
};

/*
 * MSG_NET_FORM payload  (7 bytes total).
 * seq is a monotone counter used to deduplicate relayed NET_FORM packets.
 */
struct __packed net_form_msg {
    struct msg_hdr hdr;
    uint8_t        seq;
};

/*
 * MSG_MEAS payload  (17 bytes total).
 *
 *  node_id  : low byte of the source node's BLE address
 *  counter  : per-node measurement counter (wraps at 65535)
 *  temp_10  : temperature × 10, range −250 … 2000  (i.e. −25.0 … 200.0 °C)
 *  hum_10   : humidity    × 10, range    0 … 1000  (i.e.   0.0 … 100.0 %)
 *  ts_ms    : k_uptime_get() in milliseconds at measurement time
 *
 * Fixed-point (×10) representation gives one decimal place without
 * floating-point arithmetic, in line with the hint in the lab sheet.
 */
struct __packed meas_msg {
    struct msg_hdr hdr;
    uint8_t        node_id;
    uint16_t       counter;
    int16_t        temp_10;   /* signed: negative for sub-zero temperatures  */
    uint16_t       hum_10;
    uint32_t       ts_ms;
};

BUILD_ASSERT(sizeof(struct net_form_msg) == 7,  "net_form_msg size");
BUILD_ASSERT(sizeof(struct meas_msg)     == 17, "meas_msg size");

/*
 * Single advertisement buffer large enough for the biggest message.
 * Both message types share this buffer; the msg_type field selects
 * which interpretation applies.
 */
static union {
    struct net_form_msg form;
    struct meas_msg     meas;
    uint8_t             raw[17];
} adv_buf;

/* bt_data entry that always points into adv_buf.raw.                    */
static struct bt_data ad[] = {
    BT_DATA(BT_DATA_MANUFACTURER_DATA, adv_buf.raw, sizeof(adv_buf)),
};

/*
 * Non-connectable advertising, 20–40 ms interval (shorter than Part 1
 * so that the ~50 ms relay window contains 1–2 packets).
 */
static const struct bt_le_adv_param adv_param =
    BT_LE_ADV_PARAM_INIT(BT_LE_ADV_OPT_USE_IDENTITY,
                          0x0020U,   /* 20 ms min  (0x0020 × 0.625 ms)       */
                          0x0040U,   /* 40 ms max                             */
                          NULL);

/*
 * Continuous passive scan: interval == window → 100 % duty cycle.
 * This maximises the probability of catching a 50 ms advertising window.
 */
static struct bt_le_scan_param scan_param = {
    .type     = BT_LE_SCAN_TYPE_PASSIVE,
    .options  = BT_LE_SCAN_OPT_NONE,
    .interval = 0x0020U,   /* 20 ms */
    .window   = 0x0020U,   /* 20 ms – equals interval → no gaps              */
};

static bool is_advertising;
static bool is_scanning;

/* =========================================================================
 *  NET_FORM sequence counter (SINK only – prevents relay loops)
 * ========================================================================= */
static uint8_t form_seq;

/* =========================================================================
 *  Per-node measurement counter
 * ========================================================================= */
static uint16_t meas_counter;

/* =========================================================================
 *  Deduplication tables
 *
 *  Both RELAY and SINK nodes track the last measurement counter they
 *  processed from each source.  If an identical or older counter arrives
 *  again (e.g. via a second relay path), it is silently dropped.
 *
 *  In a 4-node network there are at most 3 distinct source nodes per
 *  relay node, so MAX_SOURCES = 4 gives sufficient headroom.
 * ========================================================================= */

#define MAX_SOURCES 4

struct dedup_entry {
    uint8_t  node_id;       /* source node identifier                        */
    uint16_t last_counter;  /* most-recently processed counter for this node */
    bool     used;          /* whether this slot is occupied                 */
};

/* Used by RELAY nodes to avoid re-relaying the same measurement.        */
static struct dedup_entry relay_dedup[MAX_SOURCES];

/* Used by the SINK to avoid printing the same measurement twice.        */
static struct dedup_entry sink_dedup[MAX_SOURCES];

/* =========================================================================
 *  Forward declarations
 * ========================================================================= */

static void scan_cb(const bt_addr_le_t *, int8_t, uint8_t,
                    struct net_buf_simple *);
static void button_work_fn(struct k_work *);
static void form_done_work_fn(struct k_work *);
static void meas_work_fn(struct k_work *);
static void relay_work_fn(struct k_work *);
static void adv_stop_work_fn(struct k_work *);

K_WORK_DEFINE(button_work, button_work_fn);
K_WORK_DEFINE(relay_work,  relay_work_fn);

K_WORK_DELAYABLE_DEFINE(form_done_work, form_done_work_fn);
K_WORK_DELAYABLE_DEFINE(meas_work,      meas_work_fn);
K_WORK_DELAYABLE_DEFINE(adv_stop_work,  adv_stop_work_fn);

/*
 * Measurement staged by the BLE RX thread for relay_work to pick up.
 * relay_pending is a cheap volatile flag; relay_work reads it exactly
 * once at the start of each invocation.
 */
static struct meas_msg    relay_stage;
static volatile bool      relay_pending;

/* =========================================================================
 *  BLE helpers
 * ========================================================================= */

static void ble_start_scan(void)
{
    if (!is_scanning) {
        int err = bt_le_scan_start(&scan_param, scan_cb);
        if (err) {
            printk("[ERR] BLE scan start failed (err=%d)\n", err);
        } else {
            is_scanning = true;
        }
    }
}

static void ble_stop_scan(void)
{
    if (is_scanning) {
        bt_le_scan_stop();
        is_scanning = false;
    }
}

/*
 * Start advertising the current contents of adv_buf.
 * If advertising is already active, just update the payload in-place;
 * the BLE controller will use it on the next advertising event.
 */
static void ble_start_adv(void)
{
    if (is_advertising) {
        bt_le_adv_update_data(ad, ARRAY_SIZE(ad), NULL, 0);
    } else {
        int err = bt_le_adv_start(&adv_param, ad, ARRAY_SIZE(ad), NULL, 0);
        if (err) {
            printk("[ERR] BLE adv start failed (err=%d)\n", err);
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

/* =========================================================================
 *  Measurement generation
 * ========================================================================= */

/*
 * Generate a random temperature and humidity using the nRF52840 hardware
 * RNG via Zephyr's entropy subsystem (sys_rand32_get).
 *
 * Both values use fixed-point ×10 representation so that one decimal
 * place is preserved without floating-point arithmetic.
 *
 * Temperature range: −25.0 … 200.0 °C  → temp_10 ∈ [−250, 2000]
 *   Number of discrete values: 2000 − (−250) + 1 = 2251
 *
 * Humidity range:     0.0 … 100.0 %    → hum_10  ∈ [0, 1000]
 *   Number of discrete values: 1000 − 0 + 1 = 1001
 */
static void gen_measurement(int16_t *temp_10, uint16_t *hum_10)
{
    *temp_10 = (int16_t)((int32_t)(-250) +
                          (int32_t)(sys_rand32_get() % 2251U));
    *hum_10  = (uint16_t)(sys_rand32_get() % 1001U);
}

/*
 * Print a fixed-point temperature value with correct sign handling.
 *
 * The tricky case is −0.x (e.g. temp_10 = −5 → "−0.5"):
 *   −5 / 10 = 0  and  −5 % 10 = −5 in C99, so we must work with the
 *   absolute value and prepend the minus sign manually.
 */
static void print_temp(int16_t t10)
{
    if (t10 < 0) {
        uint16_t abs_val = (uint16_t)(-(int32_t)t10);
        printk("-%u.%u", abs_val / 10u, abs_val % 10u);
    } else {
        printk("%u.%u", (uint16_t)t10 / 10u, (uint16_t)t10 % 10u);
    }
}

/*
 * Print one measurement in the required format:
 *   <nodeID>;<counter>;<temp>;<humidity>;<timestamp_ms>;<tx_time_ms>
 *
 * tx_time_ms = rx_time_ms − m->ts_ms.
 * For the SINK's own measurements, pass rx_time_ms == m->ts_ms so that
 * tx_time prints as 0.
 */
static void print_measurement(const struct meas_msg *m, uint32_t rx_time_ms)
{
    uint32_t tx_time = (rx_time_ms >= m->ts_ms)
                       ? (rx_time_ms - m->ts_ms)
                       : 0U;

    printk("%u;%u;", (unsigned)m->node_id, (unsigned)m->counter);
    print_temp(m->temp_10);
    printk(";%u.%u;%u;%u\n",
           (unsigned)(m->hum_10 / 10u),
           (unsigned)(m->hum_10 % 10u),
           (unsigned)m->ts_ms,
           (unsigned)tx_time);
}

/* =========================================================================
 *  Deduplication helper
 *
 *  Returns true and updates last_counter when the measurement is new
 *  (counter strictly greater than the last one seen from this node).
 *  Returns false if it has already been processed or the table is full.
 * ========================================================================= */

static bool dedup_check(struct dedup_entry *table,
                         uint8_t node_id, uint16_t counter)
{
    /* Search for an existing entry for this source node. */
    for (int i = 0; i < MAX_SOURCES; i++) {
        if (table[i].used && table[i].node_id == node_id) {
            if (counter <= table[i].last_counter) {
                return false;   /* already processed – discard             */
            }
            table[i].last_counter = counter;
            return true;
        }
    }

    /* First time we see this source: allocate an empty slot. */
    for (int i = 0; i < MAX_SOURCES; i++) {
        if (!table[i].used) {
            table[i].used         = true;
            table[i].node_id      = node_id;
            table[i].last_counter = counter;
            return true;
        }
    }

    /* Table is full – should not happen in a 4-node network. */
    printk("[WARN] dedup table full\n");
    return false;
}

/* =========================================================================
 *  Button ISR  (runs in interrupt context – keep it minimal)
 * ========================================================================= */

static void button_isr(const struct device *dev,
                        struct gpio_callback *cb,
                        uint32_t pins)
{
    /* gpio_pin_get_dt: returns 1 when SW0 is pressed (active-low handled) */
    btn_active = gpio_pin_get_dt(&btn);
    k_work_submit(&button_work);
}

/* =========================================================================
 *  Button work handler  (system work queue)
 * ========================================================================= */

static void button_work_fn(struct k_work *w)
{
    /* Only a press on an IDLE node starts network formation. */
    if (btn_active != 1 || atomic_get(&node_state) != STATE_IDLE) {
        return;
    }

    /* Atomically claim the SINK role. */
    if (!atomic_cas(&node_state, STATE_IDLE, STATE_SINK)) {
        return;   /* race: another context already claimed a role            */
    }

    /* Derive net_id and node_id from our own BLE address.
     * net_id (3 bytes) identifies "our" network so nodes from a parallel
     * group do not mix in.  node_id (1 byte) appears in the CSV output.  */
    bt_addr_le_t addr;
    size_t count = 1U;
    bt_id_get(&addr, &count);
    memcpy(my_net_id, addr.a.val, sizeof(my_net_id));
    my_node_id = addr.a.val[0];

    printk("[SINK node=%u] Forming network  net_id=%02x:%02x:%02x\n",
           my_node_id, my_net_id[0], my_net_id[1], my_net_id[2]);

    gpio_pin_set_dt(&led, 1);   /* LED on: this node is the SINK            */

    /* Stop scanning and broadcast MSG_NET_FORM so relay nodes can join.    */
    ble_stop_scan();

    memset(adv_buf.raw, 0, sizeof(adv_buf.raw));
    adv_buf.form.hdr.company_id = COMPANY_ID;
    adv_buf.form.hdr.msg_type   = MSG_NET_FORM;
    memcpy(adv_buf.form.hdr.net_id, my_net_id, 3);
    adv_buf.form.seq            = form_seq++;

    ble_start_adv();

    /*
     * Give the network 1 s to form (3 hops × ~300 ms each is well within
     * 1 s), then switch the SINK from advertising to scanning.
     */
    k_work_reschedule(&form_done_work, K_MSEC(1000));
}

/* =========================================================================
 *  Formation-done work handler  (runs 1 s after SINK sends first NET_FORM)
 * ========================================================================= */

static void form_done_work_fn(struct k_work *w)
{
    printk("[SINK] Network formed – collecting measurements\n");

    /* Stop NET_FORM advertising and switch to scan-only mode.            */
    ble_stop_adv();
    ble_start_scan();

    /* Start the SINK's own 200 ms measurement loop immediately.          */
    k_work_reschedule(&meas_work, K_NO_WAIT);
}

/* =========================================================================
 *  Measurement work handler  (fires every 200 ms on every node)
 * ========================================================================= */

static void meas_work_fn(struct k_work *w)
{
    int16_t  temp_10;
    uint16_t hum_10;
    gen_measurement(&temp_10, &hum_10);

    uint32_t ts  = (uint32_t)k_uptime_get();
    uint16_t ctr = meas_counter++;

    atomic_val_t state = atomic_get(&node_state);

    if (state == STATE_SINK) {
        /*
         * SINK: print own measurement with tx_time = 0.
         * Build a temporary struct for print_measurement; passing
         * ts as both the timestamp and rx_time gives tx_time = 0.
         */
        struct meas_msg self;
        memset(&self, 0, sizeof(self));
        self.hdr.company_id = COMPANY_ID;
        self.hdr.msg_type   = MSG_MEAS;
        memcpy(self.hdr.net_id, my_net_id, 3);
        self.node_id  = my_node_id;
        self.counter  = ctr;
        self.temp_10  = temp_10;
        self.hum_10   = hum_10;
        self.ts_ms    = ts;
        print_measurement(&self, ts);   /* tx_time = ts − ts = 0            */

    } else if (state == STATE_RELAY) {
        /*
         * RELAY: build a MSG_MEAS payload and start advertising it.
         * Stop scanning first so the BLE radio is free to transmit;
         * adv_stop_work will resume scanning after 50 ms.
         */
        memset(adv_buf.raw, 0, sizeof(adv_buf.raw));
        adv_buf.meas.hdr.company_id = COMPANY_ID;
        adv_buf.meas.hdr.msg_type   = MSG_MEAS;
        memcpy(adv_buf.meas.hdr.net_id, my_net_id, 3);
        adv_buf.meas.node_id  = my_node_id;
        adv_buf.meas.counter  = ctr;
        adv_buf.meas.temp_10  = temp_10;
        adv_buf.meas.hum_10   = hum_10;
        adv_buf.meas.ts_ms    = ts;

        ble_stop_scan();
        ble_start_adv();

        /* Stop advertising and resume scanning after 50 ms.               */
        k_work_reschedule(&adv_stop_work, K_MSEC(50));
    }

    /* Schedule the next measurement in 200 ms.                           */
    k_work_reschedule(&meas_work, K_MSEC(200));
}

/* =========================================================================
 *  Relay work handler  (system work queue)
 *
 *  Re-advertises a measurement received from a neighbour.
 *  Called from the BLE RX thread via k_work_submit when relay_pending
 *  is set by parse_data.
 * ========================================================================= */

static void relay_work_fn(struct k_work *w)
{
    relay_pending = false;

    if (atomic_get(&node_state) != STATE_RELAY) {
        return;
    }

    /* Copy the staged payload into the shared advertisement buffer.      */
    memset(adv_buf.raw, 0, sizeof(adv_buf.raw));
    memcpy(&adv_buf.meas, &relay_stage, sizeof(struct meas_msg));

    if (is_advertising) {
        /*
         * Already advertising (e.g. own measurement is still running):
         * just swap the payload and extend the window by resetting the
         * adv_stop timer to 50 ms from now.
         */
        bt_le_adv_update_data(ad, ARRAY_SIZE(ad), NULL, 0);
    } else {
        /* Not advertising: stop scanning and start. */
        ble_stop_scan();
        ble_start_adv();
    }

    /* Guarantee a full 50 ms window for the relay packet.                */
    k_work_reschedule(&adv_stop_work, K_MSEC(50));
}

/* =========================================================================
 *  Advertising-stop work handler  (fires 50 ms after advertising starts)
 * ========================================================================= */

static void adv_stop_work_fn(struct k_work *w)
{
    ble_stop_adv();

    /* Resume scanning on all non-SINK nodes. */
    if (atomic_get(&node_state) != STATE_SINK) {
        ble_start_scan();
    }
}

/* =========================================================================
 *  BLE scan – advertisement data parser
 *
 *  Called by bt_data_parse for each AD structure in a received packet.
 *  Returns false to stop parsing once we have handled our manufacturer
 *  data entry; returns true to keep parsing if the entry is irrelevant.
 * ========================================================================= */

static bool parse_data(struct bt_data *data, void *user_data)
{
    /* We only care about manufacturer-specific data. */
    if (data->type != BT_DATA_MANUFACTURER_DATA) {
        return true;
    }
    if (data->data_len < (uint8_t)sizeof(struct msg_hdr)) {
        return true;
    }

    const struct msg_hdr *hdr = (const struct msg_hdr *)data->data;

    /* Verify this is our lab protocol. */
    if (hdr->company_id != COMPANY_ID) {
        return true;
    }

    atomic_val_t state = atomic_get(&node_state);

    /* ── MSG_NET_FORM: join the network ─────────────────────────────── */
    if (hdr->msg_type == MSG_NET_FORM) {
        if (state != STATE_IDLE) {
            return false;   /* already in a network – nothing to do        */
        }
        if (data->data_len < (uint8_t)sizeof(struct net_form_msg)) {
            return false;   /* truncated packet                            */
        }

        /* Atomically transition from IDLE to RELAY. */
        if (!atomic_cas(&node_state, STATE_IDLE, STATE_RELAY)) {
            return false;   /* lost race with button_work                  */
        }

        memcpy(my_net_id, hdr->net_id, 3);
        gpio_pin_set_dt(&led, 1);   /* LED on: we have joined the network  */

        printk("[RELAY node=%u] Joined network  net_id=%02x:%02x:%02x\n",
               my_node_id, my_net_id[0], my_net_id[1], my_net_id[2]);

        /*
         * Re-advertise MSG_NET_FORM so the next hop can join.
         * We copy the received packet verbatim (same seq) so that the
         * next hop's dedup (if any) would still accept it.
         * Use a 500 ms window to give the next hop ample time.
         */
        memset(adv_buf.raw, 0, sizeof(adv_buf.raw));
        memcpy(&adv_buf.form, data->data,
               MIN(data->data_len, (uint8_t)sizeof(struct net_form_msg)));
        ble_start_adv();
        k_work_reschedule(&adv_stop_work, K_MSEC(500));

        /*
         * Start own measurement loop.  Delay by 600 ms so the NET_FORM
         * relay has finished before we start advertising measurements,
         * which would overwrite the NET_FORM in adv_buf.
         */
        k_work_reschedule(&meas_work, K_MSEC(600));

        return false;
    }

    /* ── MSG_MEAS: measurement from another node ─────────────────────── */
    if (hdr->msg_type == MSG_MEAS) {
        if (data->data_len < (uint8_t)sizeof(struct meas_msg)) {
            return false;   /* truncated packet                            */
        }
        const struct meas_msg *m = (const struct meas_msg *)data->data;

        /* Discard packets from a different network. */
        if (memcmp(hdr->net_id, my_net_id, 3) != 0) {
            return false;
        }
        /* Discard our own measurements that looped back via a relay. */
        if (m->node_id == my_node_id) {
            return false;
        }

        if (state == STATE_SINK) {
            /*
             * SINK: deduplicate and print the measurement.
             * rx_time_ms − m->ts_ms gives the end-to-end transmission
             * time assuming k_uptime_get() is synchronised across nodes.
             */
            if (dedup_check(sink_dedup, m->node_id, m->counter)) {
                uint32_t rx_ms = (uint32_t)k_uptime_get();
                print_measurement(m, rx_ms);
            }

        } else if (state == STATE_RELAY) {
            /*
             * RELAY: deduplicate and forward the measurement.
             * Stage the packet and hand off to relay_work (work-queue
             * context) where it is safe to call BLE API functions.
             */
            if (dedup_check(relay_dedup, m->node_id, m->counter)) {
                memcpy(&relay_stage, m, sizeof(relay_stage));
                relay_pending = true;
                k_work_submit(&relay_work);
            }
        }

        return false;
    }

    /* Unknown message type – keep parsing other AD structures. */
    return true;
}

/* =========================================================================
 *  BLE scan callback
 * ========================================================================= */

static void scan_cb(const bt_addr_le_t *addr, int8_t rssi,
                    uint8_t adv_type, struct net_buf_simple *buf)
{
    bt_data_parse(buf, parse_data, NULL);
}

/* =========================================================================
 *  main
 * ========================================================================= */

int main(void)
{
    printk("=== IoT Lab 2 Part 3: Measurement Collection ===\n");

    /* ── Configure LED ─────────────────────────────────────────────── */
    if (!device_is_ready(led.port)) {
        printk("[ERR] LED device not ready\n");
        return 0;
    }
    gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);

    /* ── Configure button ──────────────────────────────────────────── */
    if (!device_is_ready(btn.port)) {
        printk("[ERR] Button device not ready\n");
        return 0;
    }
    gpio_pin_configure_dt(&btn, GPIO_INPUT);
    gpio_pin_interrupt_configure_dt(&btn, GPIO_INT_EDGE_BOTH);
    gpio_init_callback(&button_cb_data, button_isr, BIT(btn.pin));
    gpio_add_callback(btn.port, &button_cb_data);

    /* ── Initialise Bluetooth ──────────────────────────────────────── */
    int err = bt_enable(NULL);
    if (err) {
        printk("[ERR] BT init failed (err=%d)\n", err);
        return 0;
    }

    /*
     * Cache the low byte of our own BLE address as the node identifier.
     * This is done once here so it is available even before the network
     * forms (relay nodes need it as soon as they join).
     */
    {
        bt_addr_le_t own;
        size_t cnt = 1U;
        bt_id_get(&own, &cnt);
        my_node_id = own.a.val[0];
    }

    printk("[node=%u] Bluetooth ready\n", my_node_id);

    /* ── Start passive scanning ─────────────────────────────────────── */
    err = bt_le_scan_start(&scan_param, scan_cb);
    if (err) {
        printk("[ERR] Scan start failed (err=%d)\n", err);
        return 0;
    }
    is_scanning = true;
    printk("[node=%u] Idle – press SW0 to become SINK\n", my_node_id);

    /*
     * Main thread: polls the button every 50 ms.
     *
     * gpio0 OnGPIO 11 false/true in the Renode Monitor updates the GPIO
     * input register directly, but GPIOTE interrupts are unreliable in
     * the emulator.  Polling the register here is the reliable path for
     * Renode.  On real hardware the ISR fires immediately instead.
     */
    int prev_btn = -1;
    while (1) {
        int cur = gpio_pin_get_dt(&btn);
        if (cur >= 0 && cur != prev_btn) {
            prev_btn   = cur;
            btn_active = cur;
            k_work_submit(&button_work);
        }
        k_msleep(50);
    }

    return 0;
}

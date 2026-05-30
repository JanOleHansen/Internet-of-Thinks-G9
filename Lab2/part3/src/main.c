// To see examplecode:https://github.com/platformio/platform-nordicnrf52/
// blob/master/examples/zephyr-blink/src/main.c
#include <zephyr.h>
#include <device.h>
#include <syscalls/device.h>
#include <devicetree.h>
#include <drivers/gpio.h>
#include <sys/printk.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <sys/util.h>
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <random/rand32.h>
#include <soc.h>
#include <bluetooth/addr.h>
#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>

#define SLEEP_TIME_MS 200
#define ADV_TIME_MS 60
#define FORM_REPETITIONS 5
#define MEASUREMENT_REPETITIONS 1
#define COMPANY_ID 0xFFFF
#define TEMP_MIN_TENTHS -250
#define TEMP_MAX_TENTHS 2000
#define HUMIDITY_MIN_TENTHS 0
#define HUMIDITY_MAX_TENTHS 1000
#define MAX_SEEN_PACKETS 256

// Now define the button0 in the same way as the led0
#define BUTTON0_NODE DT_ALIAS(sw0)
/*
BUTTON0 = should be gpio0
BUTTON_PIN = should be 11
BUTTON_FLAGS = should be (GPIO_PULL_UP | GPIO_ACTIVE_LOW)
*/
#if DT_NODE_HAS_STATUS(BUTTON0_NODE, okay)
#define BUTTON0 DT_GPIO_LABEL(BUTTON0_NODE, gpios)
#define BUTTON_PIN DT_GPIO_PIN(BUTTON0_NODE, gpios)
#define BUTTON_FLAGS DT_GPIO_FLAGS(BUTTON0_NODE, gpios)
#else
#error "Unsupported board: button0 devicetree alias is not defined"
#define BUTTON0 ""
#define BUTTON_PIN 0
#define BUTTON_FLAGS 0
#endif

// Define parameters for multi hop connections
bool network_formed = false;
bool is_initiator = false;
bool form_network_sending = false;
bool create_measures = false;

bool forward_form_pending = false;
bool forward_measurement_pending = false;

static const struct bt_le_adv_param adv_param = {
    .options = BT_LE_ADV_OPT_NONE,
    .interval_max = 0x0020,
    .interval_min = 0x0020,
    .peer = NULL,
};
/*
    BT_LE_ADV_PARAM(BT_LE_ADV_OPT_NONE,
                    0x0020,  // 20 ms min
                    0x0020,  // 20 ms max
                    NULL);
*/
static const struct bt_le_scan_param scan_param = {
    .type = BT_LE_SCAN_TYPE_PASSIVE,
    .options = BT_LE_SCAN_OPT_NONE,
    .interval = 0x0010, // 10 ms
    .window = 0x0010,   // 10 ms, also fast dauerhaft scannen
};

uint8_t local_node_id = 0;
uint8_t measurement_count = 0;
uint8_t seq_id = 1;      // Starting value of sequence
uint8_t ttl_max = 3;     // max. 3 Hops

static void scan_callback(const bt_addr_le_t *addr,
                          int8_t rssi,
                          uint8_t adv_type,
                          struct net_buf_simple *buf);

// Define different message types
enum msg_type {
	MSG_FORM_NETWORK = 0, // tells other devices to form a network
	MSG_MEASUREMENT = 1,  // tells other devices that measurements were sent
};

// Struct for packet header => included in both structs below
struct packet_header {
	uint16_t company_id;
	uint8_t msg_type;
	uint8_t origin_id;
	uint8_t seq;
	uint8_t ttl;
} __packed;

// Struct for forming network command
struct form_packet {
	struct packet_header header;
} __packed;

// Struct for measurements
struct measurement_packet {
	struct packet_header header;
	uint8_t node_id;              // ID of node
	uint8_t measurement_counter;  // how many measurements were generated - 7
	int16_t temp;                  // temperature in 0.1 degree Celsius - 8
	uint16_t humidity;            // humidity in 0.1 percent - 10
	uint32_t timestamp;           // timestamp of measurement in ms - 12
} __packed;

// define global packets for calling send functions outside of callback function
struct form_packet pending_form;
struct measurement_packet pending_measurement;

// Struct for storing tuple values in received_seqs array
struct tuple_received_packets {
    uint8_t msg_type;
    uint8_t origin_id;
    uint8_t seq;
};

// list of already received sequences
struct tuple_received_packets received_seqs[256];
size_t counter = 0;

static int32_t random_range_tenths(int32_t min, int32_t max){
    uint32_t span = (uint32_t)(max - min + 1);
    return min + (int32_t)(sys_rand32_get() % span);
}

static uint16_t read_u16_le(const uint8_t *bytes){
    return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
}

static int16_t read_i16_le(const uint8_t *bytes){
    return (int16_t)read_u16_le(bytes);
}

static uint32_t read_u32_le(const uint8_t *bytes){
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

// function to save packet in memory
static void remember_packet(uint8_t msg_type, uint8_t origin_id, uint8_t seq){
    struct tuple_received_packets tuple = {
        .msg_type = msg_type,
        .origin_id = origin_id,
        .seq = seq,
    };
    received_seqs[counter] = tuple;
    counter++;
}

// function for sink to print his own measurements
static void print_measurement_line(struct measurement_packet *measurements){
     // Format: nodeID;measurement-counter;temp;humidity;timestamp;tx-time
    uint32_t tx_time = k_uptime_get_32() - measurements->timestamp;
    int frac = measurements->temp % 10;
    if (frac < 0) {
        frac = -frac;
    }
    if (measurements->temp < 0 && measurements->temp / 10 == 0){
     printk("%u;%u;-%d.%d;%u.%u;%u;%u\n",measurements->header.origin_id,
        measurements->measurement_counter,
        measurements->temp / 10, 
        frac, 
        measurements->humidity / 10, 
        measurements->humidity % 10, 
        measurements->timestamp, tx_time);
    } else {
        printk("%u;%u;%d.%d;%u.%u;%u;%u\n",measurements->header.origin_id,
        measurements->measurement_counter,
        measurements->temp / 10, 
        frac, 
        measurements->humidity / 10, 
        measurements->humidity % 10, 
        measurements->timestamp, tx_time);
    }
}

// function to detect if received seq_id already received at one time
bool already_received(uint8_t msg_type, uint8_t origin_id, uint8_t seq){
	for (size_t i = 0; i < counter; i++){
        struct tuple_received_packets tuple = received_seqs[i];
        if (tuple.msg_type == msg_type && tuple.origin_id == origin_id && tuple.seq == seq){
			return true;
		}
	}
	return false;
}

// Create packets for command to form network
static void sendFormNetwork(uint8_t origin_id, uint8_t seq, uint8_t ttl){
    bt_le_scan_stop();
    struct packet_header header = {
        .company_id = 0xFFFF,
        .msg_type = MSG_FORM_NETWORK,
        .origin_id = origin_id,
        .seq = seq,
        .ttl = ttl
    };
    struct form_packet form = {
        .header = header
    };
    // Set Advertisement data with manufacturer specific data
	const struct bt_data ad[] = {
		BT_DATA(BT_DATA_MANUFACTURER_DATA, &form, sizeof(form))
	};
	/* Start non-connectable advertising (broadcast); no scan response */
	uint8_t err = bt_le_adv_start(&adv_param, ad, ARRAY_SIZE(ad), NULL, 0);
	if (err) {
		printk("Advertising failed to start (err %d)\n", err);
		return;
	}
	printk("Sending type=%u seq=%u ttl=%u\n", MSG_FORM_NETWORK, seq, ttl);
	// Advertise for 30 MS
	k_msleep(ADV_TIME_MS);
	/* Stop advertising */
	int errStop = bt_le_adv_stop();
	err = bt_le_scan_start(&scan_param, scan_callback);
    while(err) {
        printk("New Scan failed, i will try again...\n");
        err = bt_le_scan_start(&scan_param, scan_callback);
    }
    if (errStop) {
		printk("Advertising failed to stop (err %d)\n", err);
		return;
	}
}
static void sendMeasurements(struct measurement_packet *measurement){
    bt_le_scan_stop();
    // Set Advertisement data with manufacturer specific data
	const struct bt_data ad[] = {
		BT_DATA(BT_DATA_MANUFACTURER_DATA, measurement, sizeof(*measurement))
	};
	/* Start non-connectable advertising (broadcast); no scan response */
	uint8_t err = bt_le_adv_start(&adv_param, ad, ARRAY_SIZE(ad), NULL, 0);
	if (err) {
		printk("Advertising failed to start (err %d)\n", err);
		return;
	}
	printk("Sending type=%u seq=%u ttl=%u\n", MSG_MEASUREMENT, measurement->header.seq, measurement->header.ttl);
	// Advertise for 30 MS
	k_msleep(ADV_TIME_MS);
	/* Stop advertising */
	int errStop = bt_le_adv_stop();
	err = bt_le_scan_start(&scan_param, scan_callback);
    while(err) {
        printk("New Scan failed, i will try again...\n");
        err = bt_le_scan_start(&scan_param, scan_callback);
    }
    if (errStop) {
		printk("Advertising failed to stop (err %d)\n", err);
		return;
	}
}
// Callback function to parse and print received raw advertising data
bool parse_adv_data(struct bt_data *data, void *user_data) {
    // Print the raw advertising data in hexadecimal format
    // if manufacturer specific data is received, print the data type, length, and content
    if (data->type == BT_DATA_MANUFACTURER_DATA &&
        data->data_len >= sizeof(struct packet_header) &&
        read_u16_le(data->data) == COMPANY_ID) {
		uint8_t msg_type = data->data[2];
        uint8_t origin_id = data->data[3];
        uint8_t rec_seq = data->data[4];
		if (!already_received(msg_type, origin_id, rec_seq)){ // Only process unseen data
			remember_packet(msg_type,origin_id,rec_seq);
            printk("Received type=%u seq=%u ttl=%u from node=%d\n", msg_type, rec_seq, data->data[5], origin_id);
			if (msg_type == MSG_FORM_NETWORK){
				network_formed = true;
				is_initiator = false;
                create_measures = true;
                if (data->data[5] > 0) {
                    pending_form.header.company_id = COMPANY_ID;
                    pending_form.header.msg_type = MSG_FORM_NETWORK;
                    pending_form.header.origin_id = origin_id;
                    pending_form.header.seq = rec_seq;
                    pending_form.header.ttl = data->data[5] - 1;
                    forward_form_pending = true;
                }
			} else if (msg_type == MSG_MEASUREMENT){
                if (data->data_len < sizeof(struct measurement_packet) ||
                    data->data[5] == 0) {
                    return true;
                }
                struct packet_header header = {
                    .company_id = COMPANY_ID,
                    .msg_type = MSG_MEASUREMENT,
                    .origin_id = origin_id,
                    .seq = rec_seq,
                    .ttl = data->data[5]-1,
                };
                struct measurement_packet measurement = {
                    .header = header,
                    .node_id = data->data[6],
                    .measurement_counter = data->data[7],
                    .temp = read_i16_le(&data->data[8]),
                    .humidity = read_u16_le(&data->data[10]),
                    .timestamp = read_u32_le(&data->data[12]),
                };
                pending_measurement = measurement;
                forward_measurement_pending = true;
			} else {
				printk("Invalid message type\n");
				return false;
			}
		}
	}
    return true; // Return true to continue parsing other data fields
}

bool parse_measurements_initiator(struct bt_data *data, void *user_data) {
    if (data->type == BT_DATA_MANUFACTURER_DATA &&
        data->data_len >= sizeof(struct packet_header) &&
        read_u16_le(data->data) == COMPANY_ID) {
        uint8_t msg_type = data->data[2];
        uint8_t origin_id = data->data[3];
        uint8_t rec_seq = data->data[4];
		if (!already_received(msg_type, origin_id, rec_seq)){ // Only process unseen data
            remember_packet(msg_type,origin_id,rec_seq);
            if (msg_type == MSG_MEASUREMENT){ // Only print measurements in specific format
                if (data->data_len < sizeof(struct measurement_packet)) {
                    return true;
                }
                // Format: nodeID;measurement-counter;temp;humidity;timestamp;tx-time
                int16_t temp = read_i16_le(&data->data[8]);
                uint16_t humidity = read_u16_le(&data->data[10]);
                uint32_t timestamp = read_u32_le(&data->data[12]);
                uint32_t tx_time = k_uptime_get_32() - timestamp;
                int frac = temp % 10;
                if (frac < 0) {
                    frac = -frac;
                }
                if (temp < 0 && temp /10 == 0){
                    printk("%u;%u;-%d.%d;%u.%u;%u;%u\n",origin_id,data->data[7],temp / 10, frac, humidity / 10, humidity % 10, timestamp, tx_time);
                } else {
                    printk("%u;%u;%d.%d;%u.%u;%u;%u\n",origin_id,data->data[7],temp / 10, frac, humidity / 10, humidity % 10, timestamp, tx_time);
                }
            }
        }
    }
    return true;
}

// Callback function to handle received advertising data
static void scan_callback(const bt_addr_le_t *addr,
			  int8_t rssi,
			  uint8_t adv_type,
			  struct net_buf_simple *buf)
{
	if (is_initiator) {
        // sink only reads measurements
		bt_data_parse(buf, parse_measurements_initiator, NULL);
	} else {
        // other devices can read form network command or measurements
		bt_data_parse(buf, parse_adv_data, NULL);
	}
}

// Callback handler Function for button interrupt
static void button_cb(const struct device *port,
		      struct gpio_callback *cb,
		      gpio_port_pins_t pins)
{
    if ((pins & BIT(BUTTON_PIN)) && !network_formed) {
		// button press has only one time an effect
        // No effect anymore when network is formed
        network_formed = true;
		is_initiator = true;
		form_network_sending = true;
		printk("BUTTON first press, becoming sink\n");
	}
}

// GPIO callback structure
struct gpio_callback button_cb_struct;
void main(void)
{
    // Enable Bluetooth for device
	int err = bt_enable(NULL);

	if (err) {
		printk("Bluetooth init failed (err %d)\n", err);
		return;
	}

    // Give device a unique node id based on bluetooth address (last Byte)
    bt_addr_le_t addr;
    size_t count = 1;
    bt_id_get(&addr, &count);
    local_node_id = addr.a.val[0];
    
	printk("Bluetooth initialized, node_id=%u addr0=0x%08x addr1=0x%08x\n",
	       local_node_id, NRF_FICR->DEVICEADDR[0], NRF_FICR->DEVICEADDR[1]);
    
    // Start Scan
	err = bt_le_scan_start(&scan_param, scan_callback);
	if (err) {
		printk("Scanning failed to start (err %d)\n", err);
		return;
	}

    // Retrieve device structure for button
    const struct device *dev_button = device_get_binding(BUTTON0);
	if (dev_button == NULL) {
		printk("Device BUTTON0 wasn't found!\n");
		return;
	}

    // Configure Button as GPIO Input
    int ret_button = gpio_pin_configure(dev_button, BUTTON_PIN,
					    GPIO_INPUT | BUTTON_FLAGS);
	if (ret_button != 0) {
		printk("Configuration of Button Pin failed!\n");
		return;
	}
    
    // Button Press should activate interrupt
	int ret_interrupt_button = gpio_pin_interrupt_configure(dev_button,
								BUTTON_PIN,
								GPIO_INT_EDGE_BOTH);
	if (ret_interrupt_button != 0) {
		printk("Configuration of interrupt failed!\n");
		return;
	}

    // Initialize callback struct and add button to this struct to handle button presses
	gpio_init_callback(&button_cb_struct, button_cb, BIT(BUTTON_PIN));
	gpio_add_callback(dev_button, &button_cb_struct);

    // Set up offset for each node creating their measurements to minimize collisions
	uint32_t next_measurement_time = k_uptime_get_32() + (local_node_id % 4) * 50;

	while (1) {
        if (forward_form_pending) {
            forward_form_pending = false;
            sendFormNetwork(pending_form.header.origin_id,
                            pending_form.header.seq,
                            pending_form.header.ttl);
        }
        if (forward_measurement_pending) {
            forward_measurement_pending = false;
            sendMeasurements(&pending_measurement);
        }
		if (form_network_sending) {
            // sink sends "form network" command
			form_network_sending = false;
            sendFormNetwork(local_node_id, seq_id, ttl_max);
			create_measures = true;
		}

		if (create_measures &&
		    (int32_t)(k_uptime_get_32() - next_measurement_time) >= 0) {
			next_measurement_time = k_uptime_get_32() + SLEEP_TIME_MS;
            // create measurements
			printk("Im creating my own measurements...\n");
			struct measurement_packet measurement = {
				.header = {
					.company_id = COMPANY_ID,
					.msg_type = MSG_MEASUREMENT,
					.origin_id = local_node_id,
					.seq = seq_id++,
					.ttl = ttl_max,
				},
				.node_id = local_node_id,
				.measurement_counter = measurement_count++,
				.temp = (int16_t)random_range_tenths(TEMP_MIN_TENTHS,
								     TEMP_MAX_TENTHS),
				.humidity = (uint16_t)random_range_tenths(HUMIDITY_MIN_TENTHS,
									  HUMIDITY_MAX_TENTHS),
				.timestamp = k_uptime_get_32(),
			};
            // Save packet to skip own packets that are coming back
            remember_packet(measurement.header.msg_type,
						measurement.header.origin_id,
						measurement.header.seq);
			if (is_initiator) {
                // sink prints its own measurement
				print_measurement_line(&measurement);
			} else {
                // other devices send their measurement through network to the sink
				sendMeasurements(&measurement);
			}
		} else {
			k_msleep(1);
		}
	}
}

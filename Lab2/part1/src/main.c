// To see examplecode:https://github.com/platformio/platform-nordicnrf52/
// blob/master/examples/zephyr-blink/src/main.c 
#include <zephyr.h>
#include <device.h>
#include <syscalls/device.h>
#include <devicetree.h>
#include <drivers/gpio.h>
#include <sys/printk.h>
#include <stdint.h>
#include <zephyr/types.h>
#include <stddef.h>
#include <sys/util.h>
#include <bluetooth/addr.h>
#include <bluetooth/bluetooth.h>
#include <bluetooth/addr.h>
#include <bluetooth/hci.h>

#define SLEEP_TIME_MS   500

/* The devicetree node identifier for the "led0" alias. 
led0 is defined under .platformio\packages\framework-zephyr\boards\arm\
nrf52840dk_nrf52840\nrf52840dk_nrf52840.dts ; this is the devicetree source,
that defines the devices on the board*/
#define LED0_NODE DT_ALIAS(led0)

/* Get all the components of led0, that are defined under the component gpios
in the dts file of led0; 
DT_GPIO_LABEL gets the gpio controller (group of pins) of the led, 
DT_GPIO_PIN gets the pin of the led within the gpio controller,
DT_GPIO_FLAGS gets the flags of the pin 
LED0 = should be gpio0 
LED_PIN = should be 13
LED_FLAGS = should be GPIO_ACTIVE_LOW; meaning, that led is on when energy is low*/
#if DT_NODE_HAS_STATUS(LED0_NODE, okay)
#define LED0	DT_GPIO_LABEL(LED0_NODE, gpios) 
#define LED_PIN	DT_GPIO_PIN(LED0_NODE, gpios)		
#define LED_FLAGS	DT_GPIO_FLAGS(LED0_NODE, gpios)
#else
/* A build error here means your board isn't set up to blink an LED. */
#error "Unsupported board: led0 devicetree alias is not defined"
#define LED0	""
#define LED_PIN	0
#define LED_FLAGS	0
#endif

// Now define the button0 in the same way as the led0
#define BUTTON0_NODE DT_ALIAS(sw0)
/*
BUTTON0 = should be gpio0
BUTTON_PIN = should be 11
BUTTON_FLAGS = should be (GPIO_PULL_UP | GPIO_ACTIVE_LOW)
*/
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

// Define parameters for multi hop connections
bool network_formed = false;
bool is_initiator = false;
bool form_network_sending = false;
bool button_sending = false;

// Define the advertising data
// Struct for manufacturer data
struct mfg_data {
    uint16_t company_id; 	// Company Identifier Code (2 bytes)
    uint8_t type; 			// type of message (1 byte)
	uint8_t seq; 			// Sequence ID (1 byte; only 256 possible sequences) 
    uint8_t ttl;			// Time to live of data(1 byte)
};

uint8_t seq_id = 1; 		// Starting value of sequence
uint8_t ttl_max = 3;		// max. 3 Hops

// Define different message types
enum msg_type {
    MSG_FORM_NETWORK = 0, 		// tells other devices to form a network
    MSG_LED_ON = 1,				// tells other devices to turn on LED
    MSG_LED_OFF = 2,			// tells other devices to turn off LED
};

// list of already received sequences
uint8_t received_seqs[256];
size_t counter = 0;

// function to detect if received seq_id already received at one time
bool already_received(uint8_t seq_id){
	for (size_t i = 0; i <= counter; i++){
		if (received_seqs[i] == seq_id){
			return true;
		}
	}
	return false;
}
void sendAdvertisingData(uint8_t type, uint8_t seq, uint8_t ttl){
	if (ttl > 0){
		struct mfg_data my_mfg_data = {
			.company_id = 0xFFFF,
			.type = type,
			.seq = seq,
			.ttl = ttl-1,
		};
		// Set Advertisement data with manufacturer specific data
		const struct bt_data ad[] = {
			BT_DATA(BT_DATA_MANUFACTURER_DATA, &my_mfg_data, sizeof(my_mfg_data))
		};
		/* Start non-connectable advertising (broadcast); no scan response */
		uint8_t err = bt_le_adv_start(BT_LE_ADV_NCONN, ad, ARRAY_SIZE(ad), NULL, 0);
		if (err) {
			printk("Advertising failed to start (err %d)\n", err);
			return;
		}
		printk("TX type=%u seq=%u ttl=%u\n", type, seq, ttl);
		// Advertise for 0.5 seconds
		k_msleep(SLEEP_TIME_MS);
		/* Stop advertising */
		err = bt_le_adv_stop();
		if (err) {
			printk("Advertising failed to stop (err %d)\n", err);
			return;
		}
	} else {
		printk("TTL just expired!\n");
		return;
	}
}
// Callback function to parse and print received raw advertising data
bool parse_adv_data(struct bt_data *data, void *user_data) {
    // Print the raw advertising data in hexadecimal format
    // if manufacturer specific data is received, print the data type, length, and content
    if (data->type == BT_DATA_MANUFACTURER_DATA) {
		uint8_t received_seq = data->data[3];
		if (!already_received(received_seq)){ // Only process unseen data
			printk("Data type: 0x%02X, Data length: %d, Data: \n", data->type, data->data_len);
			received_seqs[counter] = received_seq;
			counter++;
			uint8_t message_type = data->data[2];
			if (message_type == MSG_FORM_NETWORK){
				printk("Received the command to form a network!\n");
				network_formed = true;
				is_initiator = false;
			} else if (message_type == MSG_LED_OFF){
				printk("Received the command to turn off the LED\n");
				button_is_on = false;
			} else if (message_type == MSG_LED_ON){
				printk("Received the command to turn on the LED\n");
				button_is_on = true;
			} else {
				printk("Invalid message type\n");
				return false;
			}
			printk("RX type=%u seq=%u ttl=%u\n",
      			 message_type, received_seq, data->data[4]);
			// Send the message to other devices if ttl is not expired
			sendAdvertisingData(data->data[2], data->data[3], data->data[4]);
		}
	}
    return true; // Return true to continue parsing other data fields
}

// Callback function to handle received advertising data
void scan_callback(
        const bt_addr_le_t *addr, 
        int8_t rssi, 
        uint8_t adv_type, 
        struct net_buf_simple *buf) {
			char addr_str[BT_ADDR_LE_STR_LEN];

			bt_addr_le_to_str(addr, addr_str, sizeof(addr_str));
			printk("Received advertising data from %s (RSSI %d, type %u)\n",
			       addr_str, rssi, adv_type);

            if (!is_initiator){ // The initiator doesn't listen to commands!
				bt_data_parse(buf, parse_adv_data, NULL);
			}

};

// Callback handler Function for button interrupt
void button_cb(const struct device *port, struct gpio_callback *cb, gpio_port_pins_t pins){
	if (pins & BIT(BUTTON_PIN)){
		if (!network_formed){
			printk("Button was pressed for the first time, now i'm the initiator!\n");
			network_formed = true;
			is_initiator = true;
			// Start advertising Form Network Message
			form_network_sending = true;
		} else { // Network is already formed; Now send LED on/off commands
			if (is_initiator){
				uint8_t val = gpio_pin_get(port, BUTTON_PIN);
				if (val == 0) {
					// Active low, so pressed = 0
					printk("Button was pressed!\n");
					button_is_on = true;
					button_sending = true;
				} else {
					printk("Button was released!\n");
					button_is_on = false;
					button_sending = true;
				}
			}
		}
	}
	return;
}

// GPIO callback structure
struct gpio_callback button_cb_struct;

void main(void)
{
	// Initialize the Bluetooth Subsystem (synchronous)
    int err = bt_enable(NULL);
    if (err) {
        printk("Bluetooth init failed (err %d)\n", err);
        return;
    }
    printk("Bluetooth initialized\n");
	
	// Scan for possible incoming advertising data
	err = bt_le_scan_start(BT_LE_SCAN_PASSIVE, scan_callback);
    if (err) {
        printk("Scanning failed to start (err %d)\n", err);
        return;
    }

    
	// Configure GPIO Pins
	const struct device *dev_led;
	const struct device *dev_button;
	bool led_is_on = false;
	int ret_led;
	int ret_button;
	int ret_interrupt_button;

	// search for struct of gpio0 device
	dev_led = device_get_binding(LED0);
	if (dev_led == NULL) {
		printk("Device LED0 wasn't found!\n");
		return;
	}

	// search for struct of gpio0 device
	dev_button = device_get_binding(BUTTON0);
	if (dev_button == NULL) {
		printk("Device BUTTON0 wasn't found!\n");
		return;
	}
	// configure for gpio0 the pin 13 as an gpio output for the led
	ret_led = gpio_pin_configure(dev_led, LED_PIN, GPIO_OUTPUT_ACTIVE | LED_FLAGS);
	if (ret_led < 0) {
		printk("Configuration of LED Pin failed!\n");
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
	

	while (1) {
		// Activate the LED when the button is pressed
		if (button_is_on){
			// activate the led pin
			led_is_on = true;
			int led_pin = gpio_pin_set(dev_led, LED_PIN, (int)led_is_on);
			if (led_pin == 0){
				printk("LED is on!\n");
			} else {
				printk("Setting the pin wasn't successful\n");
			}
			// Advertise commands to the other devices
			if (button_sending){
				button_sending = false;
				printk("I'm sending a commando to turn on the LED\n");
				sendAdvertisingData(MSG_LED_ON, seq_id, ttl_max+1);
				seq_id++;
			} else if (form_network_sending){
				form_network_sending = false;
				printk("I'm sending a commando to form a network\n");
				sendAdvertisingData(MSG_FORM_NETWORK, seq_id, ttl_max+1);
				seq_id++;
			}
		} else {
			// deactivate the led pin
			led_is_on = false;
			int led_pin = gpio_pin_set(dev_led, LED_PIN, (int)led_is_on);
			if (led_pin == 0){
				printk("LED is off\n");
			} else {
				printk("Setting the pin wasn't successful\n");
			}
			// Advertise commands to the other devices
			if (button_sending){
				button_sending = false;
				printk("I'm sending a commando to turn off the LED\n");
				sendAdvertisingData(MSG_LED_OFF, seq_id, ttl_max+1);
				seq_id++;
			} else if (form_network_sending){
				form_network_sending = false;
				printk("I'm sending a commando to form a network\n");
				sendAdvertisingData(MSG_FORM_NETWORK, seq_id, ttl_max+1);
				seq_id++;
			}
		}
		k_msleep(SLEEP_TIME_MS);
	}
	return;
}

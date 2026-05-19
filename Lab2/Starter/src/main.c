// To see examplecode:https://github.com/platformio/platform-nordicnrf52/
// blob/master/examples/zephyr-blink/src/main.c 
#include <zephyr.h>
#include <device.h>
#include <syscalls/device.h>
#include <devicetree.h>
#include <drivers/gpio.h>

/* 1000 msec = 1 sec */
#define SLEEP_TIME_MS   1000

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

// Callback handler Function for button interrupt
void button_cb(const struct device *port, struct gpio_callback *cb, gpio_port_pins_t pins){
	if (pins & BIT(BUTTON_PIN)){
		if (button_is_on == false){
			printk("Button was pressed!\n");
			button_is_on = true;
		} else {
			printk("Button was released!\n");
			button_is_on = false;
		}
	}
	return;
}

// GPIO callback structure
struct gpio_callback button_cb_struct;

void main(void)
{
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
		} else {
			// deactivate the led pin
			led_is_on = false;
			int led_pin = gpio_pin_set(dev_led, LED_PIN, (int)led_is_on);
			if (led_pin == 0){
				printk("LED is off\n");
			} else {
				printk("Setting the pin wasn't successful\n");
			}
		}
		k_msleep(SLEEP_TIME_MS);
	}
}

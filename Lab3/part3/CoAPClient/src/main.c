#include <zephyr.h>
#include <device.h>
#include <syscalls/device.h>
#include <devicetree.h>
#include <drivers/gpio.h>
#include <sys/printk.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <net/socket.h>
#include <net/net_if.h>
#include <net/net_config.h>
#include <net/net_mgmt.h>
#include <net/ieee802154_mgmt.h>
#include <net/coap.h>

// Define the button0
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

#define COAP_PORT 5683
#define SERVER_ADDR "fd00::1"
#define RECV_BUF_SIZE 128
#define RESP_BUF_SIZE 128

static uint16_t pan_id = 0x1234;
static uint16_t channel = 15;
static volatile bool action_state = false;


static int setup_network(void)
{
    // Get network interface
    struct net_if *iface = net_if_get_default();
    int ret;
    if (!iface) {
        printk("No network interface found!\n");
        return -ENODEV;
    }
    printk("Network interface found: %p\n", iface);

    // set PAN ID and channel for 802.15.4
    ret = net_mgmt(NET_REQUEST_IEEE802154_SET_PAN_ID,
                   iface, &pan_id, sizeof(pan_id));
    if (ret < 0) {
        printk("Cannot set PAN ID: %d\n", ret);
        return ret;
    }

    ret = net_mgmt(NET_REQUEST_IEEE802154_SET_CHANNEL,
                   iface, &channel, sizeof(channel));
    if (ret < 0) {
        printk("Cannot set channel: %d\n", ret);
        return ret;
    }

    // Bring the interface up and initialize network configuration
    ret = net_if_up(iface);
    if (ret < 0) {
        printk("Cannot bring interface up: %d\n", ret);
        return ret;
    }

    ret = net_config_init_by_iface(iface,
                                   "CoAP Client 802.15.4",
                                   NET_CONFIG_NEED_IPV6,
                                   10000);
    if (ret < 0) {
        printk("Network config failed: %d\n", ret);
        return ret;
    }

    printk("Network ready\n");
    return 0;
}

// Callback handler Function for button interrupt
void button_cb(const struct device *port, struct gpio_callback *cb, gpio_port_pins_t pins){
	if (pins & BIT(BUTTON_PIN)){
		printk("Button was pressed, action_state set to true!\n");
		action_state = true;
	}
	return;
}

// Create CoAP Packet
static int create_coap_request(struct coap_packet *request,
                               uint8_t *buf,
                               size_t buf_len,
                               bool action,
                               uint16_t msg_id,
                               uint8_t *token,
                               uint8_t token_len)
{
    int ret;
    const char *path;

    ret = coap_packet_init(request,
                           buf,
                           buf_len,
                           COAP_VERSION_1,
                           COAP_TYPE_CON,
                           token_len,
                           token,
                           action ? COAP_METHOD_POST : COAP_METHOD_GET,
                           msg_id);
    if (ret < 0) {
        return ret;
    }

    path = action ? "action" : "sensor";

    ret = coap_packet_append_option(request,
                                    COAP_OPTION_URI_PATH,
                                    (uint8_t *)path,
                                    strlen(path));
    if (ret < 0) {
        return ret;
    }

    if (action) {
        const char *payload = "toggle";

        ret = coap_packet_append_payload_marker(request);
        if (ret < 0) {
            return ret;
        }

        ret = coap_packet_append_payload(request,
                                         (uint8_t *)payload,
                                         strlen(payload));
        if (ret < 0) {
            return ret;
        }
    }

    return 0;
}

// GPIO callback structure
struct gpio_callback button_cb_struct;

void main(void)
{
	printk("MAIN STARTED\n");

    // Get Button device
    const struct device *dev_button = device_get_binding(BUTTON0);
	if (!dev_button) {
		printk("Button device not found\n");
		return;
	}
    // Configure for gpio0 the pin 11 as an gpio input for the button
	int ret_button = gpio_pin_configure(dev_button, BUTTON_PIN, GPIO_INPUT | BUTTON_FLAGS);
	if (ret_button != 0) {
		printk("Configuration of Button Pin failed!\n");
		return;
	}
    // Configure for gpio0 the pin 11 as an interrupt pin
	int ret_interrupt_button = gpio_pin_interrupt_configure(dev_button, BUTTON_PIN, GPIO_INT_EDGE_TO_ACTIVE);
	if (ret_interrupt_button != 0) {
		printk("Configuration of interrupt failed!\n");
		return;
	}
    // Initialize GPIO callback structure for interrupts
	gpio_init_callback(&button_cb_struct, button_cb, BIT(BUTTON_PIN));
	gpio_add_callback(dev_button, &button_cb_struct);

	// Initialize network interface and configuration
	int sock;
    int ret;

    ret = setup_network();
    if (ret < 0) {
        return;
    }

	// Create UDP socket
    sock = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        printk("socket() failed: %d\n", errno);
        return;
    }

	// Set timeout for receiving responses
	struct timeval timeout = {
		.tv_sec = 2,
		.tv_usec = 0,
	};
	ret = setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
			&timeout, sizeof(timeout));
	if (ret < 0) {
		printk("setsockopt() failed: %d\n", errno);
	}
	// Define Server address
	struct sockaddr_in6 server_addr;
	memset(&server_addr, 0, sizeof(server_addr));
	server_addr.sin6_family = AF_INET6;
	server_addr.sin6_port = htons(COAP_PORT);
	// Convert IPv6 address from text to binary form
	ret = inet_pton(AF_INET6, SERVER_ADDR, &server_addr.sin6_addr);
	if (ret != 1) {
		printk("inet_pton() failed\n");
		return;
	}

	while(1){
		// Create CoAP request based on button state
		uint8_t send_buf[128];
		struct coap_packet request;
		uint16_t msg_id = coap_next_id();
		uint8_t token[2] = {
			msg_id >> 8,
			msg_id & 0xff
		};

		bool send_action = action_state;
		action_state = false;

		ret = create_coap_request(&request,
								send_buf,
								sizeof(send_buf),
								send_action,
								msg_id,
								token,
								sizeof(token));
		if (ret < 0) {
			printk("create_coap_request failed: %d\n", ret);
			continue;
		}
		// Send CoAP request to server
		printk("Sending CoAP request to server...\n");
		ret = sendto(sock,
             send_buf,
             request.offset,
             0,
             (struct sockaddr *)&server_addr,
             sizeof(server_addr));
		if (ret < 0) {
			printk("sendto response failed: %d\n", errno);
			k_sleep(K_SECONDS(5));
			continue;
		}
		// Wait for CoAP response from server
		uint8_t recv_buf[RECV_BUF_SIZE];
		printk("Waiting for CoAP response...\n");
		struct sockaddr_in6 server_response_addr;
		socklen_t server_len = sizeof(server_response_addr);
        struct coap_packet response;
		ret = recvfrom(sock,
                       recv_buf,
                       sizeof(recv_buf),
                       0,
                       (struct sockaddr *)&server_response_addr,
                       &server_len);

        if (ret < 0) {
            printk("recvfrom() failed: %d\n", errno);
			k_sleep(K_SECONDS(5));
            continue;
        }

        printk("Received %d bytes\n", ret);

        // Parse CoAP response
        ret = coap_packet_parse(&response, recv_buf, ret, NULL, 0);
        if (ret < 0) {
            printk("Invalid CoAP packet: %d\n", ret);
            continue;
        }
		
        uint8_t conn_type = coap_header_get_type(&response);
		uint16_t id = coap_header_get_id(&response);
		uint8_t rx_token_len;
		const uint8_t *rx_token = coap_header_get_token(&response, &rx_token_len);
        // Check if response matches the request
        if (conn_type == COAP_TYPE_ACK && id == msg_id && rx_token_len == sizeof(token) &&
    	memcmp(rx_token, token, sizeof(token)) == 0) {
            printk("Received ACK for message ID: %d\n", id);
			uint16_t payload_len;
			// Extract payload from response
			uint8_t *payload = coap_packet_get_payload(&response, &payload_len);
			if (payload && payload_len > 0) {
				printk("Received payload: %.*s\n", payload_len, payload);
			} else {
				printk("No payload received\n");
			}

        } else {
            printk("Unsupported CoAP response\n");
        }
		k_sleep(K_SECONDS(5));
	}
}

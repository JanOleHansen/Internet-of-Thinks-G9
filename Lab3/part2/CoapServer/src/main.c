#include <zephyr.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#include <net/socket.h>
#include <net/net_if.h>
#include <net/net_config.h>
#include <net/net_mgmt.h>
#include <net/ieee802154_mgmt.h>
#include <net/coap.h>

#define COAP_PORT 5683
#define RECV_BUF_SIZE 128
#define RESP_BUF_SIZE 128

static uint16_t pan_id = 0x1234;
static uint16_t channel = 15;
static uint8_t temp = 25;
static bool action_state = false;

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
                                   "CoAP Server 802.15.4",
                                   NET_CONFIG_NEED_IPV6,
                                   10000);
    if (ret < 0) {
        printk("Network config failed: %d\n", ret);
        return ret;
    }

    printk("Network ready\n");
    return 0;
}

static bool path_matches(struct coap_packet *request, const char *expected)
{
    struct coap_option options[4];
    int count;

    count = coap_find_options(request,
                              COAP_OPTION_URI_PATH,
                              options,
                              ARRAY_SIZE(options));

    if (count <= 0) {
        return false;
    }

    /*
     * Für dieses Lab erwarten wir einfache Pfade:
     * /sensor oder /action
     */
    if (count != 1) {
        return false;
    }

    if (options[0].len != strlen(expected)) {
        return false;
    }

    return memcmp(options[0].value, expected, options[0].len) == 0;
}

static int send_response(int sock,
                         struct sockaddr *client_addr,
                         socklen_t client_len,
                         struct coap_packet *request,
                         uint8_t conn_type,
                         uint8_t response_code,
                         const char *payload)
{
    uint8_t response_buf[RESP_BUF_SIZE];
    struct coap_packet response;
    const uint8_t *token;
    uint8_t token_len;
    uint16_t id;
    int ret;

    // Get token and message ID from request to include in response
    token = coap_header_get_token(request, &token_len);
    id = coap_header_get_id(request);

    // Initialize CoAP response packet
    if (conn_type == COAP_TYPE_CON) {
        conn_type = COAP_TYPE_ACK;
    } else {
        conn_type = COAP_TYPE_NON_CON;
    }
    ret = coap_packet_init(&response,
                           response_buf,
                           sizeof(response_buf),
                           COAP_VERSION_1,
                           conn_type,
                           0,
                           NULL,
                           response_code,
                           id);
    if (ret < 0) {
        printk("coap_packet_init response failed: %d\n", ret);
        return ret;
    }

    if (payload && payload[0] != '\0') {
        // Append payload marker and payload if provided
        ret = coap_packet_append_payload_marker(&response);
        if (ret < 0) {
            printk("append payload marker failed: %d\n", ret);
            return ret;
        }

        ret = coap_packet_append_payload(&response,
                                         (uint8_t *)payload,
                                         strlen(payload));
        if (ret < 0) {
            printk("append payload failed: %d\n", ret);
            return ret;
        }
    }

    // Send response back to client
    ret = sendto(sock,
                 response_buf,
                 response.offset,
                 0,
                 client_addr,
                 client_len);

    if (ret < 0) {
        printk("sendto response failed: %d\n", errno);
        return -errno;
    }

    printk("Sent CoAP response: %s\n", payload ? payload : "");
    return 0;
}

void main(void)
{
    int sock;
    int ret;

    printk("MAIN STARTED\n");

    ret = setup_network();
    if (ret < 0) {
        return;
    }

    // Create UDP socket and bind to CoAP port
    sock = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        printk("socket() failed: %d\n", errno);
        return;
    }

    struct sockaddr_in6 bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin6_family = AF_INET6;
    bind_addr.sin6_port = htons(COAP_PORT);
    bind_addr.sin6_addr = in6addr_any;

    ret = bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr));
    if (ret < 0) {
        printk("bind() failed: %d\n", errno);
        return;
    }

    printk("CoAP server listening on UDP port %d\n", COAP_PORT);

    while (1) {
        // Wait for incoming CoAP requests
        uint8_t recv_buf[RECV_BUF_SIZE];
        struct sockaddr_in6 client_addr;
        socklen_t client_len = sizeof(client_addr);
        struct coap_packet request;

        printk("Waiting for CoAP request...\n");

        ret = recvfrom(sock,
                       recv_buf,
                       sizeof(recv_buf),
                       0,
                       (struct sockaddr *)&client_addr,
                       &client_len);

        if (ret < 0) {
            printk("recvfrom() failed: %d\n", errno);
            continue;
        }

        printk("Received %d bytes\n", ret);

        // Parse CoAP request
        ret = coap_packet_parse(&request, recv_buf, ret, NULL, 0);
        if (ret < 0) {
            printk("Invalid CoAP packet: %d\n", ret);
            continue;
        }
        printk("RX MID=%u\n", coap_header_get_id(&request));
        uint8_t method = coap_header_get_code(&request);
        uint8_t conn_type = coap_header_get_type(&request);
        // Determine response based on method and path
        if (method == COAP_METHOD_GET && path_matches(&request, "sensor")) {
            // /sensor GET request -> respond with current temp value
            char payload[32];

            snprintf(payload, sizeof(payload), "temp=%u", temp++);
            printk("GET /sensor -> %s\n", payload);

            send_response(sock,
                          (struct sockaddr *)&client_addr,
                          client_len,
                          &request,
                          conn_type,
                          COAP_RESPONSE_CODE_CONTENT,
                          payload);

        } else if ((method == COAP_METHOD_POST ||
                    method == COAP_METHOD_PUT) &&
                   path_matches(&request, "action")) {
            // /action POST or PUT request -> toggle action state and respond with new state
            action_state = !action_state;

            printk("%s /action -> action_state=%d\n",
                   method == COAP_METHOD_POST ? "POST" : "PUT",
                   action_state);

            send_response(sock,
                          (struct sockaddr *)&client_addr,
                          client_len,
                          &request,
                          conn_type,
                          COAP_RESPONSE_CODE_CHANGED,
                          action_state ? "action=on" : "action=off");

        } else {
            printk("Unsupported CoAP request\n");

            send_response(sock,
                          (struct sockaddr *)&client_addr,
                          client_len,
                          &request,
                          conn_type,
                          COAP_RESPONSE_CODE_NOT_FOUND,
                          "not found");
        }
        printk("RESP MID=%u\n", coap_header_get_id(&request));
    }
}
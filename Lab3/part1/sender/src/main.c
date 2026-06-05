#include <zephyr.h>
#include <stdio.h>
#include <net/socket.h>
#include <net/net_if.h>
#include <net/net_config.h>
#include <net/net_mgmt.h>
#include <net/ieee802154_mgmt.h>
#include <string.h>
#include <errno.h>

#define RECEIVER_ADDR "fd00::2"
#define RECEIVER_PORT 12345
uint16_t pan_id = 0x1234;
uint16_t channel = 15;

void main(void)
{
    printk("MAIN STARTED\n");
    // Get network interface
    struct net_if *iface = net_if_get_default();
    if (!iface) {
        printk("No network interface found!\n");
        return;
    }

    printk("Network interface found: %p\n", iface);
    // Set PAN ID and channel for IEEE 802.15.4 in the network interface
    int ret = net_mgmt(NET_REQUEST_IEEE802154_SET_PAN_ID,
               iface,
               &pan_id,
               sizeof(pan_id));

    if (ret < 0) {
        printk("Cannot set PAN ID: %d\n", ret);
        return;
    }
    printk("PAN ID set to: 0x%04x\n", pan_id);

    ret = net_mgmt(NET_REQUEST_IEEE802154_SET_CHANNEL,
                iface,
                &channel,
                sizeof(channel));

    if (ret < 0) {
        printk("Cannot set channel: %d\n", ret);
        return;
    }

    printk("Channel set to: %d\n", channel);
    // Initialize network application
    ret = net_if_up(iface);
    if (ret < 0) {
        printk("Cannot bring interface up: %d\n", ret);
        return;
    }
    printk("Interface is up\n");
    ret = net_config_init_by_iface(iface, "Simple Sender 802.15.4", NET_CONFIG_NEED_IPV6, 10000);
    if (ret < 0) {
        printk("Failed to initialize network configuration: %d\n", ret);
        return;
    }

    printk("Network configuration initialized successfully\n");
    // Create UDP socket with IPv6 and Receiver address/port
    int sock;
    struct sockaddr_in6 dest_addr;

    sock = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        printk("socket() failed: %d\n", errno);
        return;
    }
    printk("Socket created successfully: %d\n", sock);

    memset(&dest_addr, 0, sizeof(dest_addr));
    dest_addr.sin6_family = AF_INET6;
    dest_addr.sin6_port = htons(RECEIVER_PORT);

    // Convert IPv6 address from text to binary form
    ret = inet_pton(AF_INET6, RECEIVER_ADDR, &dest_addr.sin6_addr);
    if (ret != 1) {
        printk("inet_pton() failed\n");
        return;
    }
    printk("Receiver address set to: %s\n", RECEIVER_ADDR);
    // Send data to receiver
    uint16_t temp = 20;
    while (1) {
        char msg[10];
        snprintf(msg, sizeof(msg), "temp=%d", temp);
        ret  = sendto(sock, msg, strlen(msg), 0, (struct sockaddr *)&dest_addr,
            sizeof(dest_addr));
        if (ret < 0) {
            printk("send() failed: %d\n", errno);
        } else {
            printk("Sent: %s\n", msg);
        }
        k_sleep(K_SECONDS(2));
        temp ++;
    };
}

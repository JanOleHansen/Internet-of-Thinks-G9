#include <zephyr.h>
#include <stdio.h>
#include <net/socket.h>
#include <net/net_if.h>
#include <net/net_config.h>
#include <net/net_mgmt.h>
#include <net/ieee802154_mgmt.h>
#include <string.h>
#include <errno.h>

#define LISTEN_PORT 12345
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
    ret = net_config_init_by_iface(iface, "Simple Receiver 802.15.4", NET_CONFIG_NEED_IPV6, 10000);
    if (ret < 0) {
        printk("Failed to initialize network configuration: %d\n", ret);
        return;
    }
    printk("Network configuration initialized successfully\n");
    // Create UDP socket with IPv6 and bind to the listen port
    int sock;
    struct sockaddr_in6 bind_addr;

    sock = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        printk("socket() failed: %d\n", errno);
        return;
    }
    printk("Socket created successfully: %d\n", sock);
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin6_family = AF_INET6;
    bind_addr.sin6_port = htons(LISTEN_PORT);
    bind_addr.sin6_addr = in6addr_any;

    ret = bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr));
    if (ret < 0) {
        printk("bind() failed: %d\n", errno);
        return;
    }
    printk("Socket bound to port: %d\n", LISTEN_PORT);
    // Receive data from sender
    while (1) {
        char buffer[10];
        struct sockaddr_in6 src_addr;
        socklen_t src_len = sizeof(src_addr);
        printk("Waiting for data...\n");
        ret = recvfrom(sock, buffer, sizeof(buffer) - 1, 0,
               (struct sockaddr *)&src_addr, &src_len);
        if (ret < 0) {
            printk("recv() failed: %d\n", errno);
            continue;
        }
        buffer[ret] = '\0'; // Null-terminate the received data
        printk("Received: %s\n from %s", buffer, net_addr_ntop(AF_INET6, &src_addr.sin6_addr, buffer, sizeof(buffer)));
    }
}

#include <zephyr.h>
#include <zephyr/types.h>
#include <stddef.h>
#include <sys/printk.h>
#include <sys/util.h>
#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/addr.h>

// Callback function to parse and print received raw advertising data
bool parse_adv_data(struct bt_data *data, void *user_data) {
    // Print the raw advertising data in hexadecimal format
    // if manufacturer specific data is received, print the data type, length, and content
    if (data->type == BT_DATA_MANUFACTURER_DATA) {
        printk("Data type: 0x%02X, Data length: %d, Data: \n", data->type, data->data_len);
        printk("The temperature is %d C\n", data->data[2]);
        printk("The custom message is: %.*s\n", 3, &data->data[3]);
    }
    return true; // Return true to continue parsing other data fields
}

// Callback function to handle received advertising data
void scan_callback(
        const bt_addr_le_t *addr, 
        int8_t rssi, 
        uint8_t adv_type, 
        struct net_buf_simple *buf) {
            // Print the raw advertising data with bt_data_parse() for better readability
            bt_data_parse(buf, parse_adv_data, NULL);

};

int main(void)
{
    // Initialize the Bluetooth Subsystem (synchronous)
    int err = bt_enable(NULL);
    if (err) {
        printk("Bluetooth init failed (err %d)\n", err);
        return 0;
    }
    printk("Bluetooth initialized\n");

    do {
        k_msleep(1000); /* Wait for 1 second before starting scanning */
    
        // Start scanning for Bluetooth devices
        err = bt_le_scan_start(BT_LE_SCAN_PASSIVE, scan_callback);
        if (err) {
            printk("Scanning failed to start (err %d)\n", err);
            return 0;
        }

        k_msleep(1000); /* Scan for 1 second */
        // Stop scanning
        err = bt_le_scan_stop();
        if (err) {
            printk("Scanning failed to stop (err %d)\n", err);
            return 0;
        }
        
    } while (1);
    return 0;
}

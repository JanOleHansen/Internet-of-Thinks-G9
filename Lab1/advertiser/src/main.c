#include <zephyr.h>
#include <sys/printk.h>
// include uint8_t, size_t
#include <stdint.h>
#include <zephyr/types.h>
#include <stddef.h>
#include <sys/printk.h>
#include <sys/util.h>
#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>

// Struct for manufacturer data
struct mfg_data {
    uint16_t company_id; // Company Identifier Code (2 bytes)
    uint8_t temperature; // Temperature data (1 byte)
    char message[3]; // Custom message (5 bytes)
};
// Manufacturer specific data (0xFF) with 3 bytes of data
static struct mfg_data my_mfg_data = {
    .company_id = 0xFFFF, // Example company ID (0xFFFF is reserved for testing)
    .temperature = 30, // Example temperature value
    .message = "Hot" // Example custom message
};
// Set Advertisement data with manufacturer specific data
const struct bt_data ad[] = {
    BT_DATA(BT_DATA_MANUFACTURER_DATA, &my_mfg_data, sizeof(my_mfg_data))
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
        k_msleep(100); /* Wait for 0.1 second before sending advertising data */
        printk("Sending advertising data: Temperature %d C\n", my_mfg_data.temperature);
        printk("Sending advertising data: Custom message: %s\n", my_mfg_data.message);
        /* Start non-connectable advertising (broadcast); no scan response */
        err = bt_le_adv_start(BT_LE_ADV_NCONN, ad, ARRAY_SIZE(ad), NULL, 0);
        if (err) {
            printk("Advertising failed to start (err %d)\n", err);
            return 0;
        }
        /* Send advertising data for 0.5 second */
        k_msleep(500);
        /* Stop advertising */
        err = bt_le_adv_stop();
        if (err) {
            printk("Advertising failed to stop (err %d)\n", err);
            return 0;
        }
        printk("Now the temperature is raised by 1 degree Celsius\n");
        my_mfg_data.temperature += 1; // Increment temperature for next advertising cycle
    } while (1);
    return 0;
}

#include <zephyr.h>

#include <zephyr/types.h>
#include <stddef.h>
#include <sys/printk.h>
#include <sys/util.h>
#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>

// Manufacturer specific data (0xFF) with 3 bytes of data
static uint8_t mfg_data[] = { 0xff, 0xff, 0x00 };
// Set Advertisement data with manufacturer specific data
const struct bt_data ad[] = {
    BT_DATA(BT_DATA_MANUFACTURER_DATA, mfg_data, 3)
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
        k_msleep(1000); /* Wait for 1 second before sending advertising data */
        printk("Sending advertising data: 0x%02X\n", mfg_data[2]);
        /* Start non-connectable advertising (broadcast); no scan response */
        err = bt_le_adv_start(BT_LE_ADV_NCONN, ad, ARRAY_SIZE(ad), NULL, 0);
        /* Send advertising data for 1 second */
        k_msleep(1000);
        /* Stop advertising */
        err = bt_le_adv_stop();
        if (err) {
            printk("Advertising failed to stop (err %d)\n", err);
            return 0;
        }
        mfg_data[2]++;
    } while (1);
    return 0;
}

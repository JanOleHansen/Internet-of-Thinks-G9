/* Modified Zephyr beacon example from https://github.com/zephyrproject-rtos/zephyr/tree/main/samples/bluetooth/beacon */

#include <zephyr.h>

 #include <zephyr/types.h>
 #include <stddef.h>
 #include <sys/printk.h>
 #include <sys/util.h>
 
 #include <bluetooth/bluetooth.h>
 #include <bluetooth/hci.h>
 
 #define DEVICE_NAME CONFIG_BT_DEVICE_NAME
 #define DEVICE_NAME_LEN (sizeof(DEVICE_NAME) - 1)
 
 static uint8_t mfg_data[] = { 0xff, 0xff, 0x00 };
 /* Set Advertisement data */
 static const struct bt_data ad[] = {
     //BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_NO_BREDR),
     /*BT_DATA_BYTES(BT_DATA_NAME_COMPLETE,
               'H', 'e', 'l', 'l', 'o', ' ',
               'I', 'o', 'T', ' ',
               'c', 'o', 'u', 'r', 's', 'e', '!'
            )
        */
       BT_DATA(BT_DATA_MANUFACTURER_DATA, mfg_data, 3)
    };
 
 /* Set Scan Response data
 static const struct bt_data sd[] = {
     BT_DATA(BT_DATA_NAME_COMPLETE, DEVICE_NAME, DEVICE_NAME_LEN),
 };*/
 /*
 static void bt_ready(int err)
 {
     char addr_s[BT_ADDR_LE_STR_LEN];
     bt_addr_le_t addr = {0};
     size_t count = 1;
 
     if (err) {
         printk("Bluetooth init failed (err %d)\n", err);
         return;
     }
 
     printk("Bluetooth initialized\n");
 
     /* Start advertising 
     err = bt_le_adv_start(BT_LE_ADV_NCONN_IDENTITY, ad, ARRAY_SIZE(ad),
                   sd, ARRAY_SIZE(sd));
     if (err) {
         printk("Advertising failed to start (err %d)\n", err);
         return;
     }
 
 
     /* For connectable advertising you would use
      * bt_le_oob_get_local().  For non-connectable non-identity
      * advertising an non-resolvable private address is used;
      * there is no API to retrieve that.
      
 
     bt_id_get(&addr, &count);
     bt_addr_le_to_str(&addr, addr_s, sizeof(addr_s));
 
     printk("Beacon started, advertising as %s\n", addr_s);
 }
 */
 
 int main(void)
 {
     int err;
 
     printk("Starting Broadcaster\n");
 
     /* Initialize the Bluetooth Subsystem */
     err = bt_enable(NULL);
     if (err) {
         printk("Bluetooth init failed (err %d)\n", err);
     }
     printk("Bluetooth initialized\n");
     do {
		k_msleep(1000);

		printk("Sending advertising data: 0x%02X\n", mfg_data[2]);

		/* Start advertising */
		err = bt_le_adv_start(BT_LE_ADV_NCONN, ad, ARRAY_SIZE(ad),
				      NULL, 0);
		if (err) {
			printk("Advertising failed to start (err %d)\n", err);
			return 0;
		}

		k_msleep(1000);

		err = bt_le_adv_stop();
		if (err) {
			printk("Advertising failed to stop (err %d)\n", err);
			return 0;
		}

		mfg_data[2]++;

	} while (1);
     return 0;
 }
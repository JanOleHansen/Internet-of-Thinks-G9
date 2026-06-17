#include <zephyr.h>
#include <device.h>
#include <devicetree.h>
#include <drivers/gpio.h>
#include <sys/printk.h>
#include <stdint.h>
#include <errno.h>
#include <drivers/pwm.h>

/* Part for Temp- & Humiditysensor 
https://www.mouser.com/datasheet/2/758/DHT11-Technical-Data-Sheet-Translated-Version-1143054.pdf?srsltid=AfmBOop9QYyarqOQ7LcXVN-u1Wop3ConD42_Do83c4glCUWUm1D73HyP */
#define DHT_PIN 13

/* Part for Lightsensor
https://www.berrybase.de/es/product-datasheet/018deef3df7b72c7a37e653c07583a1d/create?srsltid=AfmBOoob8iPsz877Es7JHAZs6cUjDZxpCms4U8A4gsVSBR18xQOwl5Cj */
#define LIGHT_PIN 14

/* Part for Movementsensor
https://cdn-reichelt.de/documents/datenblatt/A300/SEN-HC-SR501-ANLEITUNG-23.09.2020.pdf */
#define MOVEMENT_PIN 10
bool action = false;

static const struct device *gpio1 = DEVICE_DT_GET(DT_NODELABEL(gpio1));

struct dht11_data {
    uint8_t humidity_int;
    uint8_t humidity_dec;
    uint8_t temp_int;
    uint8_t temp_dec;
    uint8_t checksum;
};

static int wait_for_level(int level, int timeout_us)
{
    int waited = 0;

    while (gpio_pin_get(gpio1, DHT_PIN) != level) {
        if (waited >= timeout_us) {
            return -ETIMEDOUT;
        }

        k_busy_wait(1);
        waited++;
    }

    return waited;
}

int dht11_read(struct dht11_data *out)
{
    uint8_t data[5] = {0};
    int ret;

    //1) Bus freigeben/Idle HIGH sicherstellen
    ret = gpio_pin_configure(gpio1, DHT_PIN, GPIO_INPUT | GPIO_PULL_UP);
    if (ret < 0) {
        return ret;
    }

    k_msleep(2);

    // 2) Startsignal: MCU zieht DATA mindestens 18 ms LOW.

    ret = gpio_pin_configure(gpio1, DHT_PIN, GPIO_OUTPUT_LOW);
    if (ret < 0) {
        return ret;
    }

    k_msleep(20);

    // 3) Leitung freigeben: Nicht HIGH treiben, sondern auf Input schalten. 
    // Danach wartet die MCU laut Datenblatt 20-40 us.
     
    ret = gpio_pin_configure(gpio1, DHT_PIN, GPIO_INPUT | GPIO_PULL_UP);
    if (ret < 0) {
        return ret;
    }

    k_busy_wait(30);

    // 4) DHT11 Antwort: ca. 80 us LOW, danach ca. 80 us HIGH.
    ret = wait_for_level(0, 500);
    if (ret < 0) {
        printk("DHT error: no initial 80us LOW response\n");
        return -EIO;
    }

    ret = wait_for_level(1, 500);
    if (ret < 0) {
        printk("DHT error: no 80us HIGH response\n");
        return -EIO;
    }

    ret = wait_for_level(0, 500);
    if (ret < 0) {
        printk("DHT error: no LOW before data bits\n");
        return -EIO;
    }

    // 5) 40 Datenbits:
    // jedes Bit beginnt mit ca. 50 us LOW,
    // danach HIGH:
    // ca. 26-28 us = 0#
    // ca. 70 us    = 1
    for (int i = 0; i < 40; i++) {
        ret = wait_for_level(1, 500);
        if (ret < 0) {
            printk("DHT error: bit %d did not go HIGH\n", i);
            return -EIO;
        }

        int high_time = wait_for_level(0, 500);
        if (high_time < 0) {
            printk("DHT error: bit %d HIGH timeout\n", i);
            return -EIO;
        }

        data[i / 8] <<= 1;

        if (high_time > 40) {
            data[i / 8] |= 1;
        }
    }

    // 6) Checksumme: letzte 8 Bit der Summe von Byte 0-3.
     
    uint8_t checksum = (uint8_t)(data[0] + data[1] + data[2] + data[3]);

    if (checksum != data[4]) {
        printk("Checksum failed: calc=%u received=%u\n", checksum, data[4]);
        printk("Raw: %u %u %u %u %u\n",
               data[0], data[1], data[2], data[3], data[4]);
        return -EBADMSG;
    }
    printk("Raw: %u %u %u %u %u\n",
               data[0], data[1], data[2], data[3], data[4]);

    out->humidity_int = data[0];
    out->humidity_dec = data[1];
    out->temp_int = data[2];
    out->temp_dec = data[3];
    out->checksum = data[4];

    return 0;
}

int main(void)
{
    struct dht11_data dht;
    
    
    if (!device_is_ready(gpio1)){
        printk("GPIO1 is not ready\n");
        return;
    }

    int err = gpio_pin_configure(gpio1, LIGHT_PIN, GPIO_INPUT);
    if (err != 0){
        printk("Configuration of Light-Pin failed\n");
        return;
    }

    err = gpio_pin_configure(gpio1, MOVEMENT_PIN, GPIO_INPUT);
    if (err != 0){
        printk("Movement Pin Configuration failed\n");
        return;
    }

    while (1) {
        // Read DHT11
        int ret = dht11_read(&dht);

        if (ret == 0) {
            printk("Temp: %u.%u C, Humidity: %u.%u %%\n",
                   dht.temp_int,
                   dht.temp_dec,
                   dht.humidity_int,
                   dht.humidity_dec);
        } else {
            printk("DHT11 read failed: %d\n", ret);
        }

        // Read Light Sensor
        int value = gpio_pin_get(gpio1, LIGHT_PIN);
        if (value == 0){
            printk("Es ist hell\n");
        } else {
            printk("Es ist dunkel\n");
        }

        // Read Movement Sensor
        int val = gpio_pin_get(gpio1, MOVEMENT_PIN);
        if (val == 1 && !action){
            action = true;
            printk("Movement was detected\n");
        } else if (val == 0 && action){
            action = false;
            printk("No Movement was detected\n");
        }

        k_sleep(K_SECONDS(1));
    }
}
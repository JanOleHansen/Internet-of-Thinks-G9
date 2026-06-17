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

static int wait_until_level(int level, uint32_t timeout_us)
{
    uint32_t start = k_cycle_get_32();
    uint32_t timeout_cycles = k_us_to_cyc_ceil32(timeout_us);

    while (gpio_pin_get(gpio1, DHT_PIN) != level) {
        if ((uint32_t)(k_cycle_get_32() - start) > timeout_cycles) {
            return -ETIMEDOUT;
        }
    }

    return 0;
}

static int measure_level_duration_us(int level, uint32_t timeout_us)
{
    uint32_t start = k_cycle_get_32();
    uint32_t timeout_cycles = k_us_to_cyc_ceil32(timeout_us);

    while (gpio_pin_get(gpio1, DHT_PIN) == level) {
        if ((uint32_t)(k_cycle_get_32() - start) > timeout_cycles) {
            return -ETIMEDOUT;
        }
    }

    uint32_t elapsed_cycles = k_cycle_get_32() - start;
    return (int)k_cyc_to_us_floor32(elapsed_cycles);
}
int dht11_read(struct dht11_data *out)
{
    uint8_t data[5] = {0};
    int ret;

    if (!device_is_ready(gpio1)) {
        return -ENODEV;
    }

    ret = gpio_pin_configure(gpio1, DHT_PIN, GPIO_INPUT | GPIO_PULL_UP);
    if (ret < 0) {
        return ret;
    }

    k_msleep(2);

    ret = gpio_pin_configure(gpio1, DHT_PIN, GPIO_OUTPUT_LOW);
    if (ret < 0) {
        return ret;
    }

    k_msleep(20);

    ret = gpio_pin_configure(gpio1, DHT_PIN, GPIO_INPUT | GPIO_PULL_UP);
    if (ret < 0) {
        return ret;
    }

    k_busy_wait(30);

    ret = wait_until_level(0, 200);
    if (ret < 0) {
        return -EIO;
    }

    ret = measure_level_duration_us(0, 120);
    if (ret < 0) {
        return -EIO;
    }

    ret = measure_level_duration_us(1, 120);
    if (ret < 0) {
        return -EIO;
    }

    for (int i = 0; i < 40; i++) {
        ret = wait_until_level(0, 100);
        if (ret < 0) {
            return -EIO;
        }

        ret = measure_level_duration_us(0, 100);
        if (ret < 0) {
            return -EIO;
        }

        ret = measure_level_duration_us(1, 120);
        if (ret < 0) {
            return -EIO;
        }

        int high_us = ret;

        data[i / 8] <<= 1;

        if (high_us > 50) {
            data[i / 8] |= 1;
        }
    }

    uint8_t checksum = (uint8_t)(data[0] + data[1] + data[2] + data[3]);

    if (checksum != data[4]) {
        printk("Checksum failed: calc=%u received=%u\n", checksum, data[4]);
        printk("Raw: %u %u %u %u %u\n",
               data[0], data[1], data[2], data[3], data[4]);
        return -EBADMSG;
    }

    out->humidity_int = data[0];
    out->humidity_dec = data[1];
    out->temp_int = data[2];
    out->temp_dec = data[3];
    out->checksum = data[4];

    printk("Raw: %u %u %u %u %u\n",
           data[0], data[1], data[2], data[3], data[4]);

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
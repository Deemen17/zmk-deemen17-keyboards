#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/hid_indicators.h>
#include <zmk/events/hid_indicators_changed.h>

#define LED_NODE_R DT_ALIAS(ledred)
#define LED_NODE_G DT_ALIAS(ledgreen)
#define LED_NODE_B DT_ALIAS(ledblue)

#if !DT_NODE_HAS_STATUS(LED_NODE_R, okay) || !DT_NODE_HAS_STATUS(LED_NODE_G, okay) || !DT_NODE_HAS_STATUS(LED_NODE_B, okay)
#error "Unsupported board: led devicetree alias is not defined"
#endif

static const struct gpio_dt_spec LED_R = GPIO_DT_SPEC_GET(LED_NODE_R, gpios);
static const struct gpio_dt_spec LED_G = GPIO_DT_SPEC_GET(LED_NODE_G, gpios);
static const struct gpio_dt_spec LED_B = GPIO_DT_SPEC_GET(LED_NODE_B, gpios);

static bool caps_lock_active = false;

// Initialize GPIOs
static int init_led_gpios(void) {
    int err;
    if (!device_is_ready(LED_R.port)) {
        LOG_ERR("LED_R port (P0.03) not ready");
        return -ENODEV;
    }
    if (!device_is_ready(LED_G.port)) {
        LOG_ERR("LED_G port (P1.10) not ready");
        return -ENODEV;
    }
    if (!device_is_ready(LED_B.port)) {
        LOG_ERR("LED_B port (P1.11) not ready");
        return -ENODEV;
    }

    err = gpio_pin_configure_dt(&LED_R, GPIO_OUTPUT);
    if (err) LOG_ERR("Failed to configure LED_R (P0.03): %d", err);
    err = gpio_pin_configure_dt(&LED_G, GPIO_OUTPUT);
    if (err) LOG_ERR("Failed to configure LED_G (P1.10): %d", err);
    err = gpio_pin_configure_dt(&LED_B, GPIO_OUTPUT);
    if (err) LOG_ERR("Failed to configure LED_B (P1.11): %d", err);

    return err;
}

void reset_leds() {
    int err;
    err = gpio_pin_set_dt(&LED_R, 1); // HIGH = OFF (Common Anode)
    if (err) LOG_ERR("Failed to set LED_R (P0.03) HIGH: %d", err);
    err = gpio_pin_set_dt(&LED_G, 1);
    if (err) LOG_ERR("Failed to set LED_G (P1.10) HIGH: %d", err);
    err = gpio_pin_set_dt(&LED_B, 1);
    if (err) LOG_ERR("Failed to set LED_B (P1.11) HIGH: %d", err);
}

void set_led_rgb(bool r, bool g, bool b) {
    int err;
    err = gpio_pin_set_dt(&LED_R, r ? 0 : 1); // LOW = ON, HIGH = OFF
    if (err) LOG_ERR("Failed to set LED_R (P0.03): %d", err);
    err = gpio_pin_set_dt(&LED_G, g ? 0 : 1);
    if (err) LOG_ERR("Failed to set LED_G (P1.10): %d", err);
    err = gpio_pin_set_dt(&LED_B, b ? 0 : 1);
    if (err) LOG_ERR("Failed to set LED_B (P1.11): %d", err);
}

int led_caps_lock_listener(const zmk_event_t *eh) {
    zmk_hid_indicators_t flags = zmk_hid_indicators_get_current_profile();
    unsigned int capsBit = 1 << (HID_USAGE_LED_CAPS_LOCK - 1);
    bool new_caps_state = (flags & capsBit) != 0;
    LOG_DBG("Caps Lock event received, new state: %d", new_caps_state);
    if (new_caps_state != caps_lock_active) {
        caps_lock_active = new_caps_state;
        if (caps_lock_active) {
            LOG_DBG("Caps Lock active, setting LED to White");
            set_led_rgb(true, true, true); // Bật LED trắng
        } else {
            LOG_DBG("Caps Lock inactive, turning off LEDs");
            reset_leds(); // Tắt LED
        }
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(led_caps_lock_listener, led_caps_lock_listener);
ZMK_SUBSCRIPTION(led_caps_lock_listener, zmk_hid_indicators_changed);

// Test LED on boot
static int led_test_init(const struct device *device) {
    LOG_DBG("Initializing LED GPIOs");
    int err = init_led_gpios();
    if (err) {
        LOG_ERR("Failed to initialize LED GPIOs: %d", err);
        return err;
    }

    LOG_DBG("LED test on boot: Setting LED to White");
    set_led_rgb(true, true, true); // Bật LED trắng ngay khi khởi động
    return 0;
}

SYS_INIT(led_test_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
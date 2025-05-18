#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/hid_indicators.h>
#include <zmk/events/hid_indicators_changed.h>
#include <zmk/battery.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/ble.h>
#include <zmk/events/ble_active_profile_changed.h>

// Define GPIO for RGB LEDs
#define LED_NODE_R DT_ALIAS(ledred)
#define LED_NODE_G DT_ALIAS(ledgreen)
#define LED_NODE_B DT_ALIAS(ledblue)

#if !DT_NODE_HAS_STATUS(LED_NODE_R, okay) || !DT_NODE_HAS_STATUS(LED_NODE_G, okay) || !DT_NODE_HAS_STATUS(LED_NODE_B, okay)
#error "Unsupported board: led devicetree alias is not defined"
#endif

static const struct gpio_dt_spec LED_R = GPIO_DT_SPEC_GET(LED_NODE_R, gpios);
static const struct gpio_dt_spec LED_G = GPIO_DT_SPEC_GET(LED_NODE_G, gpios);
static const struct gpio_dt_spec LED_B = GPIO_DT_SPEC_GET(LED_NODE_B, gpios);

// State variables
static bool caps_lock_active = false;
static uint8_t battery_level = 0;
static bool ble_connected = false;
static bool ble_open = false;

// Battery thresholds
#define BATTERY_LEVEL_HIGH 50
#define BATTERY_LEVEL_CRITICAL 10

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

    err = gpio_pin_configure_dt(&LED_R, GPIO_OUTPUT_HIGH);
    if (err) {
        LOG_ERR("Failed to configure LED_R (P0.03): %d", err);
        return err;
    }
    err = gpio_pin_configure_dt(&LED_G, GPIO_OUTPUT_HIGH);
    if (err) {
        LOG_ERR("Failed to configure LED_G (P1.10): %d", err);
        return err;
    }
    err = gpio_pin_configure_dt(&LED_B, GPIO_OUTPUT_HIGH);
    if (err) {
        LOG_ERR("Failed to configure LED_B (P1.11): %d", err);
        return err;
    }

    return 0;
}

// Set LED state (Common Anode: LOW = ON, HIGH = OFF)
static void set_led_rgb(bool r, bool g, bool b) {
    int err;
    err = gpio_pin_set_dt(&LED_R, r ? 0 : 1);
    if (err) LOG_ERR("Failed to set LED_R (P0.03): %d", err);
    err = gpio_pin_set_dt(&LED_G, g ? 0 : 1);
    if (err) LOG_ERR("Failed to set LED_G (P1.10): %d", err);
    err = gpio_pin_set_dt(&LED_B, b ? 0 : 1);
    if (err) LOG_ERR("Failed to set LED_B (P1.11): %d", err);
}

// Update LED based on current state
static void update_led_state(void) {
    if (caps_lock_active) {
        LOG_DBG("Caps Lock active, setting LED to White");
        set_led_rgb(true, true, true); // White
    } else if (ble_connected) {
        LOG_DBG("BLE connected, setting LED to Blue");
        set_led_rgb(false, false, true); // Blue
    } else if (ble_open) {
        LOG_DBG("BLE advertising, setting LED to Yellow");
        set_led_rgb(true, true, false); // Yellow
    } else if (battery_level > BATTERY_LEVEL_HIGH) {
        LOG_DBG("Battery high (>50%%), setting LED to Green");
        set_led_rgb(false, true, false); // Green
    } else if (battery_level > BATTERY_LEVEL_CRITICAL) {
        LOG_DBG("Battery medium (11-50%%), setting LED to Yellow");
        set_led_rgb(true, true, false); // Yellow
    } else {
        LOG_DBG("Battery critical (<=10%%), setting LED to Red");
        set_led_rgb(true, false, false); // Red
    }
}

// Caps Lock listener
static int led_caps_lock_listener(const zmk_event_t *eh) {
    zmk_hid_indicators_t flags = zmk_hid_indicators_get_current_profile();
    unsigned int capsBit = 1 << (HID_USAGE_LED_CAPS_LOCK - 1);
    bool new_caps_state = (flags & capsBit) != 0;

    if (new_caps_state != caps_lock_active) {
        caps_lock_active = new_caps_state;
        LOG_DBG("Caps Lock state changed: %d", caps_lock_active);
        update_led_state();
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(led_caps_lock_listener, led_caps_lock_listener);
ZMK_SUBscription(led_caps_lock_listener, zmk_hid_indicators_changed);

// Battery status listener
static int battery_status_listener(const zmk_event_t *eh) {
    const struct zmk_battery_state_changed *ev = as_zmk_battery_state_changed(eh);
    if (ev) {
        battery_level = ev->state_of_charge;
        LOG_DBG("Battery level changed: %d%%", battery_level);
        update_led_state();
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(battery_status_listener, battery_status_listener);
ZMK_SUBscription(battery_status_listener, zmk_battery_state_changed);

// Bluetooth connection status listener
static int connection_status_listener(const zmk_event_t *eh) {
    bool new_ble_connected = zmk_ble_active_profile_is_connected();
    bool new_ble_open = zmk_ble_active_profile_is_open();

    if (new_ble_connected != ble_connected || new_ble_open != ble_open) {
        ble_connected = new_ble_connected;
        ble_open = new_ble_open;
        LOG_DBG("BLE state changed - Connected: %d, Open: %d", ble_connected, ble_open);
        update_led_state();
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(connection_status_listener, connection_status_listener);
ZMK_SUBscription(connection_status_listener, zmk_ble_active_profile_changed);

// Initialization
static int led_init(const struct device *device) {
    LOG_DBG("Initializing LED GPIOs");
    int err = init_led_gpios();
    if (err) {
        LOG_ERR("Failed to initialize LED GPIOs: %d", err);
        return err;
    }

    // Get initial states
    battery_level = zmk_battery_state_of_charge();
    ble_connected = zmk_ble_active_profile_is_connected();
    ble_open = zmk_ble_active_profile_is_open();
    zmk_hid_indicators_t flags = zmk_hid_indicators_get_current_profile();
    unsigned int capsBit = 1 << (HID_USAGE_LED_CAPS_LOCK - 1);
    caps_lock_active = (flags & capsBit) != 0;

    LOG_DBG("Initial states - Caps Lock: %d, Battery: %d%%, BLE Connected: %d, BLE Open: %d",
            caps_lock_active, battery_level, ble_connected, ble_open);

    // Set initial LED state
    update_led_state();

    return 0;
}

SYS_INIT(led_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
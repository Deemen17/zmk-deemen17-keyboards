#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/hid_indicators.h>
#include <zmk/events/hid_indicators_changed.h>
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
static bool ble_connected = false;
static bool ble_open = false;
static bool ble_profile_cleared = false;
static uint8_t ble_active_profile = 0;

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
        LOG_DBG("Caps Lock active, LED on");
        set_led_rgb(false, false, false); // White
    } else if (ble_profile_cleared) {
        LOG_DBG("BLE profile cleared, setting LED to Green");
        set_led_rgb(false, true, false); // Green
    } else if (!ble_connected && !ble_open) {
        LOG_DBG("BLE disconnected, setting LED to Yellow");
        set_led_rgb(true, true, false); // Yellow
    } else {
        LOG_DBG("Caps Lock off, BLE connected or advertising (profile %d), setting LED to Pink", ble_active_profile);
        set_led_rgb(true, false, true); // Pink (for profile 0 connected or advertising)
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
ZMK_SUBSCRIPTION(led_caps_lock_listener, zmk_hid_indicators_changed);

// Bluetooth connection status listener
static int connection_status_listener(const zmk_event_t *eh) {
    bool new_ble_connected = zmk_ble_active_profile_is_connected();
    bool new_ble_open = zmk_ble_active_profile_is_open();
    uint8_t new_ble_profile = zmk_ble_active_profile_index();

    // Detect profile clear: no connection, no advertising, and profile changed
    bool new_ble_profile_cleared = !new_ble_connected && !new_ble_open;

    if (new_ble_connected != ble_connected || new_ble_open != ble_open ||
        new_ble_profile != ble_active_profile || new_ble_profile_cleared != ble_profile_cleared) {
        ble_connected = new_ble_connected;
        ble_open = new_ble_open;
        ble_active_profile = new_ble_profile;
        ble_profile_cleared = new_ble_profile_cleared;

        LOG_DBG("BLE state changed - Connected: %d, Open: %d, Profile: %d, Cleared: %d",
                ble_connected, ble_open, ble_active_profile, ble_profile_cleared);

        update_led_state();
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(connection_status_listener, connection_status_listener);
ZMK_SUBSCRIPTION(connection_status_listener, zmk_ble_active_profile_changed);

// Initialization
static int led_init(const struct device *device) {
    LOG_DBG("Initializing LED GPIOs");
    int err = init_led_gpios();
    if (err) {
        LOG_ERR("Failed to initialize LED GPIOs: %d", err);
        return err;
    }

    // Get initial states
    ble_connected = zmk_ble_active_profile_is_connected();
    ble_open = zmk_ble_active_profile_is_open();
    ble_active_profile = zmk_ble_active_profile_index();
    ble_profile_cleared = !ble_connected && !ble_open;
    zmk_hid_indicators_t flags = zmk_hid_indicators_get_current_profile();
    unsigned int capsBit = 1 << (HID_USAGE_LED_CAPS_LOCK - 1);
    caps_lock_active = (flags & capsBit) != 0;

    LOG_DBG("Initial states - Caps Lock: %d, BLE Connected: %d, BLE Open: %d, BLE Profile: %d, BLE Cleared: %d",
            caps_lock_active, ble_connected, ble_open, ble_active_profile, ble_profile_cleared);

    // Set initial LED state
    update_led_state();

    return 0;
}

SYS_INIT(led_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
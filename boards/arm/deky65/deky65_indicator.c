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
#include <zmk/battery.h>
#include <zmk/events/battery_state_changed.h>

// Define GPIO for RGB LEDs
#define LED_NODE_R DT_ALIAS(ledred)
#define LED_NODE_G DT_ALIAS(ledgreen)
#define LED_NODE_B DT_ALIAS(ledblue)

#if !DT_NODE_HAS_STATUS(LED_NODE_R, okay) || !DT_NODE_HAS_STATUS(LED_NODE_G, okay) || !DT_NODE_HAS_STATUS(LED_NODE_B, okay)
#error "Unsupported board: LED aliases not defined in devicetree"
#endif

static const struct gpio_dt_spec LED_R = GPIO_DT_SPEC_GET(LED_NODE_R, gpios);
static const struct gpio_dt_spec LED_G = GPIO_DT_SPEC_GET(LED_NODE_G, gpios);
static const struct gpio_dt_spec LED_B = GPIO_DT_SPEC_GET(LED_NODE_B, gpios);

// LED color states
#define COLOR_OFF      0 // Off (R=OFF, G=OFF, B=OFF)
#define COLOR_WHITE    1 // White (R=ON, G=ON, B=ON)
#define COLOR_GREEN    2 // Green (R=OFF, G=ON, B=OFF)
#define COLOR_YELLOW   3 // Yellow (R=ON, G=ON, B=OFF)
#define COLOR_RED      4 // Red (R=ON, G=OFF, B=OFF)
#define COLOR_BLUE     5 // Blue (R=OFF, G=OFF, B=ON)

// Battery thresholds
#define BATTERY_LEVEL_HIGH     50
#define BATTERY_LEVEL_CRITICAL 20

// Blink configuration
#define BLINK_COUNT       3
#define BLINK_ON_MS       250
#define BLINK_OFF_MS      250

// State variables
static bool caps_lock_active = false;
static bool ble_connected = false;
static bool ble_open = false;
static bool ble_profile_cleared = false;
static uint8_t ble_active_profile = 0;
static uint8_t battery_level = 0;

// Message queue for LED updates
struct led_state {
    uint8_t color;
    bool blink; // True: blink, False: solid
};

K_MSGQ_DEFINE(led_msgq, sizeof(struct led_state), 16, 1);

// Initialize GPIOs
static int init_led_gpios(void) {
    int err;
    if (!device_is_ready(LED_R.port)) {
        LOG_ERR("LED_R port (P1.10) not ready");
        return -ENODEV;
    }
    if (!device_is_ready(LED_G.port)) {
        LOG_ERR("LED_G port (P1.11) not ready");
        return -ENODEV;
    }
    if (!device_is_ready(LED_B.port)) {
        LOG_ERR("LED_B port (P0.03) not ready");
        return -ENODEV;
    }

    err = gpio_pin_configure_dt(&LED_R, GPIO_OUTPUT_HIGH);
    if (err) {
        LOG_ERR("Failed to configure LED_R (P1.10): %d", err);
        return err;
    }
    err = gpio_pin_configure_dt(&LED_G, GPIO_OUTPUT_HIGH);
    if (err) {
        LOG_ERR("Failed to configure LED_G (P1.11): %d", err);
        return err;
    }
    err = gpio_pin_configure_dt(&LED_B, GPIO_OUTPUT_HIGH);
    if (err) {
        LOG_ERR("Failed to configure LED_B (P0.03): %d", err);
        return err;
    }

    return 0;
}

// Set LED state (Common Anode: LOW = ON, HIGH = OFF)
static void set_led_rgb(uint8_t color, bool apply) {
    bool r, g, b;
    const char *color_name;

    switch (color) {
        case COLOR_WHITE:
            r = true; g = true; b = true;
            color_name = "White";
            break;
        case COLOR_GREEN:
            r = false; g = true; b = false;
            color_name = "Green";
            break;
        case COLOR_YELLOW:
            r = true; g = true; b = false;
            color_name = "Yellow";
            break;
        case COLOR_RED:
            r = true; g = false; b = false;
            color_name = "Red";
            break;
        case COLOR_BLUE:
            r = false; g = false; b = true;
            color_name = "Blue";
            break;
        case COLOR_OFF:
        default:
            r = false; g = false; b = false;
            color_name = "Off";
            break;
    }

    if (apply) {
        int err;
        err = gpio_pin_set_dt(&LED_R, r ? 0 : 1);
        if (err) LOG_ERR("Failed to set LED_R (P1.10): %d", err);
        err = gpio_pin_set_dt(&LED_G, g ? 0 : 1);
        if (err) LOG_ERR("Failed to set LED_G (P1.11): %d", err);
        err = gpio_pin_set_dt(&LED_B, b ? 0 : 1);
        if (err) LOG_ERR("Failed to set LED_B (P0.03): %d", err);
    }

    LOG_DBG("Setting LED to %s (R=%s, G=%s, B=%s)", color_name,
            r ? "ON" : "OFF", g ? "ON" : "OFF", b ? "ON" : "OFF");
}

// Blink LED
static void blink_led(uint8_t color) {
    for (int i = 0; i < BLINK_COUNT; i++) {
        set_led_rgb(color, true);
        k_msleep(BLINK_ON_MS);
        set_led_rgb(COLOR_OFF, true);
        k_msleep(BLINK_OFF_MS);
    }
}

// Update LED state
static void update_led_state(void) {
    struct led_state state;

    if (caps_lock_active) {
        state.color = COLOR_WHITE;
        state.blink = false;
        LOG_DBG("Caps Lock active, queuing solid White LED");
    } else {
        state.color = COLOR_OFF;
        state.blink = false;
        LOG_DBG("Caps Lock off, queuing LED Off (allowing battery/BLE blinks)");
    }

    k_msgq_put(&led_msgq, &state, K_NO_WAIT);
}

// Indicate battery status
static void indicate_battery(void) {
    struct led_state state;

    if (battery_level >= BATTERY_LEVEL_HIGH) {
        state.color = COLOR_GREEN;
        LOG_DBG("Battery high (>=50%), queuing Green blink");
    } else if (battery_level > BATTERY_LEVEL_CRITICAL) {
        state.color = COLOR_YELLOW;
        LOG_DBG("Battery medium (11-49%), queuing Yellow blink");
    } else {
        state.color = COLOR_RED;
        LOG_DBG("Battery critical (<=10%), queuing Red blink");
    }

    state.blink = true;
    k_msgq_put(&led_msgq, &state, K_NO_WAIT);
}

// Indicate Bluetooth connectivity
static void indicate_connectivity(void) {
    struct led_state state;

    if (ble_profile_cleared) {
        state.color = COLOR_GREEN;
        LOG_DBG("Bluetooth profile cleared, queuing Green blink");
    } else if (ble_connected) {
        state.color = COLOR_BLUE;
        LOG_DBG("Bluetooth connected (profile %d), queuing Blue blink", ble_active_profile);
    } else if (ble_open) {
        state.color = COLOR_YELLOW;
        LOG_DBG("Bluetooth advertising (profile %d), queuing Yellow blink", ble_active_profile);
    } else {
        state.color = COLOR_RED;
        LOG_DBG("Bluetooth disconnected, queuing Red blink");
    }

    state.blink = true;
    k_msgq_put(&led_msgq, &state, K_NO_WAIT);
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

// Battery status listener
static int battery_status_listener(const zmk_event_t *eh) {
    const struct zmk_battery_state_changed *ev = as_zmk_battery_state_changed(eh);
    if (ev) {
        battery_level = ev->state_of_charge;
        LOG_DBG("Battery level changed: %d%%", battery_level);
        if (battery_level <= BATTERY_LEVEL_CRITICAL && battery_level > 0) {
            struct led_state state = {.color = COLOR_RED, .blink = true};
            LOG_DBG("Battery critical (<=10%), queuing Red blink");
            k_msgq_put(&led_msgq, &state, K_NO_WAIT);
        }
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(battery_status_listener, battery_status_listener);
ZMK_SUBSCRIPTION(battery_status_listener, zmk_battery_state_changed);

// Bluetooth connection status listener
static int connection_status_listener(const zmk_event_t *eh) {
    bool new_ble_connected = zmk_ble_active_profile_is_connected();
    bool new_ble_open = zmk_ble_active_profile_is_open();
    uint8_t new_ble_profile = zmk_ble_active_profile_index();

    // Detect profile clear: assume cleared when not connected, not advertising, and profile changed
    bool new_ble_profile_cleared = !new_ble_connected && !new_ble_open && (new_ble_profile != ble_active_profile);

    if (new_ble_connected != ble_connected || new_ble_open != ble_open ||
        new_ble_profile != ble_active_profile || new_ble_profile_cleared != ble_profile_cleared) {
        ble_connected = new_ble_connected;
        ble_open = new_ble_open;
        ble_active_profile = new_ble_profile;
        ble_profile_cleared = new_ble_profile_cleared;

        LOG_DBG("Bluetooth state changed - Connected: %d, Advertising: %d, Profile: %d, Cleared: %d",
                ble_connected, ble_open, ble_active_profile, ble_profile_cleared);

        indicate_connectivity();
        if (!caps_lock_active) {
            update_led_state(); // Update LED to off after blink if Caps Lock is off
        }
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(connection_status_listener, connection_status_listener);
ZMK_SUBSCRIPTION(connection_status_listener, zmk_ble_active_profile_changed);

// LED processing thread
static void led_process_thread(void *d0, void *d1, void *d2) {
    ARG_UNUSED(d0);
    ARG_UNUSED(d1);
    ARG_UNUSED(d2);

    while (true) {
        struct led_state state;
        k_msgq_get(&led_msgq, &state, K_FOREVER);
        if (state.blink) {
            blink_led(state.color);
        } else {
            set_led_rgb(state.color, true);
        }
    }
}

K_THREAD_DEFINE(led_process_tid, 512, led_process_thread, NULL, NULL, NULL,
                K_LOWEST_APPLICATION_THREAD_PRIO, 0, 100);

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
    ble_active_profile = zmk_ble_active_profile_index();
    ble_profile_cleared = !ble_connected && !ble_open;
    zmk_hid_indicators_t flags = zmk_hid_indicators_get_current_profile();
    unsigned int capsBit = 1 << (HID_USAGE_LED_CAPS_LOCK - 1);
    caps_lock_active = (flags & capsBit) != 0;

    LOG_DBG("Initial states - Caps Lock: %d, Battery: %d%%, BLE Connected: %d, BLE Advertising: %d, BLE Profile: %d, BLE Cleared: %d",
            caps_lock_active, battery_level, ble_connected, ble_open, ble_active_profile, ble_profile_cleared);

    // Indicate battery on boot
    indicate_battery();
    k_msleep((BLINK_COUNT * (BLINK_ON_MS + BLINK_OFF_MS)) + 100); // Wait for battery blink to complete

    // Indicate Bluetooth on boot
    indicate_connectivity();
    k_msleep((BLINK_COUNT * (BLINK_ON_MS + BLINK_OFF_MS)) + 100); // Wait for BLE blink to complete

    // Set initial LED state
    update_led_state();

    return 0;
}

SYS_INIT(led_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
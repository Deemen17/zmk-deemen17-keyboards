#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/led.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>

#include <zmk/battery.h>
#include <zmk/ble.h>
#include <zmk/endpoints.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/hid_indicators.h>
#include <zmk/events/hid_indicators_changed.h>

#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define LED_GPIO_NODE_ID DT_COMPAT_GET_ANY_STATUS_OKAY(gpio_leds)

BUILD_ASSERT(DT_NODE_EXISTS(DT_ALIAS(ledred)),
             "An alias for a red LED is not found for RGBLED_WIDGET");
BUILD_ASSERT(DT_NODE_EXISTS(DT_ALIAS(ledgreen)),
             "An alias for a green LED is not found for RGBLED_WIDGET");
BUILD_ASSERT(DT_NODE_EXISTS(DT_ALIAS(ledblue)),
             "An alias for a blue LED is not found for RGBLED_WIDGET");

// GPIO-based LED device and indices of red/green/blue LEDs inside its DT node
static const struct device *led_dev = DEVICE_DT_GET(LED_GPIO_NODE_ID);
static const uint8_t rgb_idx[] = {DT_NODE_CHILD_IDX(DT_ALIAS(ledred)),
                                  DT_NODE_CHILD_IDX(DT_ALIAS(ledgreen)),
                                  DT_NODE_CHILD_IDX(DT_ALIAS(ledblue))};

// Map from color values to names, for logging
static const char *color_names[] = {"black", "red", "green", "yellow",
                                    "blue", "magenta", "cyan", "white"};

// Hard-coded values
#define BATTERY_COLOR_HIGH        2  // Green
#define BATTERY_COLOR_MEDIUM      3  // Yellow
#define BATTERY_COLOR_LOW         1  // Red
#define BATTERY_COLOR_CRITICAL    1  // Red
#define BATTERY_COLOR_MISSING     1  // Red
#define CONN_COLOR_CONNECTED      4  // Blue
#define CONN_COLOR_ADVERTISING    6  // Cyan
#define CONN_COLOR_DISCONNECTED   5  // Magenta
#define BATTERY_BLINK_MS          250
#define CONN_BLINK_MS             250
#define INTERVAL_MS               100
#define BATTERY_LEVEL_HIGH        50
#define BATTERY_LEVEL_LOW         11
#define BATTERY_LEVEL_CRITICAL    10
#define CONN_CONNECTED_DURATION   5000  // 5 seconds
#define CONN_DISCONNECTED_BLINKS  5     // 5 blinks

// Log shorthands
#define LOG_CONN(index, status, color) \
    LOG_INF("Profile %d %s, blinking %s", index, status, color_names[color])
#define LOG_BATTERY(battery_level, color) \
    LOG_INF("Battery level %d, blinking %s", battery_level, color_names[color])

// A blink work item as specified by the color and duration
struct blink_item {
    uint8_t color;
    uint16_t duration_ms;
    uint16_t sleep_ms;
    uint8_t repeat; // Number of blinks (0 for continuous)
};

// Flag to indicate whether the initial boot up sequence is complete
static bool initialized = false;

// Caps Lock state
static bool caps_lock_active = false;

// Define message queue of blink work items
K_MSGQ_DEFINE(led_msgq, sizeof(struct blink_item), 16, 1);

#if IS_ENABLED(CONFIG_ZMK_BLE)
void indicate_connectivity(void) {
    struct blink_item blink = {
        .duration_ms = CONN_BLINK_MS,
        .sleep_ms = INTERVAL_MS,
        .repeat = 0
    };
    uint8_t profile_index = zmk_ble_active_profile_index();
    
    if (zmk_ble_active_profile_is_connected()) {
        LOG_CONN(profile_index, "connected", CONN_COLOR_CONNECTED);
        blink.color = CONN_COLOR_CONNECTED;
        blink.duration_ms = CONN_CONNECTED_DURATION;
        blink.sleep_ms = 0;
        blink.repeat = 1; // Solid for 5 seconds
    } else if (zmk_ble_active_profile_is_open()) {
        LOG_CONN(profile_index, "open", CONN_COLOR_ADVERTISING);
        blink.color = CONN_COLOR_ADVERTISING;
        blink.repeat = 0; // Continuous blinking
    } else {
        LOG_CONN(profile_index, "not connected", CONN_COLOR_DISCONNECTED);
        blink.color = CONN_COLOR_DISCONNECTED;
        blink.repeat = CONN_DISCONNECTED_BLINKS; // 5 blinks
    }

    k_msgq_put(&led_msgq, &blink, K_NO_WAIT);
}

static int led_output_listener_cb(const zmk_event_t *eh) {
    if (initialized && !caps_lock_active) {
        indicate_connectivity();
    }
    return 0;
}

ZMK_LISTENER(led_output_listener, led_output_listener_cb);
ZMK_SUBSCRIPTION(led_output_listener, zmk_ble_active_profile_changed);
#endif // IS_ENABLED(CONFIG_ZMK_BLE)

#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
static inline uint8_t get_battery_color(uint8_t battery_level) {
    if (battery_level == 0) {
        LOG_INF("Battery level undetermined (zero), blinking %s",
                color_names[BATTERY_COLOR_MISSING]);
        return BATTERY_COLOR_MISSING;
    }
    if (battery_level >= BATTERY_LEVEL_HIGH) {
        LOG_BATTERY(battery_level, BATTERY_COLOR_HIGH);
        return BATTERY_COLOR_HIGH;
    }
    if (battery_level >= BATTERY_LEVEL_LOW) {
        LOG_BATTERY(battery_level, BATTERY_COLOR_MEDIUM);
        return BATTERY_COLOR_MEDIUM;
    }
    LOG_BATTERY(battery_level, BATTERY_COLOR_LOW);
    return BATTERY_COLOR_LOW;
}

void indicate_battery(void) {
    struct blink_item blink = {
        .duration_ms = BATTERY_BLINK_MS,
        .sleep_ms = INTERVAL_MS,
        .repeat = 1 // Default: 1 blink
    };
    int retry = 0;

    uint8_t battery_level = zmk_battery_state_of_charge();
    while (battery_level == 0 && retry++ < 10) {
        k_sleep(K_MSEC(100));
        battery_level = zmk_battery_state_of_charge();
    }

    if (battery_level > 0 && battery_level <= BATTERY_LEVEL_CRITICAL) {
        LOG_BATTERY(battery_level, BATTERY_COLOR_CRITICAL);
        blink.color = BATTERY_COLOR_CRITICAL;
        blink.repeat = 0; // Continuous blinking
    } else {
        blink.color = get_battery_color(battery_level);
    }

    k_msgq_put(&led_msgq, &blink, K_NO_WAIT);
}

static int led_battery_listener_cb(const zmk_event_t *eh) {
    if (!initialized || caps_lock_active) {
        return 0;
    }

    uint8_t battery_level = as_zmk_battery_state_changed(eh)->state_of_charge;

    if (battery_level > 0 && battery_level <= BATTERY_LEVEL_CRITICAL) {
        LOG_BATTERY(battery_level, BATTERY_COLOR_CRITICAL);
        struct blink_item blink = {
            .duration_ms = BATTERY_BLINK_MS,
            .color = BATTERY_COLOR_CRITICAL,
            .sleep_ms = INTERVAL_MS,
            .repeat = 0 // Continuous blinking
        };
        k_msgq_put(&led_msgq, &blink, K_NO_WAIT);
    }
    return 0;
}

ZMK_LISTENER(led_battery_listener, led_battery_listener_cb);
ZMK_SUBSCRIPTION(led_battery_listener, zmk_battery_state_changed);
#endif // IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)

// Caps Lock listener
static int led_caps_lock_listener(const zmk_event_t *eh) {
    LOG_INF("Caps Lock event received");
    zmk_hid_indicators_t flags = zmk_hid_indicators_get_current_profile();
    unsigned int capsBit = 1 << (HID_USAGE_LED_CAPS_LOCK - 1);
    bool new_caps_state = (flags & capsBit) != 0;

    LOG_INF("Caps Lock flags: %d, new state: %d", flags, new_caps_state);

    if (new_caps_state != caps_lock_active) {
        caps_lock_active = new_caps_state;
        struct blink_item blink = {
            .color = caps_lock_active ? 7 : 0, // White (7) or Off (0)
            .duration_ms = 0, // Solid color
            .sleep_ms = 0,
            .repeat = 1
        };
        LOG_INF("Caps Lock %s, setting %s", caps_lock_active ? "ON" : "OFF", color_names[blink.color]);
        k_msgq_put(&led_msgq, &blink, K_NO_WAIT);
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(led_caps_lock_listener, led_caps_lock_listener);
ZMK_SUBSCRIPTION(led_caps_lock_listener, zmk_hid_indicators_changed);

uint8_t led_current_color = 0;

static void set_rgb_leds(uint8_t color, uint16_t duration_ms) {
    if (!device_is_ready(led_dev)) {
        LOG_ERR("LED device %s is not ready", led_dev->name);
        return;
    }

    for (uint8_t pos = 0; pos < 3; pos++) {
        uint8_t bit = BIT(pos);
        if ((bit & led_current_color) != (bit & color)) {
            if (bit & color) {
                led_on(led_dev, rgb_idx[pos]);
            } else {
                led_off(led_dev, rgb_idx[pos]);
            }
        }
    }
    if (duration_ms > 0) {
        k_sleep(K_MSEC(duration_ms));
    }
    led_current_color = color;
}

void led_process_thread(void *d0, void *d1, void *d2) {
    ARG_UNUSED(d0);
    ARG_UNUSED(d1);
    ARG_UNUSED(d2);

    LOG_INF("LED processing thread started");
    while (true) {
        struct blink_item blink;
        k_msgq_get(&led_msgq, &blink, K_FOREVER);

        // Prioritize Caps Lock: if active, set solid white and skip other blinks
        if (caps_lock_active && blink.color != 7) {
            LOG_DBG("Caps Lock active, forcing solid White LED");
            set_rgb_leds(7, 0); // White
            continue;
        }

        LOG_DBG("Got a blink item from msgq, color %d, duration %d, repeat %d", 
                blink.color, blink.duration_ms, blink.repeat);

        if (blink.repeat == 0) {
            // Continuous blinking
            while (true) {
                if (blink.color == led_current_color && blink.color > 0) {
                    set_rgb_leds(0, INTERVAL_MS);
                }
                set_rgb_leds(blink.color, blink.duration_ms);
                set_rgb_leds(caps_lock_active ? 7 : 0, blink.sleep_ms);
                // Check if Caps Lock state changed to break continuous loop
                if (caps_lock_active) {
                    break;
                }
            }
        } else {
            // Finite number of blinks
            for (uint8_t i = 0; i < blink.repeat; i++) {
                if (blink.color == led_current_color && blink.color > 0) {
                    set_rgb_leds(0, INTERVAL_MS);
                }
                set_rgb_leds(blink.color, blink.duration_ms);
                set_rgb_leds(caps_lock_active ? 7 : 0, blink.sleep_ms);
            }
        }
    }
}

K_THREAD_DEFINE(led_process_tid, 1024, led_process_thread, NULL, NULL, NULL,
                K_PRIO_PREEMPT(8), 0, 100);

void led_init_thread(void *d0, void *d1, void *d2) {
    ARG_UNUSED(d0);
    ARG_UNUSED(d1);
    ARG_UNUSED(d2);

    if (!device_is_ready(led_dev)) {
        LOG_ERR("LED device %s is not ready", led_dev->name);
        return;
    }

    // Ensure LED is off at start
    set_rgb_leds(0, 0);

#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
    LOG_INF("Indicating initial battery status");
    indicate_battery();
    k_sleep(K_MSEC(BATTERY_BLINK_MS + INTERVAL_MS));
#endif

#if IS_ENABLED(CONFIG_ZMK_BLE)
    LOG_INF("Indicating initial connectivity status");
    indicate_connectivity();
    k_sleep(K_MSEC(CONN_BLINK_MS + INTERVAL_MS));
#endif

    // Initialize Caps Lock state
    zmk_hid_indicators_t flags = zmk_hid_indicators_get_current_profile();
    unsigned int capsBit = 1 << (HID_USAGE_LED_CAPS_LOCK - 1);
    caps_lock_active = (flags & capsBit) != 0;
    struct blink_item blink = {
        .color = caps_lock_active ? 7 : 0, // White or Off
        .duration_ms = 0,
        .sleep_ms = 0,
        .repeat = 1
    };
    LOG_INF("Initial Caps Lock %s, setting %s", caps_lock_active ? "ON" : "OFF", color_names[blink.color]);
    k_msgq_put(&led_msgq, &blink, K_NO_WAIT);

    initialized = true;
    LOG_INF("Finished initializing LED widget");
}

K_THREAD_DEFINE(led_init_tid, 1024, led_init_thread, NULL, NULL, NULL,
                K_PRIO_PREEMPT(8), 0, 200);
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
#include <zmk/events/layer_state_changed.h>
#include <zmk/events/split_peripheral_status_changed.h>
#include <zmk/keymap.h>
#include <zmk/split/bluetooth/peripheral.h>
#include <zmk/hid_indicators.h>
#include <zmk/events/hid_indicators_changed.h>

#if __has_include(<zmk/split/central.h>)
#include <zmk/split/central.h>
#else
#include <zmk/split/bluetooth/central.h>
#endif

#include <zephyr/logging/log.h>

#include <zmk_rgbled_widget/widget.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define LED_GPIO_NODE_ID DT_COMPAT_GET_ANY_STATUS_OKAY(gpio_leds)

BUILD_ASSERT(DT_NODE_EXISTS(DT_ALIAS(led_red)),
             "An alias for a red LED is not found for RGBLED_WIDGET");
BUILD_ASSERT(DT_NODE_EXISTS(DT_ALIAS(led_green)),
             "An alias for a green LED is not found for RGBLED_WIDGET");
BUILD_ASSERT(DT_NODE_EXISTS(DT_ALIAS(led_blue)),
             "An alias for a blue LED is not found for RGBLED_WIDGET");

BUILD_ASSERT(!(SHOW_LAYER_CHANGE && SHOW_LAYER_COLORS),
             "CONFIG_RGBLED_WIDGET_SHOW_LAYER_CHANGE and CONFIG_RGBLED_WIDGET_SHOW_LAYER_COLORS "
             "are mutually exclusive");

// GPIO-based LED device and indices of red/green/blue LEDs inside its DT node
static const struct device *led_dev = DEVICE_DT_GET(LED_GPIO_NODE_ID);
static const uint8_t rgb_idx[] = {DT_NODE_CHILD_IDX(DT_ALIAS(led_red)),
                                  DT_NODE_CHILD_IDX(DT_ALIAS(led_green)),
                                  DT_NODE_CHILD_IDX(DT_ALIAS(led_blue))};

// Map from color values to names, for logging
static const char *color_names[] = {"black", "red",     "green", "yellow",
                                    "blue",  "magenta", "cyan",  "white"};

// Rainbow effect configuration
#define RAINBOW_BLINK_ON_MS   250
#define RAINBOW_BLINK_OFF_MS  250

#if SHOW_LAYER_COLORS
static const uint8_t layer_color_idx[] = {
    CONFIG_RGBLED_WIDGET_LAYER_0_COLOR,  CONFIG_RGBLED_WIDGET_LAYER_1_COLOR,
    CONFIG_RGBLED_WIDGET_LAYER_2_COLOR,  CONFIG_RGBLED_WIDGET_LAYER_3_COLOR,
    CONFIG_RGBLED_WIDGET_LAYER_4_COLOR,  CONFIG_RGBLED_WIDGET_LAYER_5_COLOR,
    CONFIG_RGBLED_WIDGET_LAYER_6_COLOR,  CONFIG_RGBLED_WIDGET_LAYER_7_COLOR,
    CONFIG_RGBLED_WIDGET_LAYER_8_COLOR,  CONFIG_RGBLED_WIDGET_LAYER_9_COLOR,
    CONFIG_RGBLED_WIDGET_LAYER_10_COLOR, CONFIG_RGBLED_WIDGET_LAYER_11_COLOR,
    CONFIG_RGBLED_WIDGET_LAYER_12_COLOR, CONFIG_RGBLED_WIDGET_LAYER_13_COLOR,
    CONFIG_RGBLED_WIDGET_LAYER_14_COLOR, CONFIG_RGBLED_WIDGET_LAYER_15_COLOR,
    CONFIG_RGBLED_WIDGET_LAYER_16_COLOR, CONFIG_RGBLED_WIDGET_LAYER_17_COLOR,
    CONFIG_RGBLED_WIDGET_LAYER_18_COLOR, CONFIG_RGBLED_WIDGET_LAYER_19_COLOR,
    CONFIG_RGBLED_WIDGET_LAYER_20_COLOR, CONFIG_RGBLED_WIDGET_LAYER_21_COLOR,
    CONFIG_RGBLED_WIDGET_LAYER_22_COLOR, CONFIG_RGBLED_WIDGET_LAYER_23_COLOR,
    CONFIG_RGBLED_WIDGET_LAYER_24_COLOR, CONFIG_RGBLED_WIDGET_LAYER_25_COLOR,
    CONFIG_RGBLED_WIDGET_LAYER_26_COLOR, CONFIG_RGBLED_WIDGET_LAYER_27_COLOR,
    CONFIG_RGBLED_WIDGET_LAYER_28_COLOR, CONFIG_RGBLED_WIDGET_LAYER_29_COLOR,
    CONFIG_RGBLED_WIDGET_LAYER_30_COLOR, CONFIG_RGBLED_WIDGET_LAYER_31_COLOR,
};
#endif

// Log shorthands
#define LOG_CONN_CENTRAL(index, status, color_label)                                               \
    LOG_INF("Profile %d %s, blinking %s", index, status,                                           \
            color_names[CONFIG_RGBLED_WIDGET_CONN_COLOR_##color_label])
#define LOG_CONN_PERIPHERAL(status, color_label)                                                   \
    LOG_INF("Peripheral %s, blinking %s", status,                                                  \
            color_names[CONFIG_RGBLED_WIDGET_CONN_COLOR_##color_label])
#define LOG_BATTERY(battery_level, color_label)                                                    \
    LOG_INF("Battery level %d, blinking %s", battery_level,                                        \
            color_names[CONFIG_RGBLED_WIDGET_BATTERY_COLOR_##color_label])

// A blink work item as specified by the color and duration
struct blink_item {
    uint8_t color;
    uint16_t duration_ms;
    uint16_t sleep_ms;
};

// Flag to indicate whether the initial boot up sequence is complete
static bool initialized = false;

// Caps Lock state
static bool caps_lock_active = false;

// Define message queue of blink work items, that will be processed by a separate thread
K_MSGQ_DEFINE(led_msgq, sizeof(struct blink_item), 16, 1);

#if IS_ENABLED(CONFIG_ZMK_BLE)
void indicate_connectivity(void) {
    struct blink_item blink = {.duration_ms = CONFIG_RGBLED_WIDGET_CONN_BLINK_MS};

#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    uint8_t profile_index = zmk_ble_active_profile_index();
    if (zmk_ble_active_profile_is_connected()) {
        LOG_CONN_CENTRAL(profile_index, "connected", CONNECTED);
        blink.color = CONFIG_RGBLED_WIDGET_CONN_COLOR_CONNECTED;
    } else if (zmk_ble_active_profile_is_open()) {
        LOG_CONN_CENTRAL(profile_index, "open", ADVERTISING);
        blink.color = CONFIG_RGBLED_WIDGET_CONN_COLOR_ADVERTISING;
    } else {
        LOG_CONN_CENTRAL(profile_index, "not connected", DISCONNECTED);
        blink.color = CONFIG_RGBLED_WIDGET_CONN_COLOR_DISCONNECTED;
    }
#else
    if (zmk_split_bt_peripheral_is_connected()) {
        LOG_CONN_PERIPHERAL("connected", CONNECTED);
        blink.color = CONFIG_RGBLED_WIDGET_CONN_COLOR_CONNECTED;
    } else {
        LOG_CONN_PERIPHERAL("not connected", DISCONNECTED);
        blink.color = CONFIG_RGBLED_WIDGET_CONN_COLOR_DISCONNECTED;
    }
#endif

    k_msgq_put(&led_msgq, &blink, K_NO_WAIT);
}

static int led_output_listener_cb(const zmk_event_t *eh) {
    if (initialized && !caps_lock_active) {
        indicate_connectivity();
    }
    return 0;
}

ZMK_LISTENER(led_output_listener, led_output_listener_cb);
#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
ZMK_SUBSCRIPTION(led_output_listener, zmk_ble_active_profile_changed);
#else
ZMK_SUBSCRIPTION(led_output_listener, zmk_split_peripheral_status_changed);
#endif
#endif // IS_ENABLED(CONFIG_ZMK_BLE)

#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
static inline uint8_t get_battery_color(uint8_t battery_level) {
    if (battery_level == 0) {
        LOG_INF("Battery level undetermined (zero), blinking %s",
                color_names[CONFIG_RGBLED_WIDGET_BATTERY_COLOR_MISSING]);
        return CONFIG_RGBLED_WIDGET_BATTERY_COLOR_MISSING;
    }
    if (battery_level >= CONFIG_RGBLED_WIDGET_BATTERY_LEVEL_HIGH) {
        LOG_BATTERY(battery_level, HIGH);
        return CONFIG_RGBLED_WIDGET_BATTERY_COLOR_HIGH;
    }
    if (battery_level >= CONFIG_RGBLED_WIDGET_BATTERY_LEVEL_LOW) {
        LOG_BATTERY(battery_level, MEDIUM);
        return CONFIG_RGBLED_WIDGET_BATTERY_COLOR_MEDIUM;
    }
    LOG_BATTERY(battery_level, LOW);
    return CONFIG_RGBLED_WIDGET_BATTERY_COLOR_LOW;
}

void indicate_battery(void) {
    struct blink_item blink = {.duration_ms = CONFIG_RGBLED_WIDGET_BATTERY_BLINK_MS};
    int retry = 0;

#if IS_ENABLED(CONFIG_RGBLED_WIDGET_BATTERY_SHOW_SELF) ||                                          \
    IS_ENABLED(CONFIG_RGBLED_WIDGET_BATTERY_SHOW_PERIPHERALS)
    uint8_t battery_level = zmk_battery_state_of_charge();
    while (battery_level == 0 && retry++ < 10) {
        k_sleep(K_MSEC(100));
        battery_level = zmk_battery_state_of_charge();
    };

    blink.color = get_battery_color(battery_level);
    k_msgq_put(&led_msgq, &blink, K_NO_WAIT);
#endif

#if IS_ENABLED(CONFIG_RGBLED_WIDGET_BATTERY_SHOW_PERIPHERALS) ||                                   \
    IS_ENABLED(CONFIG_RGBLED_WIDGET_BATTERY_SHOW_ONLY_PERIPHERALS)
    for (uint8_t i = 0; i < ZMK_SPLIT_BLE_PERIPHERAL_COUNT; i++) {
        uint8_t peripheral_level;
#if __has_include(<zmk/split/central.h>)
        int ret = zmk_split_central_get_peripheral_battery_level(i, &peripheral_level);
#else
        int ret = zmk_split_get_peripheral_battery_level(i, &peripheral_level);
#endif
        if (ret == 0) {
            retry = 0;
            while (peripheral_level == 0 && retry++ < (CONFIG_RGBLED_WIDGET_BATTERY_BLINK_MS +
                                                       CONFIG_RGBLED_WIDGET_INTERVAL_MS) /
                                                          100) {
                k_sleep(K_MSEC(100));
#if __has_include(<zmk/split/central.h>)
                zmk_split_central_get_peripheral_battery_level(i, &peripheral_level);
#else
                zmk_split_get_peripheral_battery_level(i, &peripheral_level);
#endif
            }

            LOG_INF("Got battery level for peripheral %d:", i);
            blink.color = get_battery_color(peripheral_level);
            k_msgq_put(&led_msgq, &blink, K_NO_WAIT);
        } else {
            LOG_ERR("Error looking up battery level for peripheral %d", i);
        }
    }
#endif
}

static int led_battery_listener_cb(const zmk_event_t *eh) {
    if (!initialized || caps_lock_active) {
        return 0;
    }

    uint8_t battery_level = as_zmk_battery_state_changed(eh)->state_of_charge;

    if (battery_level > 0 && battery_level <= CONFIG_RGBLED_WIDGET_BATTERY_LEVEL_CRITICAL) {
        LOG_BATTERY(battery_level, CRITICAL);

        struct blink_item blink = {.duration_ms = CONFIG_RGBLED_WIDGET_BATTERY_BLINK_MS,
                                   .color = CONFIG_RGBLED_WIDGET_BATTERY_COLOR_CRITICAL};
        k_msgq_put(&led_msgq, &blink, K_NO_WAIT);
    }
    return 0;
}

ZMK_LISTENER(led_battery_listener, led_battery_listener_cb);
ZMK_SUBSCRIPTION(led_battery_listener, zmk_battery_state_changed);
#endif // IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)

uint8_t led_layer_color = 0;
#if SHOW_LAYER_COLORS
void update_layer_color(void) {
    uint8_t index = zmk_keymap_highest_layer_active();

    if (led_layer_color != layer_color_idx[index]) {
        led_layer_color = layer_color_idx[index];
        struct blink_item color = {.color = led_layer_color};
        LOG_INF("Setting layer color to %s for layer %d", color_names[led_layer_color], index);
        k_msgq_put(&led_msgq, &color, K_NO_WAIT);
    }
}

static int led_layer_color_listener_cb(const zmk_event_t *eh) {
    if (initialized && !caps_lock_active) {
        update_layer_color();
    }
    return 0;
}

ZMK_LISTENER(led_layer_color_listener, led_layer_color_listener_cb);
ZMK_SUBSCRIPTION(led_layer_color_listener, zmk_layer_state_changed);
#endif // SHOW_LAYER_COLORS

#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
void indicate_layer(void) {
    uint8_t index = zmk_keymap_highest_layer_active();
    static const struct blink_item blink = {.duration_ms = CONFIG_RGBLED_WIDGET_LAYER_BLINK_MS,
                                            .color = CONFIG_RGBLED_WIDGET_LAYER_COLOR,
                                            .sleep_ms = CONFIG_RGBLED_WIDGET_LAYER_BLINK_MS};
    static const struct blink_item last_blink = {.duration_ms = CONFIG_RGBLED_WIDGET_LAYER_BLINK_MS,
                                                 .color = CONFIG_RGBLED_WIDGET_LAYER_COLOR};
    LOG_INF("Blinking %d times %s for layer change", index,
            color_names[CONFIG_RGBLED_WIDGET_LAYER_COLOR]);

    for (int i = 0; i < index; i++) {
        if (i < index - 1) {
            k_msgq_put(&led_msgq, &blink, K_NO_WAIT);
        } else {
            k_msgq_put(&led_msgq, &last_blink, K_NO_WAIT);
        }
    }
}
#endif

#if SHOW_LAYER_CHANGE
static struct k_work_delayable layer_indicate_work;

static int led_layer_listener_cb(const zmk_event_t *eh) {
    if (initialized && as_zmk_layer_state_changed(eh)->state && !caps_lock_active) {
        k_work_reschedule(&layer_indicate_work, K_MSEC(CONFIG_RGBLED_WIDGET_LAYER_DEBOUNCE_MS));
    }
    return 0;
}

static void indicate_layer_cb(struct k_work *work) { indicate_layer(); }

ZMK_LISTENER(led_layer_listener, led_layer_listener_cb);
ZMK_SUBSCRIPTION(led_layer_listener, zmk_layer_state_changed);
#endif // SHOW_LAYER_CHANGE

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
            .sleep_ms = 0
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

// Rainbow boot effect
static void rainbow_boot_effect(void) {
    LOG_INF("Starting rainbow boot effect");
    const uint8_t rainbow_colors[] = {
        1, // Red
        3, // Orange (same as Yellow)
        3, // Yellow
        2, // Green
        5, // Blue
        5, // Indigo (using Blue)
        6  // Purple (Magenta)
    };

    for (int i = 0; i < 7; i++) {
        set_rgb_leds(rainbow_colors[i], RAINBOW_BLINK_ON_MS);
        set_rgb_leds(0, RAINBOW_BLINK_OFF_MS);
    }
    LOG_INF("Rainbow boot effect completed");
}

void led_process_thread(void *d0, void *d1, void *d2) {
    ARG_UNUSED(d0);
    ARG_UNUSED(d1);
    ARG_UNUSED(d2);

#if SHOW_LAYER_CHANGE
    k_work_init_delayable(&layer_indicate_work, indicate_layer_cb);
#endif

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

        LOG_DBG("Got a blink item from msgq, color %d, duration %d", blink.color, blink.duration_ms);

        if (blink.duration_ms > 0) {
            // Blink the LEDs, using a separation blink if necessary
            if (blink.color == led_current_color && blink.color > 0) {
                set_rgb_leds(0, CONFIG_RGBLED_WIDGET_INTERVAL_MS);
            }
            set_rgb_leds(blink.color, blink.duration_ms);
            if (blink.color == led_layer_color && blink.color > 0) {
                set_rgb_leds(0, CONFIG_RGBLED_WIDGET_INTERVAL_MS);
            }
            // Restore layer color or off state
            set_rgb_leds(caps_lock_active ? 7 : led_layer_color,
                         blink.sleep_ms > 0 ? blink.sleep_ms : CONFIG_RGBLED_WIDGET_INTERVAL_MS);
        } else {
            LOG_DBG("Got a color item from msgq, color %d", blink.color);
            set_rgb_leds(blink.color, 0);
        }
    }
}

K_THREAD_DEFINE(led_process_tid, 1024, led_process_thread, NULL, NULL, NULL,
                K_PRIO_PREEMPT(8), 0, 100);

void led_init_thread(void *d0, void *d1, void *d2) {
    ARG_UNUSED(d0);
    ARG_UNUSED(d1);
    ARG_UNUSED(d2);

    // Run rainbow boot effect
    rainbow_boot_effect();
    set_rgb_leds(0, 0); // Ensure LED off after rainbow

#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
    LOG_INF("Indicating initial battery status");
    indicate_battery();
    k_sleep(K_MSEC(CONFIG_RGBLED_WIDGET_BATTERY_BLINK_MS + CONFIG_RGBLED_WIDGET_INTERVAL_MS));
#endif

#if IS_ENABLED(CONFIG_ZMK_BLE)
    LOG_INF("Indicating initial connectivity status");
    indicate_connectivity();
    k_sleep(K_MSEC(CONFIG_RGBLED_WIDGET_CONN_BLINK_MS + CONFIG_RGBLED_WIDGET_INTERVAL_MS));
#endif

#if SHOW_LAYER_COLORS
    LOG_INF("Setting initial layer color");
    update_layer_color();
#endif

    // Initialize Caps Lock state
    zmk_hid_indicators_t flags = zmk_hid_indicators_get_current_profile();
    unsigned int capsBit = 1 << (HID_USAGE_LED_CAPS_LOCK - 1);
    caps_lock_active = (flags & capsBit) != 0;
    struct blink_item blink = {
        .color = caps_lock_active ? 7 : 0, // White or Off
        .duration_ms = 0,
        .sleep_ms = 0
    };
    LOG_INF("Initial Caps Lock %s, setting %s", caps_lock_active ? "ON" : "OFF", color_names[blink.color]);
    k_msgq_put(&led_msgq, &blink, K_NO_WAIT);

    initialized = true;
    LOG_INF("Finished initializing LED widget");
}

K_THREAD_DEFINE(led_init_tid, 1024, led_init_thread, NULL, NULL, NULL,
                K_PRIO_PREEMPT(8), 0, 200);
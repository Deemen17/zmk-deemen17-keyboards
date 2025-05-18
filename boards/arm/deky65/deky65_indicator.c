#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(led_controller, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/hid_indicators.h>
#include <zmk/events/hid_indicators_changed.h>
#include <zmk/ble.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/battery.h>
#include <zmk/events/battery_state_changed.h>

// LED GPIO Definitions
#define LED_NODE_R DT_ALIAS(ledred)
#define LED_NODE_G DT_ALIAS(ledgreen)
#define LED_NODE_B DT_ALIAS(ledblue)

static const struct gpio_dt_spec LED_R = GPIO_DT_SPEC_GET(LED_NODE_R, gpios);
static const struct gpio_dt_spec LED_G = GPIO_DT_SPEC_GET(LED_NODE_G, gpios);
static const struct gpio_dt_spec LED_B = GPIO_DT_SPEC_GET(LED_NODE_B, gpios);

// LED Colors
#define COLOR_OFF      0
#define COLOR_WHITE    1
#define COLOR_GREEN    2
#define COLOR_YELLOW   3
#define COLOR_RED      4
#define COLOR_BLUE     5

// Battery Thresholds
#define BATTERY_HIGH       50
#define BATTERY_CRITICAL   10

// Timing Configurations
#define BLINK_COUNT        3
#define BLINK_ON_MS       250
#define BLINK_OFF_MS      250
#define RAINBOW_DELAY_MS  150

// System States
static bool caps_lock_active = false;
static uint8_t battery_level = 0;
static bool ble_connected = false;
static bool ble_open = false;

// Message Queue for LED control
struct led_state {
    uint8_t color;
    bool blink;
    bool force_override;
};

K_MSGQ_DEFINE(led_msgq, sizeof(struct led_state), 10, 4);

// Initialize LED GPIOs
static int init_leds(void) {
    int ret;
    
    if (!device_is_ready(LED_R.port) || 
        !device_is_ready(LED_G.port) || 
        !device_is_ready(LED_B.port)) {
        LOG_ERR("LED GPIOs not ready");
        return -ENODEV;
    }

    if ((ret = gpio_pin_configure_dt(&LED_R, GPIO_OUTPUT)) ||
        (ret = gpio_pin_configure_dt(&LED_G, GPIO_OUTPUT)) ||
        (ret = gpio_pin_configure_dt(&LED_B, GPIO_OUTPUT))) {
        LOG_ERR("Failed to configure LEDs");
        return ret;
    }

    // Turn off all LEDs initially
    gpio_pin_set_dt(&LED_R, 1);
    gpio_pin_set_dt(&LED_G, 1);
    gpio_pin_set_dt(&LED_B, 1);
    
    return 0;
}

// Set LED color
static void set_led_color(uint8_t color) {
    switch (color) {
        case COLOR_WHITE:
            gpio_pin_set_dt(&LED_R, 0);
            gpio_pin_set_dt(&LED_G, 0);
            gpio_pin_set_dt(&LED_B, 0);
            break;
        case COLOR_GREEN:
            gpio_pin_set_dt(&LED_R, 1);
            gpio_pin_set_dt(&LED_G, 0);
            gpio_pin_set_dt(&LED_B, 1);
            break;
        case COLOR_YELLOW:
            gpio_pin_set_dt(&LED_R, 0);
            gpio_pin_set_dt(&LED_G, 0);
            gpio_pin_set_dt(&LED_B, 1);
            break;
        case COLOR_RED:
            gpio_pin_set_dt(&LED_R, 0);
            gpio_pin_set_dt(&LED_G, 1);
            gpio_pin_set_dt(&LED_B, 1);
            break;
        case COLOR_BLUE:
            gpio_pin_set_dt(&LED_R, 1);
            gpio_pin_set_dt(&LED_G, 1);
            gpio_pin_set_dt(&LED_B, 0);
            break;
        case COLOR_OFF:
        default:
            gpio_pin_set_dt(&LED_R, 1);
            gpio_pin_set_dt(&LED_G, 1);
            gpio_pin_set_dt(&LED_B, 1);
            break;
    }
}

// Blink LED with specified color
static void blink_led(uint8_t color) {
    for (int i = 0; i < BLINK_COUNT; i++) {
        set_led_color(color);
        k_msleep(BLINK_ON_MS);
        set_led_color(COLOR_OFF);
        k_msleep(BLINK_OFF_MS);
    }
}

// Rainbow boot effect
static void rainbow_boot_effect(void) {
    const uint8_t colors[] = {COLOR_RED, COLOR_YELLOW, COLOR_GREEN, COLOR_BLUE};
    for (int i = 0; i < ARRAY_SIZE(colors); i++) {
        set_led_color(colors[i]);
        k_msleep(RAINBOW_DELAY_MS);
        set_led_color(COLOR_OFF);
        k_msleep(RAINBOW_DELAY_MS);
    }
}

// Update LED based on caps lock state
static void update_caps_lock(void) {
    struct led_state state = {
        .color = caps_lock_active ? COLOR_WHITE : COLOR_OFF,
        .blink = false,
        .force_override = true
    };
    k_msgq_put(&led_msgq, &state, K_NO_WAIT);
}

// Update LED based on battery state
static void update_battery(void) {
    struct led_state state = {
        .blink = battery_level > BATTERY_CRITICAL,
        .force_override = battery_level <= BATTERY_CRITICAL
    };

    if (battery_level >= BATTERY_HIGH) {
        state.color = COLOR_GREEN;
    } else if (battery_level > BATTERY_CRITICAL) {
        state.color = COLOR_YELLOW;
    } else {
        state.color = COLOR_RED;
    }

    k_msgq_put(&led_msgq, &state, K_NO_WAIT);
}

// Update LED based on Bluetooth state
static void update_bluetooth(void) {
    struct led_state state = {
        .blink = true,
        .force_override = false
    };

    if (ble_connected) {
        state.color = COLOR_BLUE;
    } else if (ble_open) {
        state.color = COLOR_YELLOW;
    } else {
        state.color = COLOR_RED;
    }

    k_msgq_put(&led_msgq, &state, K_NO_WAIT);
}

// Event Handlers
static int caps_lock_handler(const zmk_event_t *eh) {
    zmk_hid_indicators_t flags = zmk_hid_indicators_get_current_profile();
    bool new_state = flags & (1 << (HID_USAGE_LED_CAPS_LOCK - 1));
    
    if (new_state != caps_lock_active) {
        caps_lock_active = new_state;
        update_caps_lock();
    }
    return ZMK_EV_EVENT_BUBBLE;
}

static int battery_handler(const zmk_event_t *eh) {
    const struct zmk_battery_state_changed *ev = as_zmk_battery_state_changed(eh);
    if (ev && ev->state_of_charge != battery_level) {
        battery_level = ev->state_of_charge;
        update_battery();
    }
    return ZMK_EV_EVENT_BUBBLE;
}

static int bluetooth_handler(const zmk_event_t *eh) {
    bool new_connected = zmk_ble_active_profile_is_connected();
    bool new_open = zmk_ble_active_profile_is_open();
    
    if (new_connected != ble_connected || new_open != ble_open) {
        ble_connected = new_connected;
        ble_open = new_open;
        update_bluetooth();
    }
    return ZMK_EV_EVENT_BUBBLE;
}

// LED Processing Thread
static void led_thread(void *p1, void *p2, void *p3) {
    while (true) {
        struct led_state state;
        if (k_msgq_get(&led_msgq, &state, K_FOREVER) == 0) {
            if (state.blink) {
                blink_led(state.color);
            } else {
                set_led_color(state.color);
            }
        }
    }
}

// Initialization
static int led_init(const struct device *dev) {
    ARG_UNUSED(dev);
    
    if (init_leds() != 0) {
        return -EIO;
    }

    // Run rainbow effect first
    rainbow_boot_effect();
    
    // Get initial states
    battery_level = zmk_battery_state_of_charge();
    ble_connected = zmk_ble_active_profile_is_connected();
    ble_open = zmk_ble_active_profile_is_open();
    caps_lock_active = zmk_hid_indicators_get_current_profile() & 
                      (1 << (HID_USAGE_LED_CAPS_LOCK - 1));

    // Show initial states
    update_battery();
    k_msleep(BLINK_COUNT * (BLINK_ON_MS + BLINK_OFF_MS) + 100);
    update_bluetooth();
    k_msleep(BLINK_COUNT * (BLINK_ON_MS + BLINK_OFF_MS) + 100);
    update_caps_lock();

    return 0;
}

// Register listeners and thread
ZMK_LISTENER(led_caps_lock, caps_lock_handler);
ZMK_SUBSCRIPTION(led_caps_lock, zmk_hid_indicators_changed);

ZMK_LISTENER(led_battery, battery_handler);
ZMK_SUBSCRIPTION(led_battery, zmk_battery_state_changed);

ZMK_LISTENER(led_bluetooth, bluetooth_handler);
ZMK_SUBSCRIPTION(led_bluetooth, zmk_ble_active_profile_changed);

K_THREAD_DEFINE(led_thread_id, 512, led_thread, NULL, NULL, NULL,
                K_PRIO_PREEMPT(7), 0, 0);

SYS_INIT(led_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
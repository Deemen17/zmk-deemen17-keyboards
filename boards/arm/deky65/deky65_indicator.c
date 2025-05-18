#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zmk/hid_indicators.h>
#include <zmk/events/hid_indicators_changed.h>
#include <zmk/ble.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/battery.h>
#include <zmk/events/battery_state_changed.h>

LOG_MODULE_REGISTER(led_controller, CONFIG_ZMK_LOG_LEVEL);

/* Hardware Config - GPIO_ACTIVE_LOW (Common Anode) */
#define LED_R_NODE DT_ALIAS(ledred)
#define LED_G_NODE DT_ALIAS(ledgreen)
#define LED_B_NODE DT_ALIAS(ledblue)

static const struct gpio_dt_spec led_r = GPIO_DT_SPEC_GET(LED_R_NODE, gpios);
static const struct gpio_dt_spec led_g = GPIO_DT_SPEC_GET(LED_G_NODE, gpios);
static const struct gpio_dt_spec led_b = GPIO_DT_SPEC_GET(LED_B_NODE, gpios);

/* LED Color Definitions (Common Anode + GPIO_ACTIVE_LOW) */
enum led_color {
    COLOR_OFF   = 0b111,  // All LEDs OFF (GPIO=1)
    COLOR_RED   = 0b011,  // Red ON (GPIO=0), Green/Blue OFF
    COLOR_GREEN = 0b101,  // Green ON, Red/Blue OFF
    COLOR_BLUE  = 0b110,  // Blue ON, Red/Green OFF
    COLOR_YELLOW= 0b001,  // Red+Green ON (GPIO=0), Blue OFF
    COLOR_CYAN  = 0b100,  // Green+Blue ON, Red OFF
    COLOR_PURPLE= 0b010,  // Red+Blue ON, Green OFF
    COLOR_WHITE = 0b000   // All LEDs ON (GPIO=0)
};

/* System Priorities */
enum led_priority {
    PRIO_IDLE,
    PRIO_BLE,
    PRIO_BATTERY,
    PRIO_CAPS_LOCK,
    PRIO_CRITICAL
};

/* Timing Constants */
#define BLINK_INTERVAL_MS  250
#define RAINBOW_INTERVAL_MS 150
#define DEBOUNCE_MS        500

/* Global State */
static struct {
    atomic_t current_color;
    atomic_t current_priority;
    struct k_timer blink_timer;
    struct k_timer rainbow_timer;
    struct k_timer debounce_timer;
    uint8_t battery_level;
    bool caps_lock;
    bool ble_connected;
    bool ble_open;
} led_state;

/* LED Hardware Initialization */
static int init_leds(void) {
    int ret = 0;
    
    if (!device_is_ready(led_r.port) || 
        !device_is_ready(led_g.port) || 
        !device_is_ready(led_b.port)) {
        LOG_ERR("LED GPIO devices not ready");
        return -ENODEV;
    }

    ret |= gpio_pin_configure_dt(&led_r, GPIO_OUTPUT_INACTIVE);
    ret |= gpio_pin_configure_dt(&led_g, GPIO_OUTPUT_INACTIVE);
    ret |= gpio_pin_configure_dt(&led_b, GPIO_OUTPUT_INACTIVE);
    
    // Start with all LEDs off (active low)
    gpio_pin_set_dt(&led_r, 0);
    gpio_pin_set_dt(&led_g, 0);
    gpio_pin_set_dt(&led_b, 0);
    
    return ret;
}

/* LED Control Function */
static void set_led_color(uint8_t color) {
    gpio_pin_set_dt(&led_r, (color >> 2) & 1);  // Bit 2 (Red)
    gpio_pin_set_dt(&led_g, (color >> 1) & 1);  // Bit 1 (Green)
    gpio_pin_set_dt(&led_b, (color >> 0) & 1);  // Bit 0 (Blue)
}

/* Blink Handler */
static void blink_handler(struct k_timer *timer) {
    static bool led_on = false;
    led_on = !led_on;
    set_led_color(led_on ? atomic_get(&led_state.current_color) : COLOR_OFF);
}

/* Rainbow Effect */
static void rainbow_handler(struct k_timer *timer) {
    static uint8_t rainbow_pos = 0;
    const uint8_t rainbow[] = {
        COLOR_RED,
        COLOR_YELLOW,
        COLOR_GREEN,
        COLOR_CYAN,
        COLOR_BLUE,
        COLOR_PURPLE,
        COLOR_WHITE
    };
    set_led_color(rainbow[rainbow_pos++ % ARRAY_SIZE(rainbow)]);
}

/* Update LED State Based on Priority */
static void update_led_state(void) {
    uint8_t new_color = COLOR_OFF;
    enum led_priority new_priority = PRIO_IDLE;

    // Priority 1: Caps Lock
    if (led_state.caps_lock) {
        new_color = COLOR_WHITE;
        new_priority = PRIO_CAPS_LOCK;
    }
    // Priority 2: Critical Battery
    else if (led_state.battery_level <= 10) {
        new_color = COLOR_RED;
        new_priority = PRIO_CRITICAL;
    }
    // Priority 3: Normal Battery
    else if (led_state.battery_level <= 50) {
        new_color = (led_state.battery_level <= 30) ? COLOR_YELLOW : COLOR_GREEN;
        new_priority = PRIO_BATTERY;
    }
    // Priority 4: BLE Status
    else if (led_state.ble_connected) {
        new_color = COLOR_BLUE;
        new_priority = PRIO_BLE;
    } else if (led_state.ble_open) {
        new_color = COLOR_YELLOW;
        new_priority = PRIO_BLE;
    }

    // Apply if higher or equal priority
    if (new_priority >= atomic_get(&led_state.current_priority)) {
        atomic_set(&led_state.current_color, new_color);
        atomic_set(&led_state.current_priority, new_priority);
        
        if (new_priority == PRIO_CRITICAL) {
            k_timer_start(&led_state.blink_timer, K_MSEC(BLINK_INTERVAL_MS), K_MSEC(BLINK_INTERVAL_MS));
        } else {
            k_timer_stop(&led_state.blink_timer);
            set_led_color(new_color);
        }
    }
}

/* Event Handlers */
static int on_caps_lock(const zmk_event_t *eh) {
    bool new_state = zmk_hid_indicators_get_current_profile() & (1 << (HID_USAGE_LED_CAPS_LOCK - 1));
    if (new_state != led_state.caps_lock) {
        led_state.caps_lock = new_state;
        update_led_state();
    }
    return ZMK_EV_EVENT_BUBBLE;
}

static int on_battery(const zmk_event_t *eh) {
    const struct zmk_battery_state_changed *ev = as_zmk_battery_state_changed(eh);
    if (ev && ev->state_of_charge != led_state.battery_level) {
        led_state.battery_level = ev->state_of_charge;
        k_timer_start(&led_state.debounce_timer, K_MSEC(DEBOUNCE_MS), K_NO_WAIT);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

static void on_debounce(struct k_timer *timer) {
    update_led_state();
}

static int on_ble(const zmk_event_t *eh) {
    bool new_connected = zmk_ble_active_profile_is_connected();
    bool new_open = zmk_ble_active_profile_is_open();
    
    if (new_connected != led_state.ble_connected || new_open != led_state.ble_open) {
        led_state.ble_connected = new_connected;
        led_state.ble_open = new_open;
        update_led_state();
    }
    return ZMK_EV_EVENT_BUBBLE;
}

/* LED Test Function */
static void test_leds(void) {
    const uint8_t test_colors[] = {
        COLOR_RED,
        COLOR_GREEN,
        COLOR_BLUE,
        COLOR_YELLOW,
        COLOR_CYAN,
        COLOR_PURPLE,
        COLOR_WHITE,
        COLOR_OFF
    };
    
    for (int i = 0; i < ARRAY_SIZE(test_colors); i++) {
        set_led_color(test_colors[i]);
        k_msleep(500);
    }
}

/* Main Initialization */
static int led_init(const struct device *dev) {
    ARG_UNUSED(dev);
    
    if (init_leds() != 0) {
        LOG_ERR("Failed to initialize LEDs");
        return -EIO;
    }

    // Initialize timers
    k_timer_init(&led_state.blink_timer, blink_handler, NULL);
    k_timer_init(&led_state.rainbow_timer, rainbow_handler, NULL);
    k_timer_init(&led_state.debounce_timer, on_debounce, NULL);

    // Run rainbow effect on boot
    LOG_INF("Starting rainbow boot effect");
    k_timer_start(&led_state.rainbow_timer, K_MSEC(RAINBOW_INTERVAL_MS), K_MSEC(RAINBOW_INTERVAL_MS));
    k_msleep(ARRAY_SIZE(rainbow) * RAINBOW_INTERVAL_MS * 2);
    k_timer_stop(&led_state.rainbow_timer);

    // Test all LED colors
    test_leds();

    // Get initial states
    led_state.battery_level = zmk_battery_state_of_charge();
    led_state.ble_connected = zmk_ble_active_profile_is_connected();
    led_state.ble_open = zmk_ble_active_profile_is_open();
    led_state.caps_lock = zmk_hid_indicators_get_current_profile() & (1 << (HID_USAGE_LED_CAPS_LOCK - 1));

    // Apply initial state
    update_led_state();

    return 0;
}

/* ZMK Event Subscriptions */
ZMK_LISTENER(led_controller, on_caps_lock);
ZMK_SUBSCRIPTION(led_controller, zmk_hid_indicators_changed);

ZMK_LISTENER(battery_listener, on_battery);
ZMK_SUBSCRIPTION(battery_listener, zmk_battery_state_changed);

ZMK_LISTENER(ble_listener, on_ble);
ZMK_SUBSCRIPTION(ble_listener, zmk_ble_active_profile_changed);

SYS_INIT(led_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
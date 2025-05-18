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

/* Hardware Config */
#define LED_R_NODE DT_ALIAS(ledred)
#define LED_G_NODE DT_ALIAS(ledgreen)
#define LED_B_NODE DT_ALIAS(ledblue)

static const struct gpio_dt_spec led_r = GPIO_DT_SPEC_GET(LED_R_NODE, gpios);
static const struct gpio_dt_spec led_g = GPIO_DT_SPEC_GET(LED_G_NODE, gpios);
static const struct gpio_dt_spec led_b = GPIO_DT_SPEC_GET(LED_B_NODE, gpios);

/* LED States */
enum led_color {
    COLOR_OFF,
    COLOR_WHITE,
    COLOR_GREEN,
    COLOR_YELLOW,
    COLOR_RED,
    COLOR_BLUE,
    COLOR_RAINBOW
};

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

/* System State */
struct {
    atomic_t color;
    atomic_t priority;
    struct k_timer blink_timer;
    struct k_timer rainbow_timer;
    struct k_timer debounce_timer;
    uint8_t battery_level;
    bool caps_lock;
    bool ble_connected;
    bool ble_open;
} led_state;

/* Message Queue */
K_MSGQ_DEFINE(led_msgq, sizeof(struct led_state), 10, 4);

/* LED Hardware Control */
static int init_leds(void) {
    int ret = 0;
    
    if (!device_is_ready(led_r.port) || 
        !device_is_ready(led_g.port) || 
        !device_is_ready(led_b.port)) {
        LOG_ERR("LED devices not ready");
        return -ENODEV;
    }

    ret |= gpio_pin_configure_dt(&led_r, GPIO_OUTPUT);
    ret |= gpio_pin_configure_dt(&led_g, GPIO_OUTPUT);
    ret |= gpio_pin_configure_dt(&led_b, GPIO_OUTPUT);
    
    gpio_pin_set_dt(&led_r, 1);
    gpio_pin_set_dt(&led_g, 1);
    gpio_pin_set_dt(&led_b, 1);
    
    return ret;
}

/* Optimized LED Control */
static void set_led_color(uint8_t color) {
    switch (color) {
        case COLOR_WHITE:
            gpio_pin_set_dt(&led_r, 0); gpio_pin_set_dt(&led_g, 0); gpio_pin_set_dt(&led_b, 0);
            break;
        case COLOR_GREEN:
            gpio_pin_set_dt(&led_r, 1); gpio_pin_set_dt(&led_g, 0); gpio_pin_set_dt(&led_b, 1);
            break;
        case COLOR_YELLOW:
            gpio_pin_set_dt(&led_r, 0); gpio_pin_set_dt(&led_g, 0); gpio_pin_set_dt(&led_b, 1);
            break;
        case COLOR_RED:
            gpio_pin_set_dt(&led_r, 0); gpio_pin_set_dt(&led_g, 1); gpio_pin_set_dt(&led_b, 1);
            break;
        case COLOR_BLUE:
            gpio_pin_set_dt(&led_r, 1); gpio_pin_set_dt(&led_g, 1); gpio_pin_set_dt(&led_b, 0);
            break;
        default: // OFF
            gpio_pin_set_dt(&led_r, 1); gpio_pin_set_dt(&led_g, 1); gpio_pin_set_dt(&led_b, 1);
    }
}

/* Asynchronous Blink Handler */
static void blink_handler(struct k_timer *timer) {
    static bool led_on = false;
    led_on = !led_on;
    set_led_color(led_on ? atomic_get(&led_state.color) : COLOR_OFF);
}

/* Rainbow Effect Handler */
static void rainbow_handler(struct k_timer *timer) {
    static uint8_t rainbow_pos = 0;
    const uint8_t rainbow[] = {COLOR_RED, COLOR_YELLOW, COLOR_GREEN, COLOR_BLUE};
    set_led_color(rainbow[rainbow_pos++ % ARRAY_SIZE(rainbow)]);
}

/* State Update Functions */
static void update_led_state(void) {
    struct led_state new_state = {
        .color = COLOR_OFF,
        .priority = PRIO_IDLE
    };

    // Priority 1: Caps Lock
    if (led_state.caps_lock) {
        new_state.color = COLOR_WHITE;
        new_state.priority = PRIO_CAPS_LOCK;
    }
    
    // Priority 2: Critical Battery
    else if (led_state.battery_level <= 10) {
        new_state.color = COLOR_RED;
        new_state.priority = PRIO_CRITICAL;
        k_timer_start(&led_state.blink_timer, K_MSEC(BLINK_INTERVAL_MS), K_MSEC(BLINK_INTERVAL_MS));
    }
    
    // Priority 3: Normal Battery
    else if (led_state.battery_level <= 50) {
        new_state.color = (led_state.battery_level <= 30) ? COLOR_YELLOW : COLOR_GREEN;
        new_state.priority = PRIO_BATTERY;
    }
    
    // Priority 4: BLE Status
    else if (led_state.ble_connected) {
        new_state.color = COLOR_BLUE;
        new_state.priority = PRIO_BLE;
    } else if (led_state.ble_open) {
        new_state.color = COLOR_YELLOW;
        new_state.priority = PRIO_BLE;
    }

    // Apply state if higher priority
    if (new_state.priority >= atomic_get(&led_state.priority)) {
        atomic_set(&led_state.color, new_state.color);
        atomic_set(&led_state.priority, new_state.priority);
        
        if (new_state.priority == PRIO_CRITICAL) {
            k_timer_start(&led_state.blink_timer, K_MSEC(BLINK_INTERVAL_MS), K_MSEC(BLINK_INTERVAL_MS));
        } else {
            k_timer_stop(&led_state.blink_timer);
            set_led_color(new_state.color);
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
    if (ev && abs(ev->state_of_charge - led_state.battery_level) > 5) {
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

/* Main LED Thread */
static void led_thread(void *p1, void *p2, void *p3) {
    while (true) {
        update_led_state();
        k_msleep(100); // Reduce CPU usage
    }
}

/* Initialization */
static int led_init(const struct device *dev) {
    ARG_UNUSED(dev);
    
    if (init_leds() != 0) return -EIO;

    // Initialize timers
    k_timer_init(&led_state.blink_timer, blink_handler, NULL);
    k_timer_init(&led_state.rainbow_timer, rainbow_handler, NULL);
    k_timer_init(&led_state.debounce_timer, on_debounce, NULL);

    // Rainbow boot effect
    k_timer_start(&led_state.rainbow_timer, K_MSEC(RAINBOW_INTERVAL_MS), K_MSEC(RAINBOW_INTERVAL_MS));
    k_msleep(ARRAY_SIZE(rainbow) * RAINBOW_INTERVAL_MS * 2);
    k_timer_stop(&led_state.rainbow_timer);

    // Initial state
    led_state.battery_level = zmk_battery_state_of_charge();
    led_state.ble_connected = zmk_ble_active_profile_is_connected();
    led_state.ble_open = zmk_ble_active_profile_is_open();
    led_state.caps_lock = zmk_hid_indicators_get_current_profile() & (1 << (HID_USAGE_LED_CAPS_LOCK - 1));

    update_led_state();

    return 0;
}

/* ZMK Hooks */
ZMK_LISTENER(led_controller, on_caps_lock);
ZMK_SUBSCRIPTION(led_controller, zmk_hid_indicators_changed);

ZMK_LISTENER(battery_listener, on_battery);
ZMK_SUBSCRIPTION(battery_listener, zmk_battery_state_changed);

ZMK_LISTENER(ble_listener, on_ble);
ZMK_SUBSCRIPTION(ble_listener, zmk_ble_active_profile_changed);

K_THREAD_DEFINE(led_thread_id, 512, led_thread, NULL, NULL, NULL, K_PRIO_PREEMPT(7), 0, 0);
SYS_INIT(led_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
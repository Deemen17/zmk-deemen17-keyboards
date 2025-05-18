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
#define BLINK_INTERVAL_MS  300
#define RAINBOW_INTERVAL_MS 400
#define DEBOUNCE_MS        500

/* Function Declarations */
static void set_led_color(uint8_t color);

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

/* LED Control Functions */
static void set_led_color(uint8_t color) {
    const char* color_name;
    switch(color) {
        case COLOR_OFF: color_name = "OFF"; break;
        case COLOR_RED: color_name = "RED"; break;
        case COLOR_GREEN: color_name = "GREEN"; break;
        case COLOR_BLUE: color_name = "BLUE"; break;
        case COLOR_YELLOW: color_name = "YELLOW"; break;
        case COLOR_CYAN: color_name = "CYAN"; break;
        case COLOR_PURPLE: color_name = "PURPLE"; break;
        case COLOR_WHITE: color_name = "WHITE"; break;
        default: color_name = "UNKNOWN"; break;
    }
    
    bool red = (color >> 2) & 1;
    bool green = (color >> 1) & 1;
    bool blue = color & 1;
    
    LOG_DBG("LED Color Change:");
    LOG_DBG("- Color: %s (0b%d%d%d)", color_name, red, green, blue);
    LOG_DBG("- Setting pins (Common Anode - Active LOW):");
    
    gpio_pin_set_dt(&led_r, red);
    gpio_pin_set_dt(&led_g, green);
    gpio_pin_set_dt(&led_b, blue);
    
    LOG_DBG("  RED:   GPIO=%d (%s)", red, red ? "OFF" : "ON");
    LOG_DBG("  GREEN: GPIO=%d (%s)", green, green ? "OFF" : "ON");
    LOG_DBG("  BLUE:  GPIO=%d (%s)", blue, blue ? "OFF" : "ON");
}

static void blink_handler(struct k_timer *timer) {
    static bool led_on = false;
    led_on = !led_on;
    set_led_color(led_on ? atomic_get(&led_state.current_color) : COLOR_OFF);
}

static const uint8_t rainbow_colors[] = {
    COLOR_RED,
    COLOR_YELLOW,
    COLOR_GREEN,
    COLOR_BLUE,
    COLOR_CYAN,
    COLOR_PURPLE,
    COLOR_WHITE
};

/* Rainbow Effect */
static void rainbow_handler(struct k_timer *timer) {
    static uint8_t rainbow_pos = 0;
    set_led_color(rainbow_colors[rainbow_pos++ % ARRAY_SIZE(rainbow_colors)]);
}

/* LED Hardware Initialization */
static int init_leds(void) {
    int ret = 0;
    
    if (!device_is_ready(led_r.port) || 
        !device_is_ready(led_g.port) || 
        !device_is_ready(led_b.port)) {
        LOG_ERR("LED GPIO devices not ready");
        return -ENODEV;
    }

    // Chỉ cấu hình là output, không thêm flag
    ret |= gpio_pin_configure_dt(&led_r, GPIO_OUTPUT);
    ret |= gpio_pin_configure_dt(&led_g, GPIO_OUTPUT); 
    ret |= gpio_pin_configure_dt(&led_b, GPIO_OUTPUT);
    
    // Đặt tất cả LED về trạng thái OFF (HIGH for Common Anode)
    gpio_pin_set_dt(&led_r, 1);
    gpio_pin_set_dt(&led_g, 1);
    gpio_pin_set_dt(&led_b, 1);

    // Reset state
    atomic_set(&led_state.current_color, COLOR_OFF);
    atomic_set(&led_state.current_priority, PRIO_IDLE);
    
    return ret;
}

/* Update LED State Based on Priority */
static void update_led_state(void) {
    uint8_t new_color = COLOR_OFF;
    enum led_priority new_priority = PRIO_IDLE;

    LOG_DBG("Current LED Priority: %d", atomic_get(&led_state.current_priority));
    LOG_DBG("Current LED Color: %d", atomic_get(&led_state.current_color));
    LOG_DBG("Updating LED state - Battery:%d%% Caps:%d BLE:%d/%d", 
            led_state.battery_level, led_state.caps_lock,
            led_state.ble_connected, led_state.ble_open);

    // Priority 1: Critical Battery
    if (led_state.battery_level <= 10) {
        LOG_DBG("Critical battery level detected");
        new_color = COLOR_RED;
        new_priority = PRIO_CRITICAL;
    }
    // Priority 2: Caps Lock
    else if (led_state.caps_lock) {
        LOG_DBG("Caps Lock active");
        new_color = COLOR_WHITE;
        new_priority = PRIO_CAPS_LOCK;
    }
    // Priority 3: Normal Battery
    else if (led_state.battery_level <= 40) {
        LOG_DBG("Normal battery level: %d%%", led_state.battery_level);
        new_color = (led_state.battery_level <= 25) ? COLOR_YELLOW : COLOR_GREEN;
        new_priority = PRIO_BATTERY;
    }
    // Priority 4: BLE Status
    else if (led_state.ble_connected) {
        LOG_DBG("BLE connected");
        new_color = COLOR_BLUE;
        new_priority = PRIO_BLE;
    } else if (led_state.ble_open) {
        LOG_DBG("BLE open");
        new_color = COLOR_YELLOW;
        new_priority = PRIO_BLE;
    } else {
        LOG_DBG("No active state, turning LED off");
    }

    // Apply if higher or equal priority
    if (new_priority >= atomic_get(&led_state.current_priority)) {
        LOG_DBG("New LED state - Color:%d Priority:%d", new_color, new_priority);
        atomic_set(&led_state.current_color, new_color);
        atomic_set(&led_state.current_priority, new_priority);
        
        if (new_priority == PRIO_CRITICAL) {
            LOG_DBG("Starting blink timer for critical battery");
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
    LOG_DBG("Caps Lock event detected");
    LOG_DBG("Previous Caps Lock state: %d", led_state.caps_lock);
    LOG_DBG("New Caps Lock state: %d", new_state);
    
    if (new_state != led_state.caps_lock) {
        LOG_DBG("Caps Lock state changed from %d to %d", led_state.caps_lock, new_state);
        led_state.caps_lock = new_state;
        update_led_state();
    } else {
        LOG_DBG("Caps Lock state unchanged");
    }
    return ZMK_EV_EVENT_BUBBLE;
}

static int on_battery(const zmk_event_t *eh) {
    const struct zmk_battery_state_changed *ev = as_zmk_battery_state_changed(eh);
    if (ev) {
        LOG_DBG("Battery event - New level: %d%%", ev->state_of_charge);
    }
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
    
    LOG_DBG("BLE status change event detected");
    LOG_DBG("Previous BLE states - Connected:%d Open:%d", led_state.ble_connected, led_state.ble_open);
    LOG_DBG("New BLE states - Connected:%d Open:%d", new_connected, new_open);
    
    if (new_connected != led_state.ble_connected || new_open != led_state.ble_open) {
        LOG_DBG("BLE state changed:");
        if (new_connected != led_state.ble_connected) {
            LOG_DBG("- Connection changed from %d to %d", led_state.ble_connected, new_connected);
        }
        if (new_open != led_state.ble_open) {
            LOG_DBG("- Open state changed from %d to %d", led_state.ble_open, new_open);
        }
        
        led_state.ble_connected = new_connected;
        led_state.ble_open = new_open;
        update_led_state();
    } else {
        LOG_DBG("BLE state unchanged");
    }
    return ZMK_EV_EVENT_BUBBLE;
}

/* Main Initialization */
static int led_init(const struct device *dev) {
    LOG_DBG("Initializing LED controller");
    ARG_UNUSED(dev);
    
    if (init_leds() != 0) return -EIO;

    // Initialize timers
    k_timer_init(&led_state.blink_timer, blink_handler, NULL);
    k_timer_init(&led_state.rainbow_timer, rainbow_handler, NULL);
    k_timer_init(&led_state.debounce_timer, on_debounce, NULL);

    // Clear any existing timer state
    k_timer_stop(&led_state.blink_timer);
    k_timer_stop(&led_state.rainbow_timer);
    k_timer_stop(&led_state.debounce_timer);

    // Display rainbow effect once
    LOG_DBG("Starting one-time rainbow boot effect");
    for (int i = 0; i < ARRAY_SIZE(rainbow_colors); i++) {
        set_led_color(rainbow_colors[i]);
        k_msleep(RAINBOW_INTERVAL_MS);
    }
    set_led_color(COLOR_OFF);

    // Get initial states
    led_state.battery_level = zmk_battery_state_of_charge();
    led_state.ble_connected = zmk_ble_active_profile_is_connected();
    led_state.ble_open = zmk_ble_active_profile_is_open();
    led_state.caps_lock = zmk_hid_indicators_get_current_profile() & (1 << (HID_USAGE_LED_CAPS_LOCK - 1));

    LOG_DBG("Initial state - Battery:%d%% BLE:%d/%d Caps:%d",
            led_state.battery_level, led_state.ble_connected,
            led_state.ble_open, led_state.caps_lock);

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
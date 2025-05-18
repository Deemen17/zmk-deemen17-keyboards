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

/* LED Color Definitions (Common Anode - 0=ON, 1=OFF) */
enum led_color {
    COLOR_OFF   = 0b111,  // R=1 G=1 B=1 -> Tất cả OFF
    COLOR_RED   = 0b011,  // R=0 G=1 B=1 -> Chỉ RED ON
    COLOR_GREEN = 0b101,  // R=1 G=0 B=1 -> Chỉ GREEN ON
    COLOR_BLUE  = 0b110,  // R=1 G=1 B=0 -> Chỉ BLUE ON
    COLOR_YELLOW= 0b001,  // R=0 G=0 B=1 -> RED+GREEN ON = YELLOW
    COLOR_CYAN  = 0b100,  // R=1 G=0 B=0 -> GREEN+BLUE ON = CYAN  
    COLOR_PURPLE= 0b010,  // R=0 G=1 B=0 -> RED+BLUE ON = PURPLE
    COLOR_WHITE = 0b000   // R=0 G=0 B=0 -> Tất cả ON = WHITE
};

/* System Priorities */
enum led_priority {
    PRIO_IDLE,
    PRIO_BLE,      // Bluetooth status
    PRIO_BATTERY,  // Battery status
    PRIO_CAPS      // Caps Lock (highest)
};

/* Timing Constants */
#define BLINK_INTERVAL_MS  300
#define RAINBOW_INTERVAL_MS 400
#define DEBOUNCE_MS        500

/* Config Options */
#define LED_BLINK_ON_LOW_BATTERY 0  // 0/1 for enable/disable blink on low battery
#define BATTERY_LEVEL_UNKNOWN 0    // 0/1 for enable/disable battery 
#define BATTERY_CRITICAL_THRESHOLD 10
#define BATTERY_LOW_THRESHOLD 50
#define BLE_BLINK_INTERVAL 1000

/* Function Declarations */
static void set_led_color(uint8_t color);

/* Global State */
static struct {
    atomic_t current_color;
    atomic_t current_priority;
    struct k_timer blink_timer;
    struct k_timer debounce_timer;
    uint8_t battery_level;
    bool caps_lock;
    bool ble_connected;
    bool ble_open;
    bool low_battery_blink_enabled; 
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
    
     
    
    LOG_DBG("LED Color Change:");
    LOG_DBG("- Color: %s (0b%d%d%d)", color_name, red, green, blue);
    LOG_DBG("- Setting pins (Common Anode - Active LOW):");
    
    gpio_pin_set_dt(&led_r, red);
    gpio_pin_set_dt(&led_g, green);
    gpio_pin_set_dt(&led_b, blue);
    
    LOG_DBG("  RED:   GPIO=%d (%s)", red, red ? "OFF" : "ON");
    LOG_DBG("  GREEN: GPIO=%d (%s)", green, green ? "OFF" : "ON");
    LOG_DBG("  BLUE:  GPIO=%d (%s)", blue, blue ? "OFF" : "ON");

    // Verify GPIO states after setting
    LOG_DBG("LED GPIO state after set:");
    LOG_DBG("R pin %d = %d", led_r.pin, gpio_pin_get_dt(&led_r));
    LOG_DBG("G pin %d = %d", led_g.pin, gpio_pin_get_dt(&led_g));
    LOG_DBG("B pin %d = %d", led_b.pin, gpio_pin_get_dt(&led_b));
}

static void blink_handler(struct k_timer *timer) {
    static bool led_on = false;
    uint8_t current_color = atomic_get(&led_state.current_color);
    enum led_priority current_priority = atomic_get(&led_state.current_priority);
    
    led_on = !led_on;
    LOG_DBG("BLINK - State:%s Color:%d Priority:%d", 
            led_on ? "ON" : "OFF", current_color, current_priority);
    set_led_color(led_on ? current_color : COLOR_OFF);
}

static const uint8_t rainbow_colors[] = {
    COLOR_RED,    // Bắt đầu với đỏ
    COLOR_GREEN,  // Xanh lá
    COLOR_BLUE,   // Xanh dương
    COLOR_WHITE   // Kết thúc với trắng
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

    // Configure as outputs with initial state HIGH (LED OFF)
    ret |= gpio_pin_configure_dt(&led_r, GPIO_OUTPUT | GPIO_OUTPUT_INIT_HIGH);
    ret |= gpio_pin_configure_dt(&led_g, GPIO_OUTPUT | GPIO_OUTPUT_INIT_HIGH);
    ret |= gpio_pin_configure_dt(&led_b, GPIO_OUTPUT | GPIO_OUTPUT_INIT_HIGH);

    LOG_DBG("Initialized all LEDs to OFF state");

    LOG_DBG("LED GPIO config:");
    LOG_DBG("RED pin: %d", led_r.pin);
    LOG_DBG("GREEN pin: %d", led_g.pin);
    LOG_DBG("BLUE pin: %d", led_b.pin);
    
    // Reset state variables
    atomic_set(&led_state.current_color, COLOR_OFF);
    atomic_set(&led_state.current_priority, PRIO_IDLE);

    // Initialize feature flags
    led_state.low_battery_blink_enabled = LED_BLINK_ON_LOW_BATTERY;
    
    return ret;
}

/* Update LED State Based on Priority */
static void update_led_state(void) {
    uint8_t old_color = atomic_get(&led_state.current_color);
    enum led_priority old_priority = atomic_get(&led_state.current_priority);
    
    uint8_t new_color = COLOR_OFF;
    enum led_priority new_priority = PRIO_IDLE;

    // Priority 1: Caps Lock (Highest)
    if (led_state.caps_lock) {
        new_color = COLOR_WHITE;
        new_priority = PRIO_CAPS;
        k_timer_stop(&led_state.blink_timer);
    }
    // Priority 2: Critical Battery (<10%) overrides everything except Caps
    else if (led_state.battery_level < BATTERY_CRITICAL_THRESHOLD) {
        new_color = COLOR_RED;
        new_priority = PRIO_BATTERY;
        k_timer_stop(&led_state.blink_timer);
    }
    // Priority 3: Normal Battery Status
    else if (led_state.battery_level >= BATTERY_CRITICAL_THRESHOLD) {
        if (led_state.battery_level >= BATTERY_LOW_THRESHOLD) {
            new_color = COLOR_GREEN;
        } else {
            new_color = COLOR_YELLOW;
        }
        new_priority = PRIO_BATTERY;
        k_timer_stop(&led_state.blink_timer);
    }
    // Priority 4: Bluetooth Status
    else {
        new_priority = PRIO_BLE;
        if (led_state.ble_connected) {
            new_color = COLOR_BLUE;
        } else {
            new_color = COLOR_CYAN; // Open or not connected
        }
        k_timer_start(&led_state.blink_timer, K_MSEC(BLE_BLINK_INTERVAL), K_MSEC(BLE_BLINK_INTERVAL));
    }

    // Only update if new priority is higher/equal
    if (new_priority >= old_priority) {
        atomic_set(&led_state.current_color, new_color);
        atomic_set(&led_state.current_priority, new_priority);
        set_led_color(new_color);
    }
}

/* Event Handlers */
static int on_caps_lock(const zmk_event_t *eh) {
    bool new_state = zmk_hid_indicators_get_current_profile() & (1 << (HID_USAGE_LED_CAPS_LOCK - 1));
    LOG_DBG("============ CAPS LOCK EVENT ============");
    LOG_DBG("Previous state: %d", led_state.caps_lock);
    LOG_DBG("New state: %d", new_state);
    LOG_DBG("Current LED color: %d", atomic_get(&led_state.current_color));
    
    if (new_state != led_state.caps_lock) {
        LOG_DBG("State changed: %d -> %d", led_state.caps_lock, new_state);
        led_state.caps_lock = new_state;
        if (new_state) {
            LOG_DBG("CAPS LOCK ON -> Setting WHITE");
        } else {
            LOG_DBG("CAPS LOCK OFF -> Restoring previous state");
        }
        update_led_state();
    }
    LOG_DBG("========================================");
    return ZMK_EV_EVENT_BUBBLE;
}

static int on_battery(const zmk_event_t *eh) {
    const struct zmk_battery_state_changed *ev = as_zmk_battery_state_changed(eh);
    if (ev) {
        LOG_DBG("============ BATTERY EVENT ============");
        LOG_DBG("Previous state: %s", 
                (led_state.battery_level == BATTERY_LEVEL_UNKNOWN) ? "No battery" : "Battery present");
        LOG_DBG("New level: %d%%", ev->state_of_charge);
        
        // Cập nhật trạng thái pin
        if (ev->state_of_charge > 0) {
            led_state.battery_level = ev->state_of_charge;
        } else {
            led_state.battery_level = BATTERY_LEVEL_UNKNOWN;
        }
        
        k_timer_start(&led_state.debounce_timer, K_MSEC(DEBOUNCE_MS), K_NO_WAIT);
        LOG_DBG("======================================");
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
    
    // Initialize hardware first
    if (init_leds() != 0) return -EIO;

    // Initialize timers
    k_timer_init(&led_state.blink_timer, blink_handler, NULL);
    k_timer_init(&led_state.debounce_timer, on_debounce, NULL);

    // Initialize states
    led_state.battery_level = zmk_battery_state_of_charge();
    led_state.ble_connected = zmk_ble_active_profile_is_connected();
    led_state.ble_open = zmk_ble_active_profile_is_open();
    led_state.caps_lock = false;

    // Start with LED OFF
    set_led_color(COLOR_OFF);
    k_msleep(100);

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
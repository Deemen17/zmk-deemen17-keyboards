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

// Định nghĩa GPIO cho LED RGB
#define LED_NODE_R DT_ALIAS(ledred)
#define LED_NODE_G DT_ALIAS(ledgreen)
#define LED_NODE_B DT_ALIAS(ledblue)

#if !DT_NODE_HAS_STATUS(LED_NODE_R, okay) || !DT_NODE_HAS_STATUS(LED_NODE_G, okay) || !DT_NODE_HAS_STATUS(LED_NODE_B, okay)
#error "Unsupported board: led devicetree alias is not defined"
#endif

static const struct gpio_dt_spec LED_R = GPIO_DT_SPEC_GET(LED_NODE_R, gpios);
static const struct gpio_dt_spec LED_G = GPIO_DT_SPEC_GET(LED_NODE_G, gpios);
static const struct gpio_dt_spec LED_B = GPIO_DT_SPEC_GET(LED_NODE_B, gpios);

// Trạng thái và ưu tiên
static bool caps_lock_active = false;
static bool caps_lock_priority = false;
static bool battery_status_active = false;
static bool connection_status_active = false;

// Mức pin và trạng thái kết nối
static uint8_t battery_level = 0;
static bool ble_connected = false;
static bool ble_open = false;

// Cấu hình ngưỡng pin
#define BATTERY_LEVEL_HIGH 50
#define BATTERY_LEVEL_CRITICAL 10
#define BLINK_INTERVAL_MS 250
#define BLINK_COUNT 3

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
    if (err) LOG_ERR("Failed to configure LED_R (P0.03): %d", err);
    err = gpio_pin_configure_dt(&LED_G, GPIO_OUTPUT_HIGH);
    if (err) LOG_ERR("Failed to configure LED_G (P1.10): %d", err);
    err = gpio_pin_configure_dt(&LED_B, GPIO_OUTPUT_HIGH);
    if (err) LOG_ERR("Failed to configure LED_B (P1.11): %d", err);

    return err;
}

// Turn off LEDs (set GPIOs to HIGH for Common Anode)
void reset_leds() {
    int err;
    err = gpio_pin_set_dt(&LED_R, 1); // HIGH = OFF
    if (err) LOG_ERR("Failed to set LED_R (P0.03) HIGH: %d", err);
    err = gpio_pin_set_dt(&LED_G, 1);
    if (err) LOG_ERR("Failed to set LED_G (P1.10) HIGH: %d", err);
    err = gpio_pin_set_dt(&LED_B, 1);
    if (err) LOG_ERR("Failed to set LED_B (P1.11) HIGH: %d", err);
}

// Set LED state (Common Anode: LOW = ON, HIGH = OFF)
void set_led_rgb(bool r, bool g, bool b) {
    int err;
    err = gpio_pin_set_dt(&LED_R, r ? 0 : 1); // LOW = ON, HIGH = OFF
    if (err) LOG_ERR("Failed to set LED_R (P0.03): %d", err);
    err = gpio_pin_set_dt(&LED_G, g ? 0 : 1);
    if (err) LOG_ERR("Failed to set LED_G (P1.10): %d", err);
    err = gpio_pin_set_dt(&LED_B, b ? 0 : 1);
    if (err) LOG_ERR("Failed to set LED_B (P1.11): %d", err);
}

// Nhấp nháy LED với màu chỉ định
void blink_led(bool r, bool g, bool b, int count) {
    for (int i = 0; i < count; i++) {
        set_led_rgb(r, g, b);
        k_msleep(BLINK_INTERVAL_MS);
        reset_leds();
        k_msleep(BLINK_INTERVAL_MS);
    }
}

// Hiển thị trạng thái pin
void show_battery_status() {
    if (caps_lock_priority) return; // Ưu tiên Caps Lock

    battery_status_active = true;
    if (battery_level > BATTERY_LEVEL_HIGH) {
        LOG_DBG("Battery high (>50%%), blinking green");
        blink_led(false, true, false, BLINK_COUNT); // Xanh lá
    } else if (battery_level > BATTERY_LEVEL_CRITICAL) {
        LOG_DBG("Battery medium, blinking yellow");
        blink_led(true, true, false, BLINK_COUNT); // Vàng
    } else {
        LOG_DBG("Battery critical (<10%%), blinking red");
        blink_led(true, false, false, BLINK_COUNT); // Đỏ
    }
    battery_status_active = false;
}

// Hiển thị trạng thái kết nối Bluetooth
void show_connection_status() {
    if (caps_lock_priority) return; // Ưu tiên Caps Lock

    connection_status_active = true;
    if (ble_connected) {
        LOG_DBG("BLE connected, blinking blue");
        blink_led(false, false, true, BLINK_COUNT); // Xanh dương
    } else if (ble_open) {
        LOG_DBG("BLE open (advertising), blinking yellow");
        blink_led(true, true, false, BLINK_COUNT); // Vàng
    } else {
        LOG_DBG("BLE disconnected, blinking red");
        blink_led(true, false, false, BLINK_COUNT); // Đỏ
    }
    connection_status_active = false;
}

// Listener cho Caps Lock
int led_caps_lock_listener(const zmk_event_t *eh) {
    zmk_hid_indicators_t flags = zmk_hid_indicators_get_current_profile();
    unsigned int capsBit = 1 << (HID_USAGE_LED_CAPS_LOCK - 1);
    bool new_caps_state = (flags & capsBit) != 0;
    LOG_DBG("Caps Lock event received, new state: %d", new_caps_state);
    if (new_caps_state != caps_lock_active) {
        caps_lock_active = new_caps_state;
        caps_lock_priority = caps_lock_active;
        if (caps_lock_active) {
            LOG_DBG("Caps Lock active, setting LED to White");
            set_led_rgb(true, true, true); // Bật LED trắng
        } else {
            LOG_DBG("Caps Lock inactive, turning off LEDs");
            reset_leds();
            // Hiển thị trạng thái pin hoặc kết nối nếu cần
            if (battery_status_active) {
                show_battery_status();
            } else if (connection_status_active) {
                show_connection_status();
            }
        }
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(led_caps_lock_listener, led_caps_lock_listener);
ZMK_SUBSCRIPTION(led_caps_lock_listener, zmk_hid_indicators_changed);

// Listener cho Battery Status
int battery_status_listener(const zmk_event_t *eh) {
    const struct zmk_battery_state_changed *ev = as_zmk_battery_state_changed(eh);
    if (ev) {
        battery_level = ev->state_of_charge;
        LOG_DBG("Battery level changed: %d%%", battery_level);
        show_battery_status();
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(battery_status_listener, battery_status_listener);
ZMK_SUBSCRIPTION(battery_status_listener, zmk_battery_state_changed);

// Listener cho Connection Status
int connection_status_listener(const zmk_event_t *eh) {
    ble_connected = zmk_ble_active_profile_is_connected();
    ble_open = zmk_ble_active_profile_is_open();
    LOG_DBG("BLE state changed - Connected: %d, Open: %d", ble_connected, ble_open);
    show_connection_status();
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(connection_status_listener, connection_status_listener);
ZMK_SUBSCRIPTION(connection_status_listener, zmk_ble_active_profile_changed);

// Khởi động
static int led_init(const struct device *device) {
    LOG_DBG("Initializing LED GPIOs");
    int err = init_led_gpios();
    if (err) {
        LOG_ERR("Failed to initialize LED GPIOs: %d", err);
        return err;
    }

    // Lấy trạng thái ban đầu
    battery_level = zmk_battery_state_of_charge();
    ble_connected = zmk_ble_active_profile_is_connected();
    ble_open = zmk_ble_active_profile_is_open();

    // Hiển thị trạng thái ban đầu
    LOG_DBG("Initial battery level: %d%%", battery_level);
    show_battery_status();
    show_connection_status();

    return 0;
}

SYS_INIT(led_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
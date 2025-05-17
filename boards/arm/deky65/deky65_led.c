#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/ble.h>
#include <zmk/event_manager.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/hid_indicators.h>
#include <zmk/events/hid_indicators_changed.h>

#define LED_NODE_R DT_ALIAS(ledred)
#define LED_NODE_G DT_ALIAS(ledgreen)
#define LED_NODE_B DT_ALIAS(ledblue)

#define LED_TIMEOUT_S 3 // Auto-off timeout (3 seconds)

#if !DT_NODE_HAS_STATUS(LED_NODE_R, okay) || !DT_NODE_HAS_STATUS(LED_NODE_G, okay) || !DT_NODE_HAS_STATUS(LED_NODE_B, okay)
#error "Unsupported board: led devicetree alias is not defined"
#endif

#define RED 0
#define GREEN 1
#define BLUE 2

static const struct gpio_dt_spec LED_R = GPIO_DT_SPEC_GET(LED_NODE_R, gpios);
static const struct gpio_dt_spec LED_G = GPIO_DT_SPEC_GET(LED_NODE_G, gpios);
static const struct gpio_dt_spec LED_B = GPIO_DT_SPEC_GET(LED_NODE_B, gpios);

// Biến toàn cục để lưu trạng thái profile Bluetooth hiện tại
static int current_profile_index = -1;
// Biến toàn cục để lưu trạng thái Caps Lock
static bool caps_lock_active = false;

void reset_leds() {
    int err;
    err = gpio_pin_configure_dt(&LED_R, GPIO_DISCONNECTED);
    if (err) LOG_ERR("Failed to configure LED_R: %d", err);
    err = gpio_pin_configure_dt(&LED_G, GPIO_DISCONNECTED);
    if (err) LOG_ERR("Failed to configure LED_G: %d", err);
    err = gpio_pin_configure_dt(&LED_B, GPIO_DISCONNECTED);
    if (err) LOG_ERR("Failed to configure LED_B: %d", err);
}

void set_led_rgb(bool r, bool g, bool b) {
    int err;
    reset_leds();
    if (r) {
        err = gpio_pin_configure_dt(&LED_R, GPIO_OUTPUT_LOW);
        if (err) LOG_ERR("Failed to set LED_R: %d", err);
    }
    if (g) {
        err = gpio_pin_configure_dt(&LED_G, GPIO_OUTPUT_LOW);
        if (err) LOG_ERR("Failed to set LED_G: %d", err);
    }
    if (b) {
        err = gpio_pin_configure_dt(&LED_B, GPIO_OUTPUT_LOW);
        if (err) LOG_ERR("Failed to set LED_B: %d", err);
    }
}

// Hàm cập nhật trạng thái LED dựa trên profile và Caps Lock
void update_led_state() {
    k_timer_stop(&led_timer);
    if (caps_lock_active) {
        LOG_DBG("Caps Lock active, setting LED to White");
        set_led_rgb(true, true, true); // Màu trắng
    } else {
        LOG_DBG("Caps Lock inactive, setting LED based on profile %d", current_profile_index);
        switch (current_profile_index) {
            case 0:
                set_led_rgb(true, false, false); // Red
                break;
            case 1:
                set_led_rgb(false, true, false); // Green
                break;
            case 2:
                set_led_rgb(false, false, true); // Blue
                break;
            case 3:
                set_led_rgb(true, true, false); // Yellow
                break;
            case 4:
                set_led_rgb(true, false, true); // Magenta
                break;
            default:
                reset_leds();
                break;
        }
    }
    k_timer_start(&led_timer, K_SECONDS(LED_TIMEOUT_S), K_NO_WAIT);
}

void led_work_handler(struct k_work *work) {
    reset_leds();
}

K_WORK_DEFINE(led_work, led_work_handler);

void led_expiry_function() {
    k_work_submit(&led_work);
}

K_TIMER_DEFINE(led_timer, led_expiry_function, NULL);

// Listener cho sự kiện thay đổi profile Bluetooth
int led_listener(const zmk_event_t *eh) {
    const struct zmk_ble_active_profile_changed *profile_ev = as_zmk_ble_active_profile_changed(eh);
    if (!profile_ev) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    current_profile_index = profile_ev->index;
    LOG_DBG("BLE profile changed to index %d", current_profile_index);
    update_led_state();
    return ZMK_EV_EVENT_BUBBLE;
}

// Listener cho sự kiện thay đổi chỉ số HID (Caps Lock)
int led_caps_lock_listener(const zmk_event_t *eh) {
    zmk_hid_indicators_t flags = zmk_hid_indicators_get_current_profile();
    unsigned int capsBit = 1 << (HID_USAGE_LED_CAPS_LOCK - 1);
    bool new_caps_state = (flags & capsBit) != 0;
    if (new_caps_state != caps_lock_active) {
        caps_lock_active = new_caps_state;
        update_led_state();
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(led_output_status, led_listener);
ZMK_LISTENER(led_caps_lock_listener, led_caps_lock_listener);

#if defined(CONFIG_ZMK_BLE)
    ZMK_SUBSCRIPTION(led_output_status, zmk_ble_active_profile_changed);
#endif
ZMK_SUBSCRIPTION(led_caps_lock_listener, zmk_hid_indicators_changed);
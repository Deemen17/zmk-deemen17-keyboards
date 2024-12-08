#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/led.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/bluetooth/services/bas.h>

#include <zmk/usb.h>
#include <zmk/ble.h>
#include <zmk/endpoints.h>
#include <zmk/hid_indicators.h>

#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/events/hid_indicators_changed.h>

#include <zmk/events/battery_state_changed.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/events/split_peripheral_status_changed.h>

#include <zmk/keymap.h>
#include <zmk/split/bluetooth/peripheral.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define LED_GPIO_NODE_ID DT_COMPAT_GET_ANY_STATUS_OKAY(gpio_leds)

// GPIO-based LED device
static const struct device *led_dev = DEVICE_DT_GET(LED_GPIO_NODE_ID);

// 
struct output_status_state {
    struct zmk_endpoint_instance selected_endpoint;
    bool active_profile_connected;
    bool active_profile_bonded;
};

// 
static struct output_status_state get_state(const zmk_event_t *_eh) {
    return (struct output_status_state){
        .selected_endpoint = zmk_endpoints_selected(),
        .active_profile_connected = zmk_ble_active_profile_is_connected(),
        .active_profile_bonded = !zmk_ble_active_profile_is_open(),
    };
}

// Timer và trạng thái cho LED BLE quảng bá
static struct k_timer ble_adv_timer;
static bool ble_adv_led_on = false;
static int ble_adv_led_index = -1;

// Hàm callback của timer để nháy LED
static void ble_adv_timer_handler(struct k_timer *timer_id) {
    if (ble_adv_led_index >= 0) {
        if (ble_adv_led_on) {
            led_off(led_dev, ble_adv_led_index);
        } else {
            led_on(led_dev, ble_adv_led_index);
        }
        ble_adv_led_on = !ble_adv_led_on; // Đảo trạng thái LED
    }
}



// Caps Lock Indicator
static int led_keylock_listener_cb(const zmk_event_t *eh) {
    zmk_hid_indicators_t flags = zmk_hid_indicators_get_current_profile();
    unsigned int capsBit = 1 << (HID_USAGE_LED_CAPS_LOCK - 1);

    if (flags & capsBit) {
        led_on(led_dev, DT_NODE_CHILD_IDX(DT_ALIAS(led_caps)));
    } else {
        led_off(led_dev, DT_NODE_CHILD_IDX(DT_ALIAS(led_caps)));
    }

    return 0;
}

// Output Selection Indicators

static void set_status_led(struct output_status_state state) {
    // Tắt tất cả các LED trước khi bật LED tương ứng
    led_off(led_dev, DT_NODE_CHILD_IDX(DT_ALIAS(led_usb)));
    led_off(led_dev, DT_NODE_CHILD_IDX(DT_ALIAS(led_ble_0)));
    led_off(led_dev, DT_NODE_CHILD_IDX(DT_ALIAS(led_ble_1)));
    led_off(led_dev, DT_NODE_CHILD_IDX(DT_ALIAS(led_ble_2)));

    switch (state.selected_endpoint.transport) {
    case ZMK_TRANSPORT_USB:
        // Bật LED USB
        led_on(led_dev, DT_NODE_CHILD_IDX(DT_ALIAS(led_usb)));
        break;
    case ZMK_TRANSPORT_BLE:
        if (state.active_profile_bonded) {
            if (state.active_profile_connected) {
                // Bật LED BLE đã kết nối
                switch (state.selected_endpoint.ble.profile_index) {
                case 0:
                    led_on(led_dev, DT_NODE_CHILD_IDX(DT_ALIAS(led_ble_0)));
                    break;
                case 1:
                    led_on(led_dev, DT_NODE_CHILD_IDX(DT_ALIAS(led_ble_1)));
                    break;
                case 2:
                    led_on(led_dev, DT_NODE_CHILD_IDX(DT_ALIAS(led_ble_2)));
                    break;
                }
            } else {
                // BLE chưa kết nối - có thể để trống hoặc thêm logic
            }
        } else {
            switch (state.selected_endpoint.ble.profile_index) {
            case 0:
                ble_adv_led_index = DT_NODE_CHILD_IDX(DT_ALIAS(led_ble_0));
                break;
            case 1:
                ble_adv_led_index = DT_NODE_CHILD_IDX(DT_ALIAS(led_ble_1));
                break;
            case 2:
                ble_adv_led_index = DT_NODE_CHILD_IDX(DT_ALIAS(led_ble_2));
                break;
            }

            // Khởi động timer để nháy LED
            if (ble_adv_led_index >= 0) {
                k_timer_start(&ble_adv_timer, K_MSEC(500), K_MSEC(500)); // 500ms nháy
            }
        }
        break;
    }
}

static int output_status_update_cb(const zmk_event_t *_eh) {
    struct output_status_state state = get_state(_eh);
    set_status_led(state);

    return 0;
}

ZMK_LISTENER(led_indicators_listener, led_keylock_listener_cb);
ZMK_SUBSCRIPTION(led_indicators_listener, zmk_hid_indicators_changed);


ZMK_LISTENER(output_status, output_status_update_cb);
#if defined(CONFIG_ZMK_BLE)
ZMK_SUBSCRIPTION(output_status, zmk_ble_active_profile_changed);
#endif
ZMK_SUBSCRIPTION(output_status, zmk_endpoint_changed);

static int leds_init(const struct device *device) {
    if (!device_is_ready(led_dev)) {
        return -ENODEV;
    }

    return 0;
}

int output_status_init(void) {
    // Khởi tạo timer nháy LED
    k_timer_init(&ble_adv_timer, ble_adv_timer_handler, NULL);
    return 0;
}

// Run leds_init on boot
SYS_INIT(leds_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
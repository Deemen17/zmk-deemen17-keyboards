#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/hid_indicators.h>
#include <zmk/events/hid_indicators_changed.h>

#define LED_NODE_R DT_ALIAS(ledred)
#define LED_NODE_G DT_ALIAS(ledgreen)
#define LED_NODE_B DT_ALIAS(ledblue)

#if !DT_NODE_HAS_STATUS(LED_NODE_R, okay) || !DT_NODE_HAS_STATUS(LED_NODE_G, okay) || !DT_NODE_HAS_STATUS(LED_NODE_B, okay)
#error "Unsupported board: led devicetree alias is not defined"
#endif

static const struct gpio_dt_spec LED_R = GPIO_DT_SPEC_GET(LED_NODE_R, gpios);
static const struct gpio_dt_spec LED_G = GPIO_DT_SPEC_GET(LED_NODE_G, gpios);
static const struct gpio_dt_spec LED_B = GPIO_DT_SPEC_GET(LED_NODE_B, gpios);

static bool caps_lock_active = false;

/* LED Color Test Definitions */
#define TEST_DELAY_MS 500

// Set specific colors (Common Anode: LOW = ON)
static void set_color_off(void)   { set_led_rgb(false, false, false); }  // All OFF
static void set_color_red(void)   { set_led_rgb(true, false, false); }   // Only R
static void set_color_green(void) { set_led_rgb(false, true, false); }   // Only G
static void set_color_blue(void)  { set_led_rgb(false, false, true); }   // Only B
static void set_color_yellow(void){ set_led_rgb(true, true, false); }    // R+G
static void set_color_cyan(void)  { set_led_rgb(false, true, true); }    // G+B
static void set_color_purple(void){ set_led_rgb(true, false, true); }    // R+B
static void set_color_white(void) { set_led_rgb(true, true, true); }     // All ON

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

int led_caps_lock_listener(const zmk_event_t *eh) {
 zmk_hid_indicators_t flags = zmk_hid_indicators_get_current_profile();
 unsigned int capsBit = 1 << (HID_USAGE_LED_CAPS_LOCK - 1);
 bool new_caps_state = (flags & capsBit) != 0;
 LOG_DBG("Caps Lock event received, new state: %d", new_caps_state);
 if (new_caps_state != caps_lock_active) {
 caps_lock_active = new_caps_state;
 if (!caps_lock_active) { // Đảo ngược logic: Caps Lock tắt → LED sáng
 LOG_DBG("Caps Lock inactive, setting LED to White");
 set_led_rgb(true, true, true);
 } else { // Caps Lock bật → LED tắt
 LOG_DBG("Caps Lock active, turning off LEDs");
 reset_leds();
 }
 }
 return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(led_caps_lock_listener, led_caps_lock_listener);
ZMK_SUBSCRIPTION(led_caps_lock_listener, zmk_hid_indicators_changed);

// Test LED on boot
static void run_led_test_sequence(void) {
    LOG_DBG("Starting LED test sequence");
    
    LOG_DBG("Testing RED");
    set_color_red();
    k_msleep(TEST_DELAY_MS);
    
    LOG_DBG("Testing GREEN");
    set_color_green();
    k_msleep(TEST_DELAY_MS);
    
    LOG_DBG("Testing BLUE");
    set_color_blue();
    k_msleep(TEST_DELAY_MS);
    
    LOG_DBG("Testing YELLOW");
    set_color_yellow();
    k_msleep(TEST_DELAY_MS);
    
    LOG_DBG("Testing CYAN");
    set_color_cyan();
    k_msleep(TEST_DELAY_MS);
    
    LOG_DBG("Testing PURPLE");
    set_color_purple();
    k_msleep(TEST_DELAY_MS);
    
    LOG_DBG("Testing WHITE");
    set_color_white();
    k_msleep(TEST_DELAY_MS);
    
    LOG_DBG("Test complete - Setting to OFF");
    set_color_off();
}

static int led_test_init(const struct device *device) {
    LOG_DBG("Initializing LED test");
    
    int err = init_led_gpios();
    if (err) {
        LOG_ERR("Failed to initialize LED GPIOs: %d", err);
        return err;
    }

    // Run test sequence on boot
    run_led_test_sequence();
    return 0;
}

SYS_INIT(led_test_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/event_manager.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/ble.h>
#include <zmk/endpoints.h>

#define BUZZER_NODE DT_ALIAS(buzzer)

#if DT_NODE_HAS_STATUS(BUZZER_NODE, okay)

// Optimized buzzer configuration
#define BUZZER_THREAD_STACK_SIZE 1024
#define BUZZER_THREAD_PRIORITY K_PRIO_COOP(8)
#define BLE_MONITOR_INTERVAL_MS 3000
#define MAX_BLE_PROFILES 5

// Musical note periods in nanoseconds (optimized for memory)
typedef struct {
    uint32_t period_ns;
    uint8_t duration_ms;
} buzzer_note_t;

// Pre-computed note frequencies for better performance
enum musical_notes {
    NOTE_C5 = 1908396, // 523.25Hz
    NOTE_D5 = 1700680, // 587.33Hz
    NOTE_E5 = 1515152, // 659.25Hz
    NOTE_F5 = 1431818, // 698.46Hz
    NOTE_G5 = 1275510, // 783.99Hz
    NOTE_A5 = 1136364, // 880.00Hz
    NOTE_B5 = 1013776, // 987.77Hz
    NOTE_C6 = 954198,  // 1046.50Hz
    NOTE_FS5 = 851064, // 739.99Hz (F#5)
    NOTE_GS5 = 758374, // 830.61Hz (G#5)
    NOTE_SILENT = 0    // Silence
};

// Buzzer state management with anti-spam protection
typedef struct {
    bool is_playing;
    bool hw_ready;
    uint8_t current_profile;
    bool connection_states[MAX_BLE_PROFILES];
    struct k_work_q work_queue;
    K_THREAD_STACK_MEMBER(work_stack, BUZZER_THREAD_STACK_SIZE);

    // Anti-spam protection
    uint32_t last_profile_change;  // Timestamp of last profile change
    uint32_t last_endpoint_change; // Timestamp of last endpoint change
    uint32_t last_sound_played;    // Timestamp of last sound played
    uint8_t spam_counter;          // Count rapid events
    bool spam_mode;                // In spam protection mode
} buzzer_state_t;

static buzzer_state_t buzzer_state = {0};

// Anti-spam configuration
#define MIN_PROFILE_INTERVAL_MS 300  // Minimum 300ms between profile sounds
#define MIN_ENDPOINT_INTERVAL_MS 500 // Minimum 500ms between endpoint sounds
#define MIN_SOUND_INTERVAL_MS 100    // Minimum 100ms between any sounds
#define SPAM_THRESHOLD 5             // 5 rapid events = spam detected
#define SPAM_COOLDOWN_MS 2000        // 2 second cooldown in spam mode

static const struct pwm_dt_spec pwm = PWM_DT_SPEC_GET(BUZZER_NODE);

// Anti-spam protection functions
static bool is_spam_detected(uint32_t now, uint32_t last_event, uint32_t min_interval) {
    if (now - last_event < min_interval) {
        buzzer_state.spam_counter++;
        if (buzzer_state.spam_counter >= SPAM_THRESHOLD) {
            buzzer_state.spam_mode = true;
            LOG_WRN("Spam detected! Activating protection mode");
            return true;
        }
    } else {
        buzzer_state.spam_counter = 0;
    }
    return false;
}

static bool should_block_sound(uint32_t now, uint32_t last_event, uint32_t min_interval) {
    // Block if in spam mode and cooldown not finished
    if (buzzer_state.spam_mode && (now - buzzer_state.last_sound_played < SPAM_COOLDOWN_MS)) {
        return true;
    }

    // Block if too frequent
    if (now - last_event < min_interval) {
        return true;
    }

    // Exit spam mode after cooldown
    if (buzzer_state.spam_mode && (now - buzzer_state.last_sound_played >= SPAM_COOLDOWN_MS)) {
        buzzer_state.spam_mode = false;
        buzzer_state.spam_counter = 0;
        LOG_INF("Exiting spam protection mode");
    }

    return false;
}

static void update_sound_timestamp(void) { buzzer_state.last_sound_played = k_uptime_get_32(); }

// Optimized tone playing with non-blocking approach
static inline void play_tone_async(uint32_t period_ns, uint8_t duration_ms) {
    if (!buzzer_state.hw_ready || buzzer_state.is_playing) {
        return;
    }

    buzzer_state.is_playing = true;
    pwm_set_dt(&pwm, period_ns, period_ns / 2U);
    k_msleep(duration_ms);
    pwm_set_dt(&pwm, 0, 0);
    buzzer_state.is_playing = false;
}

// Optimized sequence player with anti-spam protection
static void play_melody(const buzzer_note_t *melody, size_t note_count) {
    uint32_t now = k_uptime_get_32();

    if (!buzzer_state.hw_ready) {
        return;
    }

    // Check if sound should be blocked due to spam
    if (should_block_sound(now, buzzer_state.last_sound_played, MIN_SOUND_INTERVAL_MS)) {
        LOG_DBG("Sound blocked due to anti-spam protection");
        return;
    }

    // Play melody if not blocked
    for (size_t i = 0; i < note_count && buzzer_state.hw_ready; i++) {
        if (buzzer_state.spam_mode) {
            // In spam mode, play shorter/quieter sounds
            uint8_t reduced_duration = melody[i].duration_ms / 2;
            play_tone_async(melody[i].period_ns, reduced_duration);
        } else {
            play_tone_async(melody[i].period_ns, melody[i].duration_ms);
        }
        k_msleep(10); // Shorter gap in spam mode
    }

    update_sound_timestamp();
} // Optimized profile sounds using structured melodies
static const buzzer_note_t profile_melodies[][5] = {
    // Profile 1 - Single note
    {{NOTE_C5, 100}, {NOTE_SILENT, 0}},

    // Profile 2 - Two harmonious notes
    {{NOTE_C5, 80}, {NOTE_E5, 80}, {NOTE_SILENT, 0}},

    // Profile 3 - Three note chord
    {{NOTE_C5, 70}, {NOTE_E5, 70}, {NOTE_G5, 70}, {NOTE_SILENT, 0}},

    // Profile 4 - Four note arpeggio
    {{NOTE_C5, 60}, {NOTE_E5, 60}, {NOTE_G5, 60}, {NOTE_C6, 60}, {NOTE_SILENT, 0}},

    // Profile 5 - Five note scale
    {{NOTE_C5, 50}, {NOTE_D5, 50}, {NOTE_E5, 50}, {NOTE_F5, 50}, {NOTE_G5, 50}}};

static const buzzer_note_t startup_melody[] = {
    {NOTE_C5, 120}, {NOTE_E5, 120}, {NOTE_G5, 120}, {NOTE_C6, 150}};

static const buzzer_note_t usb_melody[] = {{NOTE_FS5, 60}, {NOTE_GS5, 60}};

static const buzzer_note_t ble_melody[] = {{NOTE_G5, 90}, {NOTE_A5, 90}, {NOTE_B5, 90}};

static const buzzer_note_t ble_connected_melody[] = {
    {NOTE_C5, 70}, {NOTE_E5, 70}, {NOTE_G5, 70}, {NOTE_A5, 70}};

// Work queue functions for non-blocking audio
static void profile_sound_work(struct k_work *work);
static void system_sound_work(struct k_work *work);
static void connection_sound_work(struct k_work *work);

K_WORK_DEFINE(profile_work, profile_sound_work);
K_WORK_DEFINE(system_work, system_sound_work);
K_WORK_DEFINE(connection_work, connection_sound_work);

static uint8_t pending_profile = 0;
static uint8_t pending_system_sound = 0; // 1=startup, 2=usb, 3=ble

// Work queue implementations for non-blocking audio
static void profile_sound_work(struct k_work *work) {
    if (pending_profile < 5) {
        size_t melody_len = 0;
        // Calculate melody length
        for (int i = 0; i < 5; i++) {
            if (profile_melodies[pending_profile][i].period_ns == NOTE_SILENT) {
                break;
            }
            melody_len++;
        }
        play_melody(profile_melodies[pending_profile], melody_len);
        LOG_INF("Profile %d sound played", pending_profile + 1);
    }
}

static void system_sound_work(struct k_work *work) {
    switch (pending_system_sound) {
    case 1: // Startup
        play_melody(startup_melody, ARRAY_SIZE(startup_melody));
        LOG_INF("Startup sound played");
        break;
    case 2: // USB
        play_melody(usb_melody, ARRAY_SIZE(usb_melody));
        LOG_INF("USB sound played");
        break;
    case 3: // BLE
        play_melody(ble_melody, ARRAY_SIZE(ble_melody));
        LOG_INF("BLE sound played");
        break;
    }
    pending_system_sound = 0;
}

static void connection_sound_work(struct k_work *work) {
    play_melody(ble_connected_melody, ARRAY_SIZE(ble_connected_melody));
    LOG_INF("BLE connected sound played");
}

// Anti-spam protected public interface functions
static inline void play_profile_sound(uint8_t profile_idx) {
    uint32_t now = k_uptime_get_32();

    if (profile_idx >= 5 || !buzzer_state.hw_ready) {
        return;
    }

    // Check for profile spam
    if (is_spam_detected(now, buzzer_state.last_profile_change, MIN_PROFILE_INTERVAL_MS)) {
        LOG_WRN("Profile change spam detected");
        return;
    }

    if (should_block_sound(now, buzzer_state.last_profile_change, MIN_PROFILE_INTERVAL_MS)) {
        LOG_DBG("Profile sound blocked - too frequent");
        return;
    }

    buzzer_state.last_profile_change = now;
    pending_profile = profile_idx;
    k_work_submit_to_queue(&buzzer_state.work_queue, &profile_work);
}

static inline void play_startup_sound(void) {
    if (buzzer_state.hw_ready) {
        pending_system_sound = 1;
        k_work_submit_to_queue(&buzzer_state.work_queue, &system_work);
    }
}

static inline void play_usb_sound(void) {
    uint32_t now = k_uptime_get_32();

    if (!buzzer_state.hw_ready) {
        return;
    }

    if (should_block_sound(now, buzzer_state.last_endpoint_change, MIN_ENDPOINT_INTERVAL_MS)) {
        LOG_DBG("USB sound blocked - too frequent");
        return;
    }

    buzzer_state.last_endpoint_change = now;
    pending_system_sound = 2;
    k_work_submit_to_queue(&buzzer_state.work_queue, &system_work);
}

static inline void play_ble_sound(void) {
    uint32_t now = k_uptime_get_32();

    if (!buzzer_state.hw_ready) {
        return;
    }

    if (should_block_sound(now, buzzer_state.last_endpoint_change, MIN_ENDPOINT_INTERVAL_MS)) {
        LOG_DBG("BLE sound blocked - too frequent");
        return;
    }

    buzzer_state.last_endpoint_change = now;
    pending_system_sound = 3;
    k_work_submit_to_queue(&buzzer_state.work_queue, &system_work);
}

static inline void play_ble_connected_sound(void) {
    uint32_t now = k_uptime_get_32();

    if (!buzzer_state.hw_ready) {
        return;
    }

    // Connection sounds are less restricted but still have basic protection
    if (should_block_sound(now, buzzer_state.last_sound_played, MIN_SOUND_INTERVAL_MS)) {
        LOG_DBG("BLE connected sound blocked - too frequent");
        return;
    }

    k_work_submit_to_queue(&buzzer_state.work_queue, &connection_work);
}

// Optimized event listeners
static int buzzer_listener(const zmk_event_t *eh) {
    const struct zmk_ble_active_profile_changed *profile_ev = as_zmk_ble_active_profile_changed(eh);
    if (!profile_ev) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    uint8_t profile_index = profile_ev->index;
    if (profile_index < MAX_BLE_PROFILES) {
        buzzer_state.current_profile = profile_index;
        play_profile_sound(profile_index);
        LOG_DBG("Profile changed to %d", profile_index);
    }

    return ZMK_EV_EVENT_BUBBLE;
}

static int endpoint_listener(const zmk_event_t *eh) {
    const struct zmk_endpoint_changed *endpoint_ev = as_zmk_endpoint_changed(eh);
    if (!endpoint_ev) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    // Check endpoint type using transport field
    if (endpoint_ev->endpoint.transport == ZMK_TRANSPORT_USB) {
        play_usb_sound();
        LOG_DBG("Endpoint: USB");
    } else if (endpoint_ev->endpoint.transport == ZMK_TRANSPORT_BLE) {
        play_ble_sound();
        LOG_DBG("Endpoint: BLE");
    }

    return ZMK_EV_EVENT_BUBBLE;
}

// Optimized BLE connection monitoring
static void ble_connection_monitor(struct k_work *work) {
    bool state_changed = false;

    // Check only the active profile for connection state
    uint8_t active_profile = zmk_ble_active_profile_index();
    bool current_state = zmk_ble_active_profile_is_connected();

    // Update current profile tracking
    buzzer_state.current_profile = active_profile;

    // Check for new connection on active profile
    if (current_state && !buzzer_state.connection_states[active_profile]) {
        play_ble_connected_sound();
        LOG_INF("BLE profile %d connected", active_profile);
        state_changed = true;
    }

    // Update connection state for active profile
    buzzer_state.connection_states[active_profile] = current_state;

    if (state_changed) {
        LOG_DBG("BLE connection states updated");
    }
}

K_WORK_DEFINE(ble_monitor_work, ble_connection_monitor);

static void ble_monitor_timer_handler(struct k_timer *timer) {
    k_work_submit_to_queue(&buzzer_state.work_queue, &ble_monitor_work);
}

K_TIMER_DEFINE(ble_monitor_timer, ble_monitor_timer_handler, NULL);

// Optimized buzzer initialization
static int buzzer_init(void) {
    // Hardware check
    if (!device_is_ready(pwm.dev)) {
        LOG_ERR("PWM device %s not ready", pwm.dev->name);
        return -ENODEV;
    }

    buzzer_state.hw_ready = true;

    // Initialize work queue with dedicated thread
    k_work_queue_init(&buzzer_state.work_queue);
    k_work_queue_start(&buzzer_state.work_queue, buzzer_state.work_stack,
                       K_THREAD_STACK_SIZEOF(buzzer_state.work_stack), BUZZER_THREAD_PRIORITY,
                       NULL);

    // Initialize BLE connection state tracking
    buzzer_state.current_profile = zmk_ble_active_profile_index();

    // Initialize connection states - only track active profile
    for (int i = 0; i < MAX_BLE_PROFILES; i++) {
        buzzer_state.connection_states[i] = false; // Initialize to false
    }

    // Set current active profile connection state
    buzzer_state.connection_states[buzzer_state.current_profile] =
        zmk_ble_active_profile_is_connected();

    // Delay for system stability
    k_msleep(300);

    // Play startup sound
    play_startup_sound();

    // Start BLE monitoring with optimized interval
    k_timer_start(&ble_monitor_timer, K_MSEC(BLE_MONITOR_INTERVAL_MS),
                  K_MSEC(BLE_MONITOR_INTERVAL_MS));

    LOG_INF("Buzzer system initialized (thread priority: %d)", BUZZER_THREAD_PRIORITY);
    return 0;
}

ZMK_LISTENER(buzzer_output_status, buzzer_listener)
ZMK_LISTENER(buzzer_endpoint_status, endpoint_listener)

#if defined(CONFIG_ZMK_BLE)
ZMK_SUBSCRIPTION(buzzer_output_status, zmk_ble_active_profile_changed);
#endif

#if defined(CONFIG_ZMK_USB) || defined(CONFIG_ZMK_BLE)
ZMK_SUBSCRIPTION(buzzer_endpoint_status, zmk_endpoint_changed);
#endif

// Initialize buzzer after system startup
SYS_INIT(buzzer_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
#endif
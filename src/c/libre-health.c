#include <pebble.h>

static Window *s_main_window;
static TextLayer *s_text_layer;
static TextLayer *s_status_layer;
static bool s_launched_by_wakeup = false;
static bool s_wakeup_enabled = false;

// Define message keys
#define KEY_STEP_COUNT 100
#define KEY_HEART_RATE 101
#define KEY_SLEEP 102

// Define Wakeup constants
#define WAKEUP_COOKIE 717  // A random identifier for your wakeup events

// Persistent storage key
#define PERSIST_KEY_WAKEUP_ENABLED 1

static void schedule_next_wakeup();

static void reset_text_layer() {
  if (s_text_layer) {
    text_layer_set_text(s_text_layer, "LibreHealth\n\nUP: Enable\nSELECT: Send\nDOWN: Disable");
  }
}

static void send_message_to_phone() {
  // Get health data
  HealthMetric step_metric = HealthMetricStepCount;
  HealthMetric sleep_metric = HealthMetricSleepSeconds;
  
  int steps = (int)health_service_sum_today(step_metric);
  int heart_rate = (int)health_service_peek_current_value(HealthMetricHeartRateBPM);
  int sleep_seconds = (int)health_service_sum_today(sleep_metric);
  
  // Create a dictionary
  DictionaryIterator *iter;
  AppMessageResult result = app_message_outbox_begin(&iter);
  
  if (result == APP_MSG_OK) {
    // Add health data to the message
    dict_write_int(iter, KEY_STEP_COUNT, &steps, sizeof(steps), true);
    dict_write_int(iter, KEY_HEART_RATE, &heart_rate, sizeof(heart_rate), true);
    dict_write_int(iter, KEY_SLEEP, &sleep_seconds, sizeof(sleep_seconds), true);
    
    // Send the message
    result = app_message_outbox_send();
    
    if (result == APP_MSG_OK) {
      APP_LOG(APP_LOG_LEVEL_INFO, ">> DATA SENT: Steps: %d, HR: %d, Sleep: %ds", 
              steps, heart_rate, sleep_seconds);
    } else {
      APP_LOG(APP_LOG_LEVEL_ERROR, "Error sending message: %d", (int)result);
    }
  } else {
    APP_LOG(APP_LOG_LEVEL_ERROR, "Error preparing message: %d", (int)result);
  }
}

// --- Wakeup Logic ---
static void update_status_display() {
  if (s_status_layer) {
    if (s_wakeup_enabled) {
      text_layer_set_text(s_status_layer, "Hourly: ON");
    } else {
      text_layer_set_text(s_status_layer, "Hourly: OFF");
    }
  }
}

static void schedule_next_wakeup() {
  if (!s_wakeup_enabled) {
    wakeup_cancel_all();
    APP_LOG(APP_LOG_LEVEL_INFO, "Wakeup disabled, canceling all wakeups");
    return;
  }

  wakeup_cancel_all();
  
  time_t future_time = time(NULL) + /60 * 60; // One Hour from now
  
  WakeupId id = wakeup_schedule(future_time, WAKEUP_COOKIE, true);
  
  if (id >= 0) {
    APP_LOG(APP_LOG_LEVEL_INFO, ">> NEXT WAKEUP SCHEDULED for: %d (in 3600s)", (int)future_time);
  } else {
    APP_LOG(APP_LOG_LEVEL_ERROR, "FAILED to schedule wakeup: %d", (int)id);
  }
}

static void wakeup_handler(WakeupId id, int32_t cookie) {
  // This handler is called if the app is ALREADY running when the timer fires
  APP_LOG(APP_LOG_LEVEL_INFO, "Wakeup triggered while app open!");
  
  send_message_to_phone();
  schedule_next_wakeup();
}

// --- Existing Handlers ---

static void reset_timer_callback(void *data) {
  reset_text_layer();
}

static void select_click_handler(ClickRecognizerRef recognizer, void *context) {
  text_layer_set_text(s_text_layer, "Sending data...");
  send_message_to_phone();
  
  // Reset screen after 2 seconds
  app_timer_register(2000, reset_timer_callback, NULL);
}

static void up_click_handler(ClickRecognizerRef recognizer, void *context) {
  s_wakeup_enabled = true;
  persist_write_bool(PERSIST_KEY_WAKEUP_ENABLED, true);
  text_layer_set_text(s_text_layer, "Hourly wakeup\nACTIVATED");
  update_status_display();
  schedule_next_wakeup();
  APP_LOG(APP_LOG_LEVEL_INFO, "Hourly wakeup ENABLED by user");
  
  // Reset screen after 2 seconds
  app_timer_register(2000, reset_timer_callback, NULL);
}

static void down_click_handler(ClickRecognizerRef recognizer, void *context) {
  s_wakeup_enabled = false;
  persist_write_bool(PERSIST_KEY_WAKEUP_ENABLED, false);
  text_layer_set_text(s_text_layer, "Hourly wakeup\nDISABLED");
  update_status_display();
  schedule_next_wakeup(); // This will cancel all wakeups
  APP_LOG(APP_LOG_LEVEL_INFO, "Hourly wakeup DISABLED by user");
  
  // Reset screen after 2 seconds
  app_timer_register(2000, reset_timer_callback, NULL);
}

static void click_config_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_SELECT, select_click_handler);
  window_single_click_subscribe(BUTTON_ID_UP, up_click_handler);
  window_single_click_subscribe(BUTTON_ID_DOWN, down_click_handler);
}

static void outbox_sent_handler(DictionaryIterator *iterator, void *context) {
  APP_LOG(APP_LOG_LEVEL_INFO, "AppMessage: Delivery Confirmed.");
  
  // If launched by wakeup, close the app after successful send
  if (s_launched_by_wakeup) {
    APP_LOG(APP_LOG_LEVEL_INFO, "Auto-scheduled send complete. Closing app...");
    window_stack_pop_all(false);
  }
}

static void outbox_failed_handler(DictionaryIterator *iterator, AppMessageResult reason, void *context) {
  APP_LOG(APP_LOG_LEVEL_ERROR, "AppMessage: Delivery Failed: %d", (int)reason);
}

static void inbox_received_handler(DictionaryIterator *iterator, void *context) {
  APP_LOG(APP_LOG_LEVEL_INFO, "AppMessage: Received message");
}

static void inbox_dropped_handler(AppMessageResult reason, void *context) {
  APP_LOG(APP_LOG_LEVEL_ERROR, "AppMessage: Dropped: %d", (int)reason);
}

static void main_window_load(Window *window) {
  Layer *window_layer = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(window_layer);
  
  // Status layer at the top
  s_status_layer = text_layer_create(GRect(0, 10, bounds.size.w, 30));
  text_layer_set_text_alignment(s_status_layer, GTextAlignmentCenter);
  text_layer_set_font(s_status_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
  layer_add_child(window_layer, text_layer_get_layer(s_status_layer));
  update_status_display();
  
  // Main text layer in the center
  s_text_layer = text_layer_create(GRect(0, bounds.size.h / 2 - 40, bounds.size.w, 120));
  text_layer_set_text(s_text_layer, "LibreHealth\n\nUP: Enable\nSELECT: Send\nDOWN: Disable");
  text_layer_set_text_alignment(s_text_layer, GTextAlignmentCenter);
  text_layer_set_font(s_text_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
  layer_add_child(window_layer, text_layer_get_layer(s_text_layer));
}

static void main_window_unload(Window *window) {
  text_layer_destroy(s_text_layer);
  text_layer_destroy(s_status_layer);
}

static void init() {
  // 1. Load persistent state
  s_wakeup_enabled = persist_exists(PERSIST_KEY_WAKEUP_ENABLED) 
                     ? persist_read_bool(PERSIST_KEY_WAKEUP_ENABLED) 
                     : false;
  APP_LOG(APP_LOG_LEVEL_INFO, "Loaded wakeup state: %s", s_wakeup_enabled ? "ENABLED" : "DISABLED");

  // 2. Open AppMessage
  app_message_register_inbox_received(inbox_received_handler);
  app_message_register_inbox_dropped(inbox_dropped_handler);
  app_message_register_outbox_sent(outbox_sent_handler);
  app_message_register_outbox_failed(outbox_failed_handler);
  app_message_open(128, 128);

  // 3. Subscribe to Wakeup Service (for when app is already open)
  wakeup_service_subscribe(wakeup_handler);

  // 4. Create Window
  s_main_window = window_create();
  window_set_click_config_provider(s_main_window, click_config_provider);
  window_set_window_handlers(s_main_window, (WindowHandlers) {
    .load = main_window_load,
    .unload = main_window_unload,
  });
  window_stack_push(s_main_window, true);

  // 5. Handle Launch Reason
  if (launch_reason() == APP_LAUNCH_WAKEUP) {
    // The app was started by the system because the timer fired
    WakeupId id = 0;
    int32_t cookie = 0;
    
    // Get details about the wakeup
    wakeup_get_launch_event(&id, &cookie);
    
    APP_LOG(APP_LOG_LEVEL_INFO, ">> APP LAUNCHED BY WAKEUP (Cookie: %d)", (int)cookie);
    
    // Mark that we were launched by wakeup
    s_launched_by_wakeup = true;
    
    // Show different message for auto-send
    text_layer_set_text(s_text_layer, "LibreHealth\nsending data...");
    
    // Send data immediately
    send_message_to_phone();
    
    // Schedule the NEXT wakeup to keep the loop going
    schedule_next_wakeup();
    
  } else {
    // The app was started by the user manually
    APP_LOG(APP_LOG_LEVEL_INFO, ">> APP LAUNCHED MANUALLY");
    
    // Ensure wakeup state is correct
    schedule_next_wakeup();
  }
}

static void deinit() {
  window_destroy(s_main_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
#include <pebble.h>

static Window *s_main_window;
static TextLayer *s_text_layer;
static TextLayer *s_status_layer;
static bool s_launched_by_wakeup = false;
static bool s_wakeup_enabled = false;
static AppTimer *s_close_timer = NULL;

// Define message keys
#define KEY_HEART_RATE 101
#define KEY_SLEEP 102
#define KEY_CURRENT_HOUR 103
#define KEY_CURRENT_DAY 104
#define KEY_CURRENT_MONTH 105
#define KEY_CURRENT_YEAR 106
#define KEY_STEP_HOUR_0 200

// Define Wakeup constants
#define WAKEUP_COOKIE 717  // A random identifier for your wakeup events

// Persistent storage keys
#define PERSIST_KEY_WAKEUP_ENABLED 1
#define PERSIST_KEY_LAST_STEPS 10
#define PERSIST_KEY_LAST_DAY 11
#define PERSIST_KEY_HOUR_0 20  // 20..43 for hours 0..23

static void schedule_next_wakeup();

static void reset_text_layer() {
  if (s_text_layer) {
    text_layer_set_text(s_text_layer, "LibreHealth\n\nUP: Enable\nSELECT: Send\nDOWN: Disable");
  }
}

static void update_hourly_steps() {
  time_t now = time(NULL);
  struct tm *now_tm = localtime(&now);
  int current_hour = now_tm->tm_hour;
  int current_day = now_tm->tm_yday;

  // Detect day change — reset all hourly buckets
  int last_day = persist_exists(PERSIST_KEY_LAST_DAY) ? persist_read_int(PERSIST_KEY_LAST_DAY) : -1;
  if (last_day != current_day) {
    for (int h = 0; h < 24; h++) {
      persist_write_int(PERSIST_KEY_HOUR_0 + h, 0);
    }
    persist_write_int(PERSIST_KEY_LAST_STEPS, 0);
    persist_write_int(PERSIST_KEY_LAST_DAY, current_day);
    APP_LOG(APP_LOG_LEVEL_INFO, "New day detected, reset hourly buckets");
  }

  // Compute delta since last update
  int current_total = (int)health_service_sum_today(HealthMetricStepCount);
  int last_total = persist_exists(PERSIST_KEY_LAST_STEPS) ? persist_read_int(PERSIST_KEY_LAST_STEPS) : 0;
  int delta = current_total - last_total;

  if (delta > 0) {
    int hour_steps = persist_exists(PERSIST_KEY_HOUR_0 + current_hour)
                     ? persist_read_int(PERSIST_KEY_HOUR_0 + current_hour) : 0;
    hour_steps += delta;
    persist_write_int(PERSIST_KEY_HOUR_0 + current_hour, hour_steps);
    APP_LOG(APP_LOG_LEVEL_INFO, "Hour %d: +%d steps (now %d)", current_hour, delta, hour_steps);
  }

  persist_write_int(PERSIST_KEY_LAST_STEPS, current_total);
}

static void send_message_to_phone() {
  // Update hourly buckets before sending
  update_hourly_steps();

  // Check Bluetooth connection before sending
  if (!bluetooth_connection_service_peek()) {
    APP_LOG(APP_LOG_LEVEL_WARNING, "No phone connection. Data not sent.");
    text_layer_set_text(s_text_layer, "No phone connection!");
    return;
  }

  // Read all stored hourly values
  int hourly_steps[24];
  for (int h = 0; h < 24; h++) {
    hourly_steps[h] = persist_exists(PERSIST_KEY_HOUR_0 + h)
                      ? persist_read_int(PERSIST_KEY_HOUR_0 + h) : 0;
  }

  int heart_rate = (int)health_service_peek_current_value(HealthMetricHeartRateBPM);
  int sleep_seconds = (int)health_service_sum_today(HealthMetricSleepSeconds);

  time_t now = time(NULL);
  struct tm *now_tm = localtime(&now);
  int current_day = now_tm->tm_mday;
  int current_hour = now_tm->tm_hour;
  int current_month = now_tm->tm_mon; // 0-indexed month
  int current_year = now_tm->tm_year + 1900;

  // Open outbox and write everything quickly
  DictionaryIterator *iter;
  AppMessageResult result = app_message_outbox_begin(&iter);

  if (result == APP_MSG_OK) {
    for (int h = 0; h < 24; h++) {
      dict_write_int(iter, KEY_STEP_HOUR_0 + h, &hourly_steps[h], sizeof(int), true);
    }

    dict_write_int(iter, KEY_HEART_RATE, &heart_rate, sizeof(heart_rate), true);
    dict_write_int(iter, KEY_SLEEP, &sleep_seconds, sizeof(sleep_seconds), true);
    dict_write_int(iter, KEY_CURRENT_DAY, &current_day, sizeof(current_day), true);
    dict_write_int(iter, KEY_CURRENT_HOUR, &current_hour, sizeof(current_hour), true);
    dict_write_int(iter, KEY_CURRENT_MONTH, &current_month, sizeof(current_month), true);
    dict_write_int(iter, KEY_CURRENT_YEAR, &current_year, sizeof(current_year), true);

    result = app_message_outbox_send();

    if (result == APP_MSG_OK) {
      APP_LOG(APP_LOG_LEVEL_INFO, ">> DATA SENT: 24 STEP VALUES, HR: %d, Sleep: %ds, Day: %d, Hour: %d, Month: %d, Year: %d", heart_rate, sleep_seconds, current_day, current_hour, current_month, current_year);
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
  
  time_t future_time = time(NULL) + 30 * 60; // Next schedule in 30 min.
  
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

static void close_app_timeout_callback(void *data) {
  APP_LOG(APP_LOG_LEVEL_INFO, "Timeout reached - no connection. Closing app...");
  s_close_timer = NULL;
  window_stack_pop_all(false);
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
    // Cancel the timeout timer if it exists
    if (s_close_timer) {
      app_timer_cancel(s_close_timer);
      s_close_timer = NULL;
    }
    APP_LOG(APP_LOG_LEVEL_INFO, "Auto-scheduled send complete. Closing app...");
    window_stack_pop_all(false);
  }
}

static void outbox_failed_handler(DictionaryIterator *iterator, AppMessageResult reason, void *context) {
  APP_LOG(APP_LOG_LEVEL_ERROR, "AppMessage: Delivery Failed: %d", (int)reason);
  
  // If launched by wakeup and delivery failed (e.g., no connection), close after timeout
  if (s_launched_by_wakeup && !s_close_timer) {
    APP_LOG(APP_LOG_LEVEL_INFO, "No connection detected. Setting timeout to close app...");
    s_close_timer = app_timer_register(5000, close_app_timeout_callback, NULL); // 5 seconds timeout
  }
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
  app_message_open(1024, 1024);

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
  // Cancel any pending timers
  if (s_close_timer) {
    app_timer_cancel(s_close_timer);
    s_close_timer = NULL;
  }
  window_destroy(s_main_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
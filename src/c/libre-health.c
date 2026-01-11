#include <pebble.h>

static Window *s_main_window;
static TextLayer *s_text_layer;
static bool s_launched_by_wakeup = false;

// Define message keys
#define KEY_STEP_COUNT 100
#define KEY_HEART_RATE 101
#define KEY_SLEEP 102

// Define Wakeup constants
#define WAKEUP_COOKIE 717  // A random identifier for your wakeup events

static void schedule_next_wakeup();

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
      // Update UI if visible
      if (s_text_layer) {
        text_layer_set_text(s_text_layer, "Sent health data!");
      }
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

static void schedule_next_wakeup() {
  // Check if we already have a wakeup scheduled to avoid duplicates
  WakeupId id = 0;
  time_t timestamp = 0;
  
  // wakeup_query checks if a wakeup with our specific cookie exists
  if (wakeup_query(WAKEUP_COOKIE, &timestamp)) {
    // Already scheduled, no need to do anything
    // APP_LOG(APP_LOG_LEVEL_DEBUG, "Wakeup already scheduled for %d", (int)timestamp);
    return;
  }

  time_t future_time = time(NULL) +  15 * 60;
  
  // Schedule the wakeup
  // notify_if_missed=true will alert the user if the watch was off during the time
  id = wakeup_schedule(future_time, WAKEUP_COOKIE, true);
  
  if (id >= 0) {
    APP_LOG(APP_LOG_LEVEL_INFO, ">> NEXT WAKEUP SCHEDULED for: %d (in 60s)", (int)future_time);
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

static void select_click_handler(ClickRecognizerRef recognizer, void *context) {
  text_layer_set_text(s_text_layer, "Sending health\ndata...");
  send_message_to_phone();
  schedule_next_wakeup(); // Ensure scheduling is active on manual click too
}

static void click_config_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_SELECT, select_click_handler);
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
  
  s_text_layer = text_layer_create(GRect(0, bounds.size.h / 2 - 40, bounds.size.w, 80));
  text_layer_set_text(s_text_layer, "LibreHealth\nActive\nSELECT to trigger");
  text_layer_set_text_alignment(s_text_layer, GTextAlignmentCenter);
  text_layer_set_font(s_text_layer, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
  layer_add_child(window_layer, text_layer_get_layer(s_text_layer));
}

static void main_window_unload(Window *window) {
  text_layer_destroy(s_text_layer);
}

static void init() {
  // 1. Open AppMessage
  app_message_register_inbox_received(inbox_received_handler);
  app_message_register_inbox_dropped(inbox_dropped_handler);
  app_message_register_outbox_sent(outbox_sent_handler);
  app_message_register_outbox_failed(outbox_failed_handler);
  app_message_open(128, 128);

  // 2. Subscribe to Wakeup Service (for when app is already open)
  wakeup_service_subscribe(wakeup_handler);

  // 3. Create Window
  s_main_window = window_create();
  window_set_click_config_provider(s_main_window, click_config_provider);
  window_set_window_handlers(s_main_window, (WindowHandlers) {
    .load = main_window_load,
    .unload = main_window_unload,
  });
  window_stack_push(s_main_window, true);

  // 4. Handle Launch Reason
  if (launch_reason() == APP_LAUNCH_WAKEUP) {
    // The app was started by the system because the timer fired
    WakeupId id = 0;
    int32_t cookie = 0;
    
    // Get details about the wakeup
    wakeup_get_launch_event(&id, &cookie);
    
    APP_LOG(APP_LOG_LEVEL_INFO, ">> APP LAUNCHED BY WAKEUP (Cookie: %d)", (int)cookie);
    
    // Mark that we were launched by wakeup
    s_launched_by_wakeup = true;
    
    // Send data immediately
    send_message_to_phone();
    
    // Schedule the NEXT wakeup to keep the loop going
    schedule_next_wakeup();
    
  } else {
    // The app was started by the user manually
    APP_LOG(APP_LOG_LEVEL_INFO, ">> APP LAUNCHED MANUALLY");
    
    // Start the loop
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
#include <pebble.h>

static Window *s_main_window;
static TextLayer *s_text_layer;

// Define message keys
#define KEY_STEP_COUNT 100
#define KEY_HEART_RATE 101
#define KEY_SLEEP 102

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
      text_layer_set_text(s_text_layer, "Sent health data!");
      APP_LOG(APP_LOG_LEVEL_INFO, "Health data sent! Steps: %d, HR: %d, Sleep: %ds", 
              steps, heart_rate, sleep_seconds);
    } else {
      text_layer_set_text(s_text_layer, "Error sending\nhealth data");
      APP_LOG(APP_LOG_LEVEL_ERROR, "Error sending message: %d", (int)result);
    }
  } else {
    text_layer_set_text(s_text_layer, "Error preparing\nmessage");
    APP_LOG(APP_LOG_LEVEL_ERROR, "Error preparing message: %d", (int)result);
  }
}

static void select_click_handler(ClickRecognizerRef recognizer, void *context) {
  // Update the text layer
  text_layer_set_text(s_text_layer, "Sending health\ndata...");
  
  // Send message to phone
  send_message_to_phone();
}

static void click_config_provider(void *context) {
  // Register the select button (middle button) click handler
  window_single_click_subscribe(BUTTON_ID_SELECT, select_click_handler);
}

static void outbox_sent_handler(DictionaryIterator *iterator, void *context) {
  APP_LOG(APP_LOG_LEVEL_INFO, "Message confirmed sent to phone!");
}

static void outbox_failed_handler(DictionaryIterator *iterator, AppMessageResult reason, void *context) {
  APP_LOG(APP_LOG_LEVEL_ERROR, "Failed to send message: %d", (int)reason);
}

static void inbox_received_handler(DictionaryIterator *iterator, void *context) {
  APP_LOG(APP_LOG_LEVEL_INFO, "Message received from phone");
}

static void inbox_dropped_handler(AppMessageResult reason, void *context) {
  APP_LOG(APP_LOG_LEVEL_ERROR, "Message dropped: %d", (int)reason);
}

static void main_window_load(Window *window) {
  Layer *window_layer = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(window_layer);
  
  // Create text layer
  s_text_layer = text_layer_create(GRect(0, bounds.size.h / 2 - 40, bounds.size.w, 80));
  text_layer_set_text(s_text_layer, "Press SELECT\nto send health");
  text_layer_set_text_alignment(s_text_layer, GTextAlignmentCenter);
  text_layer_set_font(s_text_layer, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
  layer_add_child(window_layer, text_layer_get_layer(s_text_layer));
}

static void main_window_unload(Window *window) {
  // Destroy text layer
  text_layer_destroy(s_text_layer);
}

static void init() {
  // Register AppMessage handlers
  app_message_register_inbox_received(inbox_received_handler);
  app_message_register_inbox_dropped(inbox_dropped_handler);
  app_message_register_outbox_sent(outbox_sent_handler);
  app_message_register_outbox_failed(outbox_failed_handler);
  
  // Open AppMessage with buffer sizes
  app_message_open(128, 128);
  
  // Create main Window
  s_main_window = window_create();
  window_set_click_config_provider(s_main_window, click_config_provider);
  window_set_window_handlers(s_main_window, (WindowHandlers) {
    .load = main_window_load,
    .unload = main_window_unload,
  });
  window_stack_push(s_main_window, true);
}

static void deinit() {
  // Destroy main Window
  window_destroy(s_main_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}

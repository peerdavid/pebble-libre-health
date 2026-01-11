#include <pebble.h>

static Window *s_main_window;
static TextLayer *s_text_layer;
static int s_counter = 0;

// Define message keys
#define KEY_TEST_NUMBER 100

static void send_message_to_phone() {
  // Create a dictionary
  DictionaryIterator *iter;
  AppMessageResult result = app_message_outbox_begin(&iter);
  
  if (result == APP_MSG_OK) {
    // Add the test number to the message
    dict_write_int(iter, KEY_TEST_NUMBER, &s_counter, sizeof(s_counter), true);
    
    // Send the message
    result = app_message_outbox_send();
    
    if (result == APP_MSG_OK) {
      APP_LOG(APP_LOG_LEVEL_INFO, "Message sent successfully! Counter: %d", s_counter);
      s_counter++;
    } else {
      APP_LOG(APP_LOG_LEVEL_ERROR, "Error sending message: %d", (int)result);
    }
  } else {
    APP_LOG(APP_LOG_LEVEL_ERROR, "Error preparing message: %d", (int)result);
  }
}

static void select_click_handler(ClickRecognizerRef recognizer, void *context) {
  // Update the text layer
  static char buffer[32];
  snprintf(buffer, sizeof(buffer), "Sending: %d", s_counter);
  text_layer_set_text(s_text_layer, buffer);
  
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
  text_layer_set_text(s_text_layer, "Press SELECT\nto send data");
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

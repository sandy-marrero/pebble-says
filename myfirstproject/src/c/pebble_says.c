#include <pebble.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>

#define MAX_SEQUENCE 8
#define SHOW_MS 700  // base show duration; will ramp down each round
#define PAUSE_MS 300

typedef enum {
  SEQ_BTN_UP = 0,
  SEQ_BTN_SELECT = 1,
  SEQ_BTN_DOWN = 2
} SequenceButton;

static Window *s_window;
static TextLayer *s_text_layer;
static TextLayer *s_info_layer;
static TextLayer *s_glyph_layers[3];
// Using simple letter glyphs (U/S/D) instead of icon overlay layers
static TextLayer *s_title_layer; // title splash layer
static Layer *s_flash_layer;     // full-screen flash/invert layer for celebrations

static int s_sequence[MAX_SEQUENCE];
static int s_seq_len = 0;
static int s_input_index = 0;


static bool s_showing = false;
static int s_show_index = 0;
static int s_show_phase = 0; // 0 = show, 1 = pause
static AppTimer *s_sequence_timer = NULL;
static AppTimer *s_feedback_timer = NULL;
static AppTimer *s_transition_timer = NULL; // round completion animation timer
static AppTimer *s_flash_timer = NULL;      // flash animation timer

static bool s_game_over = false;
static int s_round = 0;
static bool s_transitioning = false; // block input during round-end animation
static int s_show_ms_current = SHOW_MS;     // dynamically adjusted show time
static int s_flash_phase = 0;               // flash animation phase counter
static int s_flash_interval_ms = 150;       // per-tick interval for flash

static void schedule_sequence_timer(uint32_t ms);
static void start_show_sequence(void *data);
static void begin_round(void *data);
static void apply_layout(void);
static void highlight_glyph(int idx, bool on);
static void clear_glyph_callback(void *data);
static void sequence_timer_callback(void *data);
static void clear_feedback_callback(void *data);
static void start_round_transition(void);
static void round_transition_callback(void *data);
static void flash_layer_update_proc(Layer *layer, GContext *ctx);
static void start_flash_animation(int cycles);
static void flash_animation_tick(void *data);
static int calc_show_ms(int len);

static const char* button_name(int b) {
  switch (b) {
    case SEQ_BTN_UP: return "Up";
    case SEQ_BTN_SELECT: return "Select";
    case SEQ_BTN_DOWN: return "Down";
    default: return "?";
  }
}

static void show_message(const char *msg) {
  text_layer_set_text(s_text_layer, msg);
}

static void update_info_layer(void) {
  static char buf[48];
  if (s_game_over) {
    if (s_seq_len == 0) {
      // Initial start screen: no duplicate 'Press Select'
      buf[0] = '\0';
    } else {
      // After a game has been played (loss or win): provide restart instructions
      snprintf(buf, sizeof(buf), "Press Select to Restart\nRound: %d", s_round);
    }
  } else {
    snprintf(buf, sizeof(buf), "Round: %d", s_round);
  }
  text_layer_set_text(s_info_layer, buf);
}

static void apply_layout(void) {
  if (!s_window) return;
  Layer *window_layer = window_get_root_layer(s_window);
  GRect bounds = layer_get_bounds(window_layer);
  int glyph_w = 36;
  int usable_w = bounds.size.w - glyph_w;
  // Goal: command text (s_text_layer) exactly centered vertically; title above; info below.
  const int text_h = 44;     // GOTHIC_28_BOLD block height
  const int title_h = 34;    // GOTHIC_28_BOLD title
  const int gap = 6;         // spacing between elements
  bool initial_screen = (s_game_over && s_seq_len == 0);
  const int info_h = (s_game_over && !initial_screen) ? 44 : 28; // larger only when showing restart info

  const int center_y = bounds.size.h / 2;
  int text_y = center_y - text_h / 2;
  int title_y = text_y - title_h - gap;
  if (title_y < 0) title_y = 0; // clamp to screen top
  int info_y = text_y + text_h + gap;
  if (info_y + info_h > bounds.size.h) {
    // Clamp info to bottom if needed
    info_y = bounds.size.h - info_h;
  }

  layer_set_frame(text_layer_get_layer(s_text_layer), GRect(0, text_y, usable_w, text_h));
  text_layer_set_text_alignment(s_text_layer, GTextAlignmentCenter);

  if (s_title_layer) {
    layer_set_frame(text_layer_get_layer(s_title_layer), GRect(0, title_y, usable_w, title_h));
    text_layer_set_text_alignment(s_title_layer, GTextAlignmentCenter);
  }

  layer_set_frame(text_layer_get_layer(s_info_layer), GRect(0, info_y, usable_w, info_h));
  text_layer_set_text_alignment(s_info_layer, GTextAlignmentCenter);
  layer_set_hidden(text_layer_get_layer(s_info_layer), initial_screen); // hide info on initial screen
}

static void add_random_step(void) {
  if (s_seq_len < MAX_SEQUENCE) {
    s_sequence[s_seq_len++] = rand() % 3;
    APP_LOG(APP_LOG_LEVEL_INFO, "Added step %d (len=%d)", s_sequence[s_seq_len-1], s_seq_len);
  }
}

static int calc_show_ms(int len) {
  // Steeper early, gentler later; floor at 200ms
  int ms;
  if (len <= 1) ms = SHOW_MS;
  else if (len <= 3) ms = SHOW_MS - 60 * (len - 1);
  else ms = SHOW_MS - 60 * 2 - 35 * (len - 3);
  if (ms < 200) ms = 200;
  return ms;
}

static void begin_round(void *data) {
  s_input_index = 0;
  s_show_index = 0;
  s_show_phase = 0;
  s_showing = true;
  s_game_over = false;
  s_transitioning = false;
  s_round = s_seq_len;
  // speed ramp with piecewise curve
  s_show_ms_current = calc_show_ms(s_seq_len);
  update_info_layer();
  APP_LOG(APP_LOG_LEVEL_INFO, "Begin round %d (seq_len=%d, show_ms=%d)", s_round, s_seq_len, s_show_ms_current);
  // start showing immediately
  start_show_sequence(NULL);
  apply_layout();
}

static void end_game(void) {
  s_game_over = true;
  s_showing = false;
  if (s_sequence_timer) {
    app_timer_cancel(s_sequence_timer);
    s_sequence_timer = NULL;
  }
  APP_LOG(APP_LOG_LEVEL_INFO, "Game over at round %d, seq_len=%d", s_round, s_seq_len);
  update_info_layer();
  show_message("Game Over");
  apply_layout();
}

static void sequence_timer_callback(void *data) {
  // The timer that invoked this callback has now expired; clear the handle
  // so we don't try to cancel an already-fired timer later (which logs warnings).
  s_sequence_timer = NULL;
  if (!s_showing) return;

  if (s_show_index >= s_seq_len) {
    // finished showing
    s_showing = false;
    // clear any highlighted glyphs
    for (int i = 0; i < 3; ++i) highlight_glyph(i, false);
    show_message("Your turn");
    return;
  }

  if (s_show_phase == 0) {
    // show current step
    int step = s_sequence[s_show_index];
    show_message(button_name(step));
    // highlight shown glyph
    highlight_glyph(step, true);
    s_show_phase = 1;
  schedule_sequence_timer(s_show_ms_current);
  } else {
    // pause between steps
    show_message("");
    // clear highlight for the step we just showed
    int prev_step = s_sequence[s_show_index];
    highlight_glyph(prev_step, false);
    s_show_phase = 0;
    s_show_index++;
    schedule_sequence_timer(PAUSE_MS);
  }
}

static void schedule_sequence_timer(uint32_t ms) {
  if (s_sequence_timer) {
    app_timer_cancel(s_sequence_timer);
  }
  s_sequence_timer = app_timer_register(ms, sequence_timer_callback, NULL);
}

static void start_show_sequence(void *data) {
  // Disable input while showing
  s_showing = true;
  s_show_index = 0;
  s_show_phase = 0;
  s_transitioning = false; // ensure not in transition
  // Drop any stale handle left over from prior rounds to avoid cancel warnings
  s_sequence_timer = NULL;
  APP_LOG(APP_LOG_LEVEL_DEBUG, "Starting to show sequence (len=%d, show_ms=%d)", s_seq_len, s_show_ms_current);
  sequence_timer_callback(NULL);
}

static void clear_feedback_callback(void *data) {
  // after brief "Good" feedback, prompt the player for the next input
  show_message("Your turn");
}

static void highlight_glyph(int idx, bool on) {
  if (idx < 0 || idx > 2) return;
  if (!s_glyph_layers[idx]) return;
  // set highlight colors for letter glyphs
  #ifdef PBL_COLOR
  GColor base;
  switch(idx) {
    case 0: base = GColorRed; break;
    case 1: base = GColorBlue; break;
    case 2: base = GColorIslamicGreen; break;
    default: base = GColorDarkGray; break;
  }
  if (on) {
    text_layer_set_background_color(s_glyph_layers[idx], base);
    text_layer_set_text_color(s_glyph_layers[idx], GColorWhite);
  } else {
    text_layer_set_background_color(s_glyph_layers[idx], GColorClear);
    text_layer_set_text_color(s_glyph_layers[idx], base);
  }
  #else
  if (on) {
    text_layer_set_background_color(s_glyph_layers[idx], GColorBlack);
    text_layer_set_text_color(s_glyph_layers[idx], GColorWhite);
  } else {
    text_layer_set_background_color(s_glyph_layers[idx], GColorClear);
    text_layer_set_text_color(s_glyph_layers[idx], GColorBlack);
  }
  #endif
}

static void clear_glyph_callback(void *data) {
  int idx = (int)(intptr_t)data;
  highlight_glyph(idx, false);
  s_feedback_timer = NULL;
}

static void start_round_transition(void) {
  s_transitioning = true;
  s_showing = false;
  // base vibration
  vibes_double_pulse();
  // milestone extra pulses
  if (s_seq_len == 4 || s_seq_len == 6) {
    vibes_short_pulse();
  } else if (s_seq_len == 8) {
    vibes_long_pulse();
  }
  static char buf[32];
  snprintf(buf, sizeof(buf), "Length %d", s_seq_len); // center celebration text
  show_message(buf);
  // start flash animation (more cycles for milestones)
  int cycles = (s_seq_len == 4 || s_seq_len == 6) ? 5 : (s_seq_len == 8 ? 7 : 3);
  start_flash_animation(cycles);
  if (s_transition_timer) {
    app_timer_cancel(s_transition_timer);
  }
  // slightly longer than flash cycles to allow finish before next round
  int duration = 150 * cycles + 200;
  s_transition_timer = app_timer_register(duration, round_transition_callback, NULL);
}

static void round_transition_callback(void *data) {
  s_transition_timer = NULL;
  // ensure flash ends
  if (s_flash_timer) {
    app_timer_cancel(s_flash_timer);
    s_flash_timer = NULL;
  }
  s_flash_phase = 0;
  layer_mark_dirty(s_flash_layer);
  begin_round(NULL);
}

static void handle_input(int pressed) {
  if (s_showing) {
    APP_LOG(APP_LOG_LEVEL_DEBUG, "Input ignored while showing: %d", pressed);
    return; // ignore input while showing
  }
  if (s_transitioning) {
    APP_LOG(APP_LOG_LEVEL_DEBUG, "Input ignored during transition: %d", pressed);
    return; // ignore input during round-end transition
  }
  if (s_game_over) {
    // only Select restarts
    APP_LOG(APP_LOG_LEVEL_DEBUG, "Input ignored - game over: %d", pressed);
    return;
  }

  if (s_input_index >= s_seq_len) {
    APP_LOG(APP_LOG_LEVEL_DEBUG, "Input ignored - index >= seq_len: idx=%d len=%d", s_input_index, s_seq_len);
    return;
  }

  APP_LOG(APP_LOG_LEVEL_INFO, "Button pressed: %d, expecting: %d (idx=%d)", pressed, s_sequence[s_input_index], s_input_index);

  // visual/vibe feedback for press
  highlight_glyph(pressed, true);
  if (s_feedback_timer) {
    app_timer_cancel(s_feedback_timer);
    s_feedback_timer = NULL;
  }
  s_feedback_timer = app_timer_register(150, clear_glyph_callback, (void*)(intptr_t)pressed);

  if (pressed == s_sequence[s_input_index]) {
    // correct
    vibes_short_pulse();
    s_input_index++;
    APP_LOG(APP_LOG_LEVEL_INFO, "Correct press, new input_index=%d", s_input_index);
    if (s_input_index == s_seq_len) {
      // completed round
      if (s_seq_len >= MAX_SEQUENCE) {
        // won at max length
        s_game_over = true;
        update_info_layer();
        show_message("You win!");
        APP_LOG(APP_LOG_LEVEL_INFO, "Player won at max sequence length %d", s_seq_len);
      } else {
        // prepare next round
        add_random_step();
        s_round = s_seq_len;
        update_info_layer();
  APP_LOG(APP_LOG_LEVEL_INFO, "Round complete, starting transition (len=%d)", s_seq_len);
  // visual confirmation for end of round
  start_round_transition();
      }
    } else {
      // prompt for next input
      show_message("Good");
      // brief feedback, then restore the prompt without restarting the sequence
      app_timer_register(200, (AppTimerCallback)clear_feedback_callback, NULL);
    }
  } else {
    // wrong
    vibes_long_pulse();
    APP_LOG(APP_LOG_LEVEL_INFO, "Wrong press: %d (expected %d) at idx=%d", pressed, s_sequence[s_input_index], s_input_index);
    end_game();
  }
}

static void prv_select_click_handler(ClickRecognizerRef recognizer, void *context) {
  if (s_game_over) {
    // restart game - start at length 1 instead of 2
    s_seq_len = 0;
    s_round = 0;
    add_random_step();
    begin_round(NULL);
    apply_layout();
    return;
  }
  handle_input(SEQ_BTN_SELECT);
}

static void prv_up_click_handler(ClickRecognizerRef recognizer, void *context) {
  handle_input(SEQ_BTN_UP);
}

static void prv_down_click_handler(ClickRecognizerRef recognizer, void *context) {
  handle_input(SEQ_BTN_DOWN);
}

static void prv_click_config_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_SELECT, prv_select_click_handler);
  window_single_click_subscribe(BUTTON_ID_UP, prv_up_click_handler);
  window_single_click_subscribe(BUTTON_ID_DOWN, prv_down_click_handler);
}

static void flash_layer_update_proc(Layer *layer, GContext *ctx) {
  if (!s_transitioning) return; // only draw flashes during transition
#ifdef PBL_COLOR
  // alternate between white and yellow flashes on color devices
  // subtle fade: draw translucent overlay depending on phase
  int phase_mod = s_flash_phase % 4;
  GColor col = (phase_mod < 2) ? GColorWhite : GColorYellow;
  graphics_context_set_fill_color(ctx, col);
  graphics_fill_rect(ctx, layer_get_bounds(layer), 0, GCornerNone);
#else
  // invert flash: fill white every other phase
  if (s_flash_phase % 2 == 0) {
    graphics_context_set_fill_color(ctx, GColorWhite);
    graphics_fill_rect(ctx, layer_get_bounds(layer), 0, GCornerNone);
  }
#endif
}

static void start_flash_animation(int cycles) {
  s_flash_phase = 0;
  // platform-specific timing tweak (Chalk slower for round face aesthetics)
#ifdef PBL_PLATFORM_CHALK
  s_flash_interval_ms = 180;
#else
  s_flash_interval_ms = 140;
#endif
  layer_mark_dirty(s_flash_layer);
  if (s_flash_timer) {
    app_timer_cancel(s_flash_timer);
  }
  s_flash_timer = app_timer_register(s_flash_interval_ms, flash_animation_tick, (void*)(intptr_t)cycles);
}

static void flash_animation_tick(void *data) {
  int cycles = (int)(intptr_t)data;
  s_flash_phase++;
  layer_mark_dirty(s_flash_layer);
  if (s_flash_phase >= cycles) {
    s_flash_timer = NULL; // done
    return;
  }
  s_flash_timer = app_timer_register(s_flash_interval_ms, flash_animation_tick, (void*)(intptr_t)cycles);
}

/* Icon overlay drawing removed: using simple letters for glyphs. */

static void prv_window_load(Window *window) {
  Layer *window_layer = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(window_layer);

  // leave a right column for glyphs (36px) and place main text to the left
  int glyph_w = 36;
  int glyph_x = bounds.size.w - glyph_w;
  // Title layer
  s_title_layer = text_layer_create(GRect(0, 4, bounds.size.w - glyph_w, 34));
  text_layer_set_text_alignment(s_title_layer, GTextAlignmentCenter);
  text_layer_set_font(s_title_layer, fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD));
  text_layer_set_text(s_title_layer, "Pebble Says");
  layer_add_child(window_layer, text_layer_get_layer(s_title_layer));

  // central text block
  s_text_layer = text_layer_create(GRect(0, 46, bounds.size.w - glyph_w, 44));
  text_layer_set_text(s_text_layer, "Press Select");
  text_layer_set_text_alignment(s_text_layer, GTextAlignmentCenter);
  text_layer_set_font(s_text_layer, fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD));
  text_layer_set_overflow_mode(s_text_layer, GTextOverflowModeWordWrap);
  layer_add_child(window_layer, text_layer_get_layer(s_text_layer));

  s_info_layer = text_layer_create(GRect(0, 98, bounds.size.w - glyph_w, 20));
  text_layer_set_text_alignment(s_info_layer, GTextAlignmentCenter);
  text_layer_set_font(s_info_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18));
  text_layer_set_overflow_mode(s_info_layer, GTextOverflowModeWordWrap);
  layer_add_child(window_layer, text_layer_get_layer(s_info_layer));

  // distribute glyphs vertically near button positions
  s_glyph_layers[0] = text_layer_create(GRect(glyph_x, 24, glyph_w, 30)); // Up
  text_layer_set_text_alignment(s_glyph_layers[0], GTextAlignmentCenter);
  text_layer_set_font(s_glyph_layers[0], fonts_get_system_font(FONT_KEY_GOTHIC_24));
  text_layer_set_text(s_glyph_layers[0], "U");
  layer_add_child(window_layer, text_layer_get_layer(s_glyph_layers[0]));

  s_glyph_layers[1] = text_layer_create(GRect(glyph_x, bounds.size.h/2 - 15, glyph_w, 30)); // Select
  text_layer_set_text_alignment(s_glyph_layers[1], GTextAlignmentCenter);
  text_layer_set_font(s_glyph_layers[1], fonts_get_system_font(FONT_KEY_GOTHIC_24));
  text_layer_set_text(s_glyph_layers[1], "S");
  layer_add_child(window_layer, text_layer_get_layer(s_glyph_layers[1]));

  s_glyph_layers[2] = text_layer_create(GRect(glyph_x, bounds.size.h - 50, glyph_w, 30)); // Down
  text_layer_set_text_alignment(s_glyph_layers[2], GTextAlignmentCenter);
  text_layer_set_font(s_glyph_layers[2], fonts_get_system_font(FONT_KEY_GOTHIC_24));
  text_layer_set_text(s_glyph_layers[2], "D");
  layer_add_child(window_layer, text_layer_get_layer(s_glyph_layers[2]));

  // initialize glyph colors in non-highlight state
  for (int i = 0; i < 3; ++i) {
    highlight_glyph(i, false);
  }


  // flash layer on top (initially invisible until transition)
  s_flash_layer = layer_create(bounds);
  layer_set_update_proc(s_flash_layer, flash_layer_update_proc);
  layer_add_child(window_layer, s_flash_layer);

  update_info_layer();
  apply_layout();
}

static void prv_window_unload(Window *window) {
  if (s_sequence_timer) {
    app_timer_cancel(s_sequence_timer);
    s_sequence_timer = NULL;
  }
  if (s_feedback_timer) {
    app_timer_cancel(s_feedback_timer);
    s_feedback_timer = NULL;
  }
  if (s_transition_timer) {
    app_timer_cancel(s_transition_timer);
    s_transition_timer = NULL;
  }
  if (s_flash_timer) {
    app_timer_cancel(s_flash_timer);
    s_flash_timer = NULL;
  }
  text_layer_destroy(s_text_layer);
  text_layer_destroy(s_info_layer);
  if (s_title_layer) {
    text_layer_destroy(s_title_layer);
    s_title_layer = NULL;
  }
  for (int i = 0; i < 3; ++i) {
    if (s_glyph_layers[i]) {
      text_layer_destroy(s_glyph_layers[i]);
      s_glyph_layers[i] = NULL;
    }
  }
  if (s_flash_layer) {
    layer_destroy(s_flash_layer);
    s_flash_layer = NULL;
  }
}

static void prv_init(void) {
  srand(time(NULL));
  s_window = window_create();
  window_set_click_config_provider(s_window, prv_click_config_provider);
  window_set_window_handlers(s_window, (WindowHandlers) {
    .load = prv_window_load,
    .unload = prv_window_unload,
  });
  const bool animated = true;
  window_stack_push(s_window, animated);

  // initialize game state
  s_seq_len = 0;
  s_round = 0;
  s_game_over = true; // show start message until user presses select
  update_info_layer();
}

static void prv_deinit(void) {
  window_destroy(s_window);
}

int main(void) {
  prv_init();

  APP_LOG(APP_LOG_LEVEL_DEBUG, "Done initializing, pushed window: %p", s_window);

  app_event_loop();
  prv_deinit();
}
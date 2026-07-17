#include <pebble.h>

// ============================================================================
// CONFIGURATION
// Layout tuned for gabbro (Pebble Round 2, 260x260 round, 64-color).
// All positions are computed from layer_get_bounds() at runtime, these
// constants are offsets/lengths relative to the dial center.
// ============================================================================

#define CLOCK_RADIUS 120
#define HOUR_HAND_LENGTH 72
#define HOUR_HAND_WIDTH 5
#define MINUTE_HAND_LENGTH 98
#define MINUTE_HAND_WIDTH 4

#define READOUT_OFFSET_X 72   // air readout at +X (3 o'clock), aare at -X (9 o'clock)
#define DATE_OFFSET_Y 84      // date box below center

static const char *const MONTHS[] = {
    "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
    "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"
};

// ============================================================================
// GLOBAL STATE
// ============================================================================

static Window *s_main_window;
static Layer *s_canvas_layer;
static GPoint s_center;

static GPath *s_hour_hand_path = NULL;
static GPath *s_minute_hand_path = NULL;
static GPoint s_hour_hand_points[4];
static GPoint s_minute_hand_points[4];

static GPathInfo s_hour_hand_info = { .num_points = 4, .points = s_hour_hand_points };
static GPathInfo s_minute_hand_info = { .num_points = 4, .points = s_minute_hand_points };

// -1 = no data received yet from phone.
static int s_air_temp = -999;
static int s_aare_temp = -999;

// ============================================================================
// UTILITY
// ============================================================================

static void calculate_hand_points(GPoint *points, int length, int width) {
    points[0] = GPoint(0, -length);
    points[1] = GPoint(width, -length / 3);
    points[2] = GPoint(0, length / 6);
    points[3] = GPoint(-width, -length / 3);
}

static GPoint point_on_circle(GPoint center, int32_t angle, int radius) {
    return GPoint(
        center.x + (int16_t)((sin_lookup(angle) * radius) / TRIG_MAX_RATIO),
        center.y - (int16_t)((cos_lookup(angle) * radius) / TRIG_MAX_RATIO));
}

// ============================================================================
// DRAWING: DIAL FACE
// ============================================================================

static void draw_ticks(GContext *ctx) {
    for (int i = 0; i < 12; i++) {
        int32_t angle = (i * TRIG_MAX_ANGLE) / 12;
        bool is_cardinal = (i % 3 == 0);
        int len = is_cardinal ? 14 : 7;

        GPoint outer = point_on_circle(s_center, angle, CLOCK_RADIUS);
        GPoint inner = point_on_circle(s_center, angle, CLOCK_RADIUS - len);

        graphics_context_set_stroke_color(ctx, is_cardinal ? GColorWhite : GColorDarkGray);
        graphics_context_set_stroke_width(ctx, is_cardinal ? 3 : 2);
        graphics_draw_line(ctx, inner, outer);
    }
}

static void draw_top_labels(GContext *ctx) {
    graphics_context_set_text_color(ctx, GColorLightGray);
    graphics_draw_text(ctx, "N", fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
                        GRect(s_center.x - 30, s_center.y - CLOCK_RADIUS + 10, 60, 16),
                        GTextOverflowModeFill, GTextAlignmentCenter, NULL);

    graphics_context_set_text_color(ctx, GColorDarkGray);
    graphics_draw_text(ctx, "BERN", fonts_get_system_font(FONT_KEY_GOTHIC_14),
                        GRect(s_center.x - 50, s_center.y - CLOCK_RADIUS + 28, 100, 16),
                        GTextOverflowModeFill, GTextAlignmentCenter, NULL);
}

// ============================================================================
// DRAWING: SUN ICON (air temperature)
// ============================================================================

static void draw_sun_icon(GContext *ctx, GPoint c) {
    graphics_context_set_stroke_color(ctx, GColorOrange);
    graphics_context_set_fill_color(ctx, GColorOrange);
    graphics_context_set_stroke_width(ctx, 2);

    graphics_fill_circle(ctx, c, 4);
    graphics_draw_line(ctx, GPoint(c.x, c.y - 10), GPoint(c.x, c.y - 7));
    graphics_draw_line(ctx, GPoint(c.x, c.y + 7), GPoint(c.x, c.y + 10));
    graphics_draw_line(ctx, GPoint(c.x - 10, c.y), GPoint(c.x - 7, c.y));
    graphics_draw_line(ctx, GPoint(c.x + 7, c.y), GPoint(c.x + 10, c.y));
}

// ============================================================================
// DRAWING: WAVE ICON (Aare temperature)
// ============================================================================

static void draw_wave_icon(GContext *ctx, GPoint c) {
    // Two stacked sine-ish waves approximated with short line segments
    // (no floats / no bezier primitive available on-device).
    graphics_context_set_stroke_width(ctx, 2);
    graphics_context_set_stroke_color(ctx, GColorVividCerulean);

    GPoint prev = GPoint(c.x - 10, c.y + 2);
    for (int i = 1; i <= 8; i++) {
        int32_t angle = (i * TRIG_MAX_ANGLE) / 8;
        GPoint p = GPoint(c.x - 10 + (i * 20) / 8,
                           c.y + 2 - (int16_t)((sin_lookup(angle) * 4) / TRIG_MAX_RATIO));
        graphics_draw_line(ctx, prev, p);
        prev = p;
    }

    graphics_context_set_stroke_color(ctx, GColorLiberty);
    prev = GPoint(c.x - 10, c.y - 3);
    for (int i = 1; i <= 8; i++) {
        int32_t angle = (i * TRIG_MAX_ANGLE) / 8;
        GPoint p = GPoint(c.x - 10 + (i * 20) / 8,
                           c.y - 3 - (int16_t)((sin_lookup(angle) * 3) / TRIG_MAX_RATIO));
        graphics_draw_line(ctx, prev, p);
        prev = p;
    }
}

// ============================================================================
// DRAWING: READOUTS
// ============================================================================

static void draw_readout(GContext *ctx, GPoint rc, int temp_c, bool is_air) {
    GPoint icon_c = GPoint(rc.x, rc.y - 20);
    if (is_air) {
        draw_sun_icon(ctx, icon_c);
    } else {
        draw_wave_icon(ctx, icon_c);
    }

    static char air_buf[8];
    static char aare_buf[8];
    char *buf = is_air ? air_buf : aare_buf;
    if (temp_c <= -999) {
        snprintf(buf, 8, "--\u00b0");
    } else {
        snprintf(buf, 8, "%d\u00b0", temp_c);
    }

    graphics_context_set_text_color(ctx, GColorWhite);
    graphics_draw_text(ctx, buf, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                        GRect(rc.x - 30, rc.y - 12, 60, 22),
                        GTextOverflowModeFill, GTextAlignmentCenter, NULL);

    graphics_context_set_text_color(ctx, GColorDarkGray);
    graphics_draw_text(ctx, is_air ? "AIR" : "AARE", fonts_get_system_font(FONT_KEY_GOTHIC_14),
                        GRect(rc.x - 30, rc.y + 12, 60, 16),
                        GTextOverflowModeFill, GTextAlignmentCenter, NULL);
}

// ============================================================================
// DRAWING: DATE
// ============================================================================

static void draw_date(GContext *ctx, struct tm *t) {
    static char buf[8];
    snprintf(buf, sizeof(buf), "%s %d", MONTHS[t->tm_mon], t->tm_mday);

    GRect box = GRect(s_center.x - 40, s_center.y + DATE_OFFSET_Y - 10, 80, 20);

    graphics_context_set_stroke_color(ctx, GColorDarkGray);
    graphics_context_set_stroke_width(ctx, 1);
    graphics_draw_round_rect(ctx, box, 3);

    graphics_context_set_text_color(ctx, GColorWhite);
    graphics_draw_text(ctx, buf, fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
                        GRect(box.origin.x, box.origin.y + 2, box.size.w, 16),
                        GTextOverflowModeFill, GTextAlignmentCenter, NULL);
}

// ============================================================================
// DRAWING: HANDS
// ============================================================================

static void draw_hands(GContext *ctx, struct tm *t) {
    int32_t hour_angle = ((t->tm_hour % 12) * TRIG_MAX_ANGLE / 12) +
                          (t->tm_min * TRIG_MAX_ANGLE / 12 / 60);
    int32_t minute_angle = (t->tm_min * TRIG_MAX_ANGLE) / 60;

    gpath_rotate_to(s_hour_hand_path, hour_angle);
    gpath_move_to(s_hour_hand_path, s_center);
    graphics_context_set_fill_color(ctx, GColorWhite);
    gpath_draw_filled(ctx, s_hour_hand_path);

    gpath_rotate_to(s_minute_hand_path, minute_angle);
    gpath_move_to(s_minute_hand_path, s_center);
    graphics_context_set_fill_color(ctx, GColorWhite);
    gpath_draw_filled(ctx, s_minute_hand_path);

    graphics_context_set_fill_color(ctx, GColorWhite);
    graphics_fill_circle(ctx, s_center, 4);
}

// ============================================================================
// MAIN CANVAS UPDATE
// ============================================================================

static void canvas_update_proc(Layer *layer, GContext *ctx) {
    GRect bounds = layer_get_bounds(layer);
    graphics_context_set_fill_color(ctx, GColorBlack);
    graphics_fill_rect(ctx, bounds, 0, GCornerNone);

    draw_ticks(ctx);
    draw_top_labels(ctx);

    draw_readout(ctx, GPoint(s_center.x + READOUT_OFFSET_X, s_center.y), s_air_temp, true);
    draw_readout(ctx, GPoint(s_center.x - READOUT_OFFSET_X, s_center.y), s_aare_temp, false);

    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    if (t) {
        draw_date(ctx, t);
        // Hands drawn last so they sweep on top of the readouts, matching
        // the original design's stacking order.
        draw_hands(ctx, t);
    }
}

// ============================================================================
// TIME
// ============================================================================

static void request_data(void) {
    DictionaryIterator *iter;
    if (app_message_outbox_begin(&iter) == APP_MSG_OK) {
        dict_write_uint8(iter, MESSAGE_KEY_REQUEST_DATA, 1);
        app_message_outbox_send();
    }
}

static void tick_handler(struct tm *tick_time, TimeUnits units_changed) {
    if (s_canvas_layer) {
        layer_mark_dirty(s_canvas_layer);
    }
    if (tick_time->tm_min % 30 == 0) {
        request_data();
    }
}

// ============================================================================
// APPMESSAGE (air / Aare temperature from PebbleKit JS)
// ============================================================================

static void inbox_received_callback(DictionaryIterator *iterator, void *context) {
    Tuple *air_tuple = dict_find(iterator, MESSAGE_KEY_AIR_TEMP);
    Tuple *aare_tuple = dict_find(iterator, MESSAGE_KEY_AARE_TEMP);

    bool changed = false;
    if (air_tuple) {
        s_air_temp = (int)air_tuple->value->int32;
        changed = true;
    }
    if (aare_tuple) {
        s_aare_temp = (int)aare_tuple->value->int32;
        changed = true;
    }
    if (changed && s_canvas_layer) {
        layer_mark_dirty(s_canvas_layer);
    }
}

static void inbox_dropped_callback(AppMessageResult reason, void *context) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "Message dropped: %d", reason);
}

static void outbox_failed_callback(DictionaryIterator *iterator,
                                    AppMessageResult reason, void *context) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "Outbox send failed: %d", reason);
}

// ============================================================================
// WINDOW HANDLERS
// ============================================================================

static void main_window_load(Window *window) {
    Layer *window_layer = window_get_root_layer(window);
    GRect bounds = layer_get_bounds(window_layer);
    s_center = GPoint(bounds.size.w / 2, bounds.size.h / 2);

    s_canvas_layer = layer_create(bounds);
    layer_set_update_proc(s_canvas_layer, canvas_update_proc);
    layer_add_child(window_layer, s_canvas_layer);

    calculate_hand_points(s_hour_hand_points, HOUR_HAND_LENGTH, HOUR_HAND_WIDTH);
    calculate_hand_points(s_minute_hand_points, MINUTE_HAND_LENGTH, MINUTE_HAND_WIDTH);
    s_hour_hand_path = gpath_create(&s_hour_hand_info);
    s_minute_hand_path = gpath_create(&s_minute_hand_info);
}

static void main_window_unload(Window *window) {
    if (s_hour_hand_path) {
        gpath_destroy(s_hour_hand_path);
        s_hour_hand_path = NULL;
    }
    if (s_minute_hand_path) {
        gpath_destroy(s_minute_hand_path);
        s_minute_hand_path = NULL;
    }
    if (s_canvas_layer) {
        layer_destroy(s_canvas_layer);
        s_canvas_layer = NULL;
    }
}

// ============================================================================
// APPLICATION LIFECYCLE
// ============================================================================

static void init(void) {
    s_main_window = window_create();
    window_set_background_color(s_main_window, GColorBlack);
    window_set_window_handlers(s_main_window, (WindowHandlers) {
        .load = main_window_load,
        .unload = main_window_unload
    });
    window_stack_push(s_main_window, true);

    tick_timer_service_subscribe(MINUTE_UNIT, tick_handler);

    app_message_register_inbox_received(inbox_received_callback);
    app_message_register_inbox_dropped(inbox_dropped_callback);
    app_message_register_outbox_failed(outbox_failed_callback);
    app_message_open(128, 128);

    request_data();
}

static void deinit(void) {
    tick_timer_service_unsubscribe();
    window_destroy(s_main_window);
}

int main(void) {
    init();
    app_event_loop();
    deinit();
    return 0;
}

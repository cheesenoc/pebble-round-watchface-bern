#include <pebble.h>

// ============================================================================
// CONFIGURATION
// "Twin Arc" dial for gabbro (Pebble Round 2, 260x260 round, 64-colour).
//
// Air and Aare temperature are drawn as two gauges on the rim rather than as
// digits alone: air fills the top half clockwise from 9 o'clock, water fills
// the bottom half anticlockwise from the same point, both on one shared
// scale, so the two readings can be compared by length. The digits stay
// parked at fixed spots so a glance always lands in the same place.
//
// Geometry below is in pixels relative to the dial centre and matches the
// design canvas, which is drawn on a 260x260 face centred at (130, 130).
// ============================================================================

#define ARC_OUTER_R        116    // rim gauge band spans r108..r116
#define ARC_THICKNESS        8

#define SCALE_MIN_C         -5    // both gauges share one -5..35 degC scale...
#define SCALE_MAX_C         35
#define SCALE_SWEEP_DEG    180    // ...spread over half the rim each

#define INDEX_OUTER_R       96    // the four cardinal indices
#define INDEX_INNER_R       86

#define HOUR_HAND_LENGTH    68
#define HOUR_HAND_WIDTH      5
#define MINUTE_HAND_LENGTH  92
#define MINUTE_HAND_WIDTH    4

// The label stack mirrors about the centre: index, label, readout.
#define LABEL_OFFSET_Y     -84    // BERN box top
#define READOUT_OFFSET_Y    54    // air readout at -Y, Aare readout at +Y
#define DATE_OFFSET_Y       71    // date box top

#define READOUT_GAP          8    // icon-to-digits gap within one readout
#define ICON_WIDTH          16
#define ICON_HEIGHT         12

#define NO_DATA           -999    // no reading received from the phone yet

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

static int s_air_temp = NO_DATA;
static int s_aare_temp = NO_DATA;

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

// How far round the rim a reading sits, in degrees, clamped to the scale.
static int temp_to_sweep_deg(int temp_c) {
    int sweep = ((temp_c - SCALE_MIN_C) * SCALE_SWEEP_DEG) / (SCALE_MAX_C - SCALE_MIN_C);
    if (sweep < 0) {
        sweep = 0;
    } else if (sweep > SCALE_SWEEP_DEG) {
        sweep = SCALE_SWEEP_DEG;
    }
    return sweep;
}

// ============================================================================
// DRAWING: RIM GAUGES
//
// Both gauges start at 9 o'clock (270 degrees, with 0 at 12 and angles
// increasing clockwise). Air runs clockwise over the top to 3 o'clock; water
// runs anticlockwise under the bottom to the same place. graphics_fill_radial
// always sweeps clockwise, so the water arc is expressed as (270 - sweep)
// through 6 o'clock back to 270.
// ============================================================================

static void draw_gauge(GContext *ctx, int temp_c, bool is_air) {
    GRect box = GRect(s_center.x - ARC_OUTER_R, s_center.y - ARC_OUTER_R,
                      ARC_OUTER_R * 2, ARC_OUTER_R * 2);

    graphics_context_set_fill_color(ctx, is_air ? GColorFromRGB(0x55, 0x55, 0x00)
                                                : GColorFromRGB(0x00, 0x55, 0x55));
    graphics_fill_radial(ctx, box, GOvalScaleModeFitCircle, ARC_THICKNESS,
                         DEG_TO_TRIGANGLE(is_air ? 270 : 90),
                         DEG_TO_TRIGANGLE(is_air ? 450 : 270));

    if (temp_c <= NO_DATA) {
        return;   // no reading yet: the empty track is the whole message
    }

    int sweep = temp_to_sweep_deg(temp_c);
    graphics_context_set_fill_color(ctx, is_air ? GColorOrange : GColorVividCerulean);
    graphics_fill_radial(ctx, box, GOvalScaleModeFitCircle, ARC_THICKNESS,
                         DEG_TO_TRIGANGLE(is_air ? 270 : 270 - sweep),
                         DEG_TO_TRIGANGLE(is_air ? 270 + sweep : 270));
}

static void draw_indices(GContext *ctx) {
    graphics_context_set_stroke_color(ctx, GColorWhite);
    graphics_context_set_stroke_width(ctx, 2);

    for (int i = 0; i < 4; i++) {
        int32_t angle = (i * TRIG_MAX_ANGLE) / 4;
        graphics_draw_line(ctx, point_on_circle(s_center, angle, INDEX_INNER_R),
                                point_on_circle(s_center, angle, INDEX_OUTER_R));
    }
}

// ============================================================================
// DRAWING: READOUT ICONS
// ============================================================================

static void draw_sun_icon(GContext *ctx, GPoint c) {
    graphics_context_set_stroke_color(ctx, GColorOrange);
    graphics_context_set_fill_color(ctx, GColorOrange);
    graphics_context_set_stroke_width(ctx, 2);

    graphics_fill_circle(ctx, c, 3);
    graphics_draw_line(ctx, GPoint(c.x, c.y - 7), GPoint(c.x, c.y - 5));
    graphics_draw_line(ctx, GPoint(c.x, c.y + 5), GPoint(c.x, c.y + 7));
    graphics_draw_line(ctx, GPoint(c.x - 7, c.y), GPoint(c.x - 5, c.y));
    graphics_draw_line(ctx, GPoint(c.x + 5, c.y), GPoint(c.x + 7, c.y));
}

static void draw_wave_icon(GContext *ctx, GPoint c) {
    // Two stacked sine-ish waves approximated with short line segments
    // (no floats / no bezier primitive available on-device).
    graphics_context_set_stroke_width(ctx, 2);

    for (int pass = 0; pass < 2; pass++) {
        graphics_context_set_stroke_color(ctx, pass == 0 ? GColorVividCerulean : GColorLiberty);
        int baseline = c.y + (pass == 0 ? 3 : -2);

        GPoint prev = GPoint(c.x - 10, baseline);
        for (int i = 1; i <= 8; i++) {
            int32_t angle = (i * TRIG_MAX_ANGLE) / 8;
            GPoint p = GPoint(c.x - 10 + (i * 20) / 8,
                              baseline - (int16_t)((sin_lookup(angle) * 2) / TRIG_MAX_RATIO));
            graphics_draw_line(ctx, prev, p);
            prev = p;
        }
    }
}

// ============================================================================
// DRAWING: READOUTS
//
// Icon and digits are laid out as one group and centred together, so a
// two-character reading and a three-character one both sit on the dial's
// vertical axis rather than drifting sideways.
// ============================================================================

static void draw_readout(GContext *ctx, int offset_y, int temp_c, bool is_air) {
    static char air_buf[8];
    static char aare_buf[8];
    char *buf = is_air ? air_buf : aare_buf;

    if (temp_c <= NO_DATA) {
        snprintf(buf, 8, "--°");
    } else {
        snprintf(buf, 8, "%d°", temp_c);
    }

    GFont font = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);
    GSize text_size = graphics_text_layout_get_content_size(
        buf, font, GRect(0, 0, 100, 24), GTextOverflowModeFill, GTextAlignmentLeft);

    int left = s_center.x - (ICON_WIDTH + READOUT_GAP + text_size.w) / 2;
    int cy = s_center.y + offset_y;

    GPoint icon_c = GPoint(left + ICON_WIDTH / 2, cy);
    if (is_air) {
        draw_sun_icon(ctx, icon_c);
    } else {
        draw_wave_icon(ctx, icon_c);
    }

    // -14 rather than -12: graphics_draw_text hangs the glyphs off the top of
    // the box by the font's ascent, so the box sits slightly high to land the
    // digits on the readout's centre line.
    graphics_context_set_text_color(ctx, temp_c <= NO_DATA ? GColorDarkGray : GColorWhite);
    graphics_draw_text(ctx, buf, font,
                       GRect(left + ICON_WIDTH + READOUT_GAP, cy - 14, text_size.w + 4, 24),
                       GTextOverflowModeFill, GTextAlignmentLeft, NULL);
}

// ============================================================================
// DRAWING: LABELS
// ============================================================================

static void draw_labels(GContext *ctx, struct tm *t) {
    static char date_buf[12];
    snprintf(date_buf, sizeof(date_buf), "%s %d", MONTHS[t->tm_mon], t->tm_mday);

    GFont font = fonts_get_system_font(FONT_KEY_GOTHIC_14);
    graphics_context_set_text_color(ctx, GColorDarkGray);

    graphics_draw_text(ctx, "BERN", font,
                       GRect(s_center.x - 50, s_center.y + LABEL_OFFSET_Y, 100, 16),
                       GTextOverflowModeFill, GTextAlignmentCenter, NULL);

    graphics_draw_text(ctx, date_buf, font,
                       GRect(s_center.x - 50, s_center.y + DATE_OFFSET_Y, 100, 16),
                       GTextOverflowModeFill, GTextAlignmentCenter, NULL);
}

// ============================================================================
// DRAWING: HANDS
// ============================================================================

static void draw_hand(GContext *ctx, GPath *path, int32_t angle) {
    gpath_rotate_to(path, angle);
    gpath_move_to(path, s_center);

    // Black casing first, so a hand stays legible where it crosses a readout
    // or a rim gauge.
    graphics_context_set_fill_color(ctx, GColorBlack);
    graphics_context_set_stroke_color(ctx, GColorBlack);
    graphics_context_set_stroke_width(ctx, 3);
    gpath_draw_filled(ctx, path);
    gpath_draw_outline(ctx, path);

    graphics_context_set_fill_color(ctx, GColorWhite);
    gpath_draw_filled(ctx, path);
}

static void draw_hands(GContext *ctx, struct tm *t) {
    int32_t hour_angle = ((t->tm_hour % 12) * TRIG_MAX_ANGLE / 12) +
                          (t->tm_min * TRIG_MAX_ANGLE / 12 / 60);
    int32_t minute_angle = (t->tm_min * TRIG_MAX_ANGLE) / 60;

    draw_hand(ctx, s_hour_hand_path, hour_angle);
    draw_hand(ctx, s_minute_hand_path, minute_angle);

    graphics_context_set_fill_color(ctx, GColorBlack);
    graphics_fill_circle(ctx, s_center, 6);
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

    draw_gauge(ctx, s_air_temp, true);
    draw_gauge(ctx, s_aare_temp, false);
    draw_indices(ctx);

    draw_readout(ctx, -READOUT_OFFSET_Y, s_air_temp, true);
    draw_readout(ctx, READOUT_OFFSET_Y, s_aare_temp, false);

    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    if (t) {
        draw_labels(ctx, t);
        // Hands drawn last so they sweep on top of the readouts.
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

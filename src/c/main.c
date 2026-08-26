// main.c — entry point of the watchface.
//
// Three screens (Clock / Astro / Stopwatch), a data packet received from the
// phone over AppMessage, an inactivity return from Astro to Clock, and a forced
// refresh request.
//
// INPUT MODEL — the "contextual tap": the only gesture is a strike on the case,
// and its meaning depends on the current screen and sub-state. Where the gesture
// comes from is hidden inside input.c; touch is unavailable, as the firmware
// does not deliver touches to watchfaces.
//
// A single Layer with one update_proc draws the current screen; state lives in
// statics.

#include <pebble.h>
#include "packet.h"
#include "settings.h"
#include "timefmt.h"
#include "ring.h"
#include "input.h"
#include "geom.h"
#include "icons.h"

// --- Screens ---
typedef enum {
  MODE_CLOCK = 0,
  MODE_ASTRO,
  MODE_STOPWATCH,
} Mode;

// Stopwatch sub-state
typedef enum {
  SW_IDLE = 0,
  SW_RUN,
  SW_STOPPED
} SwState;

// --- Constants ---
#define STALE_THRESHOLD_SEC (3 * 60 * 60) // data older than this is stale
#define RING_THICKNESS 8
#define RING_MARGIN 4
#define REFRESH_TIMEOUT_MS 15000
#define SW_TICK_MS 100

// The key number IS the DataPacket schema version: an older, shorter blob must
// never be read into a longer struct or the fields shift. Every layout change
// therefore takes the next free number. The price is losing the cache once after
// an upgrade.
#define PACKET_PERSIST_KEY 5

// Marker for the light window we have already buzzed about. It lives in persist
// rather than in a static alone: switching watchfaces or rebooting restarts the
// app, and without the marker the notification would fire again for the same
// window.
#define NOTIFY_PERSIST_KEY 8

// --- Global state ---
static Window *s_window;
static Layer *s_canvas;

static Mode s_mode = MODE_CLOCK;
static SwState s_sw_state = SW_IDLE;

// Cached local time, filled in tick_handler from the firmware's tick_time.
// Drawing takes the time ONLY from here: on this device localtime() called
// inside update_proc returned a frozen value.
static struct tm s_now_tm;

static DataPacket s_packet;
static Settings s_settings;
static bool s_connected = true;

// Forced refresh
static bool s_refreshing = false;
static uint8_t s_refresh_tick = 0;
static AppTimer *s_refresh_timeout = NULL;

// Auto-return Astro→Clock
static AppTimer *s_astro_timer = NULL;

// Auto-exit Stopwatch(idle)→Clock. Exists ONLY in SW_IDLE; cleared on Start.
static AppTimer *s_sw_idle_timer = NULL;

// Stopwatch. PURE TICK COUNTING: the accumulator gains SW_TICK_MS on every timer
// tick, and time_ms() is never consulted — its sub-second part is unreliable in
// the emulator and produced jumping seconds and stuttering tenths.
static uint64_t s_sw_elapsed_ms = 0; // accumulated milliseconds, for drawing
static AppTimer *s_sw_ticker = NULL;

// forward
static void request_refresh(void);
static void cancel_astro_timer(void);
static void schedule_astro_timer(void);
static void sw_start_ticker(void);
static void sw_stop_ticker(void);
static void cancel_sw_idle_timer(void);
static void schedule_sw_idle_timer(void);
static uint32_t light_window_start(void);

// --- Packet persistence ---
static void load_cached_packet(void) {
  packet_init_empty(&s_packet);
  if (persist_exists(PACKET_PERSIST_KEY)) {
    persist_read_data(PACKET_PERSIST_KEY, &s_packet, sizeof(DataPacket));
  }
}

static void save_packet(void) {
  persist_write_data(PACKET_PERSIST_KEY, &s_packet, sizeof(DataPacket));
}

// --- Point on a circle (angle 0 = top, clockwise) ---
static GPoint point_on_circle(int cx, int cy, int radius, int32_t angle_deg) {
  int32_t a = (angle_deg * TRIG_MAX_ANGLE) / 360;
  int32_t sn = sin_lookup(a);
  int32_t cs = cos_lookup(a);
  // 0° at the top: x = cx + r*sin, y = cy - r*cos
  GPoint p;
  p.x = cx + (int)((sn * radius) / TRIG_MAX_RATIO);
  p.y = cy - (int)((cs * radius) / TRIG_MAX_RATIO);
  return p;
}

// How one day phase is drawn. On a colour display the phases differ ONLY by
// colour (every other field is identical); on a 1-bit one, only by shape.
typedef struct {
  GColor color;
  uint8_t inset;     // inset from the outer radius of the ring
  uint8_t thickness; // 0 = do not draw this phase at all
  // Dash pitch in TENTHS OF A PIXEL ALONG THE ARC; 0 = a solid band. NOT in
  // degrees: the same 4° is ≈4.7 px of arc at R=68 (diorite) and ≈8.8 px at
  // R=126 (gabbro), so a ring dashed on one platform comes out striped on
  // another. Arc length keeps the gap identical across all six geometries
  // without a per-radius table.
  uint16_t step_tenths_px;
} RingStyle;

// Arc length in pixels → number of dashes. Integer arithmetic: 2πR ≈ 628·R/100,
// hence N = (628·R·Δangle) / (3600·pitch_in_tenths). The numerator peaks at
// 628·126·360 ≈ 28.5 million, which fits int32 with room to spare.
static int ring_dash_count(int radius, int32_t span_deg, uint16_t step_tenths_px) {
  if (step_tenths_px == 0 || span_deg <= 0 || radius <= 0) return 0;
  int n = (int)(((int32_t)628 * radius * span_deg) / (3600 * (int32_t)step_tenths_px));
  return n > 0 ? n : 1;
}

static void draw_ring_arc(GContext *ctx, int cx, int cy, int outer_r,
                          const RingStyle *st, int32_t from_deg, int32_t to_deg) {
  if (st->thickness == 0 || to_deg <= from_deg) return;
  int r_out = outer_r - st->inset;

  // A solid phase is filled as a whole ring sector rather than as overlapping
  // radial strokes: the SDK primitive leaves no gaps at any radius, whereas a
  // band imitated with a fine dash pitch comes out striped on large screens.
  if (st->step_tenths_px == 0) {
    graphics_context_set_fill_color(ctx, st->color);
    graphics_fill_radial(ctx, GRect(cx - r_out, cy - r_out, r_out * 2, r_out * 2),
                         GOvalScaleModeFitCircle, st->thickness,
                         DEG_TO_TRIGANGLE(from_deg), DEG_TO_TRIGANGLE(to_deg));
    return;
  }

  int r_in = r_out - st->thickness;
  int32_t span = to_deg - from_deg;
  int n = ring_dash_count(r_out, span, st->step_tenths_px);
  graphics_context_set_stroke_color(ctx, st->color);
  graphics_context_set_stroke_width(ctx, 2);
  for (int i = 0; i <= n; i++) {
    int32_t a = from_deg + (span * i) / n;
    GPoint p1 = point_on_circle(cx, cy, r_in, a);
    GPoint p2 = point_on_circle(cx, cy, r_out, a);
    graphics_draw_line(ctx, p1, p2);
  }
}

// An arc may cross midnight (from > to), in which case it is cut in two.
static void draw_ring_span(GContext *ctx, int cx, int cy, int outer_r,
                           const RingStyle *st, int32_t from_deg, int32_t to_deg) {
  if (from_deg <= to_deg) {
    draw_ring_arc(ctx, cx, cy, outer_r, st, from_deg, to_deg);
  } else {
    draw_ring_arc(ctx, cx, cy, outer_r, st, from_deg, 360);
    draw_ring_arc(ctx, cx, cy, outer_r, st, 0, to_deg);
  }
}

#ifdef PBL_BW
// On a 1-bit display colour distinguishes NOTHING — GColorDarkGray collapses to
// black and everything else to white. The phases are separated by TWO
// independent cues instead:
//
//   THICKNESS = how much the phase matters   full 8 px = shooting windows,
//                                            thin 3 px = backdrop of the day
//   SOLID or DASHED = how bright it is       solid = golden hour, daytime
//                                            dashed = blue hour, twilight
//   astronomical night — empty: the gap in the ring IS the darkest sky.
//
// Two cues of two values, not one scale of five: five thickness steps are
// indistinguishable on an 8 px ring and five dash densities blur into noise.
static RingStyle ring_style_for(RingArcKind kind) {
  switch (kind) {
    case RING_ARC_GOLDEN:      return (RingStyle){ GColorWhite, 0, RING_THICKNESS, 0 };
    case RING_ARC_BLUE:        return (RingStyle){ GColorWhite, 0, RING_THICKNESS, 47 };
    case RING_ARC_DAY:         return (RingStyle){ GColorWhite, 0, 3, 0 };
    case RING_ARC_ASTRO_NIGHT: return (RingStyle){ GColorBlack, 0, 0, 0 };
    case RING_ARC_TWILIGHT:
    default:                   return (RingStyle){ GColorWhite, 0, 3, 71 };
  }
}
#else
// Arc brightness = sky brightness: astronomical night #000055 → twilight #550055
// → blue hour #0055FF → daytime #55AAFF → golden hour #FFAA00. Every phase is
// solid and full thickness — on colour the hue carries the difference by itself.
//
// COMPARE PALETTE VALUES, NOT NAMES when changing these: GColorDarkGray
// (#555555) is lighter than GColorOxfordBlue (#000055), so a "dark grey" night
// comes out brighter than the twilight preceding it and turns the meaning of the
// ring inside out. Pure black is no good either — the arc merges with the
// background and the ring looks broken.
//
// Twilight is PURPLE rather than a darker blue because brightness alone cannot
// separate it from night — #000055 and #0000AA look the same on an 8 px ring,
// while a different hue reads instantly.
static RingStyle ring_style_for(RingArcKind kind) {
  switch (kind) {
    case RING_ARC_GOLDEN:      return (RingStyle){ GColorChromeYellow, 0, RING_THICKNESS, 0 };
    case RING_ARC_DAY:         return (RingStyle){ GColorPictonBlue, 0, RING_THICKNESS, 0 };
    case RING_ARC_BLUE:        return (RingStyle){ GColorBlueMoon, 0, RING_THICKNESS, 0 };
    case RING_ARC_ASTRO_NIGHT: return (RingStyle){ GColorOxfordBlue, 0, RING_THICKNESS, 0 };
    case RING_ARC_TWILIGHT:
    default:                   return (RingStyle){ GColorImperialPurple, 0, RING_THICKNESS, 0 };
  }
}
#endif

// --- Marker for the next light window ---
//
// A permanent element of the ring, drawn whether or not notifications are on. It
// differs from the "now" marker by three cues at once: orientation (tangential
// against radial), side (inside the ring against outside) and not crossing the
// ring — enough to stay distinct on a 1-bit display too.
//
// Its length is in PIXELS OF ARC rather than degrees: otherwise the marker at
// R=68 would be twice as long as at R=126 while meaning the same thing.
#define RING_PIP_ARC_PX 10
#define RING_PIP_THICKNESS 3
#define RING_PIP_GAP 4 // gap between the inner edge of the ring and the marker

static void draw_window_pip(GContext *ctx, int cx, int cy, int outer_r) {
  uint32_t start = light_window_start();
  if (start == 0) return;
  int32_t angle_deg = ring_ts_to_angle(start);
  if (angle_deg < 0) return;

  int r_out = outer_r - RING_THICKNESS - RING_PIP_GAP;
  if (r_out <= RING_PIP_THICKNESS) return;

  // Half-length in trig units: (5 px / 2πr) · TRIG_MAX_ANGLE. Computed in those
  // units rather than in degrees — at large radii the marker spans less than 3°,
  // and rounding to a whole degree changes its length by half again.
  int32_t half = (RING_PIP_ARC_PX / 2) * TRIG_MAX_ANGLE * 100 / (628 * r_out);
  int32_t center = DEG_TO_TRIGANGLE(angle_deg);
  int32_t a0 = center - half, a1 = center + half;
  if (a0 < 0) { a0 += TRIG_MAX_ANGLE; a1 += TRIG_MAX_ANGLE; }

  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_fill_radial(ctx, GRect(cx - r_out, cy - r_out, r_out * 2, r_out * 2),
                       GOvalScaleModeFitCircle, RING_PIP_THICKNESS, a0, a1);
}

// --- Day-scale ticks: 00 / 06 / 12 / 18 ---
//
// The circle is a DAY, not 12 hours: 0° at the top = midnight, 180° = noon. The
// marker therefore travels at half the speed of an hour hand, and without
// reference points the scale reads as "the hand is lying".
//
// The ticks live INSIDE the ring, in the gap before the window-marker band,
// rather than on top of the arcs. Midnight always falls on the darkest phase, so
// a white tick over an arc vanishes on 1-bit (the golden hour is a solid white
// block) and a black one vanishes on colour (night #000055 against a black
// background). In the gap the background is black whatever the data, so one
// design works on all six geometries.
//
// LARGE screens only (outer R ≥ 90: emery 96, gabbro 126). At R = 68 the space
// inside the ring is 120 px across, and a fourth white element next to the now
// marker, the window pip and the status icon reads as noise.
//
// Length and thickness scale as a FRACTION OF THE RADIUS: the same 3 px reads as
// a division at R = 96 and as a dot at R = 126. The reference geometry is emery.
#define RING_TICK_REF_R 96     // emery
#define RING_TICK_REF_LEN 4    // length along the radius at RING_TICK_REF_R, px
#define RING_TICK_REF_WIDTH 2  // stroke width at RING_TICK_REF_R, px
#define RING_TICK_MIN_R 90     // outer radius below which ticks are dropped

static int ring_tick_scaled(int ref_px, int outer_r) {
  int v = (ref_px * outer_r + RING_TICK_REF_R / 2) / RING_TICK_REF_R;
  return v < 1 ? 1 : v;
}

static void draw_ring_ticks(GContext *ctx, int cx, int cy, int outer_r) {
  if (outer_r < RING_TICK_MIN_R) return;
  int len = ring_tick_scaled(RING_TICK_REF_LEN, outer_r);
  if (len > RING_PIP_GAP) len = RING_PIP_GAP; // touch the marker band, never enter it
  int r_in = outer_r - RING_THICKNESS;
  if (r_in <= len) return;
  graphics_context_set_stroke_color(ctx, GColorWhite);
  graphics_context_set_stroke_width(ctx,
                                    ring_tick_scaled(RING_TICK_REF_WIDTH, outer_r));
  for (int32_t a = 0; a < 360; a += 90) {
    graphics_draw_line(ctx, point_on_circle(cx, cy, r_in - len, a),
                       point_on_circle(cx, cy, r_in, a));
  }
}

static void draw_ring(GContext *ctx, int cx, int cy, int outer_r,
                      const struct tm *now_tm) {
  // The base ring IS TWILIGHT: what shows through between the blue hour and
  // astronomical night. Phase arcs are drawn on top.
  RingStyle twilight = ring_style_for(RING_ARC_TWILIGHT);
  draw_ring_arc(ctx, cx, cy, outer_r, &twilight, 0, 360);

  if (packet_has_sun_data(&s_packet)) {
    RingArc arcs[RING_MAX_ARCS];
    int n = ring_build_arcs(&s_packet, arcs, RING_MAX_ARCS);
    for (int i = 0; i < n; i++) {
      RingStyle st = ring_style_for(arcs[i].kind);
#ifdef PBL_BW
      // On a 1-bit display the arcs are NOT full thickness (daytime and
      // twilight are a 3 px strip), so the twilight dashes would show through
      // underneath. Clear the sector first with a solid fill, leaving no gaps at
      // any radius.
      RingStyle erase = { GColorBlack, 0, RING_THICKNESS, 0 };
      draw_ring_span(ctx, cx, cy, outer_r, &erase,
                     arcs[i].from_deg, arcs[i].to_deg);
#endif
      draw_ring_span(ctx, cx, cy, outer_r, &st, arcs[i].from_deg, arcs[i].to_deg);
    }
    draw_window_pip(ctx, cx, cy, outer_r);
  }

  // Scale ticks sit outside the data block: the day grid exists without a packet.
  draw_ring_ticks(ctx, cx, cy, outer_r);

  // Marker for the current time.
  int32_t now_angle = ring_tm_to_angle(now_tm);
  GPoint p_out = point_on_circle(cx, cy, outer_r + 1, now_angle);
  GPoint p_in = point_on_circle(cx, cy, outer_r - RING_THICKNESS - 3, now_angle);
  graphics_context_set_stroke_color(ctx, GColorWhite);
  graphics_context_set_stroke_width(ctx, 3);
  graphics_draw_line(ctx, p_in, p_out);
}

// --- Inscribed-circle geometry ---
//
// Both layouts (Clock and Astro) derive a row's width from the CHORD of the
// circle at that row's own height, not from one shared inscribed column —
// otherwise the outermost rows of a block dictate the width of the central ones,
// which have twice the room. isqrt32 and chord_half_at live in geom.c.

// Row width from the ACTUAL font, never from a "point size × factor" estimate:
// the system GOTHIC and LECO have proportions of their own, and on basalt the
// layout decision has only 8 px of slack — less than the error of such an
// estimate, so the "pair / stack" choice would hinge on it.
static int text_width(const char *s, GFont font) {
  return graphics_text_layout_get_content_size(
             s, font, GRect(0, 0, 400, 200), GTextOverflowModeFill,
             GTextAlignmentLeft).w;
}

// Text CENTRED vertically within its band. graphics_draw_text aligns to the TOP
// of its rectangle, and a band's height comes from the row pitch rather than the
// point size, so without this the spare height all falls below the glyphs.
static void draw_text_vcenter(GContext *ctx, const char *s, GFont font,
                              GRect box, GTextAlignment align) {
  int th = graphics_text_layout_get_content_size(s, font, box,
                                                 GTextOverflowModeFill, align).h;
  int dy = (box.size.h - th) / 2;
  if (dy > 0) { box.origin.y += dy; box.size.h -= dy; }
  graphics_draw_text(ctx, s, font, box, GTextOverflowModeFill, align, NULL);
}

// An "icon + value" row in a single colour (the weather figures on Clock).
//
// The GROUP is aligned as a whole rather than the text apart from the icon: with
// right alignment (the left half-row) the icon would otherwise stay at the left
// edge of the band, separated from its value by the band's full width.
static void draw_icon_text(GContext *ctx, IconGlyph g, const char *s, GFont font,
                           GRect box, GTextAlignment align, int icon_size,
                           GColor color) {
  int adv = ICON_ADVANCE(icon_size);
  int group = adv + text_width(s, font);
  int x = box.origin.x;
  if (align == GTextAlignmentCenter)     x += (box.size.w - group) / 2;
  else if (align == GTextAlignmentRight) x += box.size.w - group;
  if (x < box.origin.x) x = box.origin.x;

  // The icon sits on the CAP-HEIGHT centre of the value, not on the centre of
  // the band — a geometrically centred icon hangs 3 px too high (icon_text_dy).
  icon_draw(ctx, g,
            GPoint(x, box.origin.y + (box.size.h - icon_size) / 2
                          + icon_text_dy(g, icon_size)),
            icon_size, color);
  graphics_context_set_text_color(ctx, color);
  draw_text_vcenter(ctx, s, font,
                    GRect(x + adv, box.origin.y,
                          box.origin.x + box.size.w - x - adv, box.size.h),
                    GTextAlignmentLeft);
}

// --- Weather figures on the Clock screen ---
//
// One scale for all three figures: green — conditions help, yellow — workable,
// grey — in the way, dark grey — no data. On 1-bit displays GColorDarkGray
// (#555555) turns BLACK, so "no data" would be invisible against the background;
// both greys therefore fall back to white in monochrome, where the "--" dashes
// carry the absence instead.
#define WX_GOOD PBL_IF_COLOR_ELSE(GColorGreen, GColorWhite)
#define WX_FAIR PBL_IF_COLOR_ELSE(GColorYellow, GColorWhite)
#define WX_POOR PBL_IF_COLOR_ELSE(GColorLightGray, GColorWhite)
#define WX_NONE PBL_IF_COLOR_ELSE(GColorDarkGray, GColorWhite)

// Thresholds in shooting terms rather than wind force as such: below 3 m/s
// foliage is nearly still and a long exposure stays clean, from 8 m/s the tripod
// starts to shake.
static GColor wx_wind_color(int wind_ms) {
  if (wind_ms < 0) return WX_NONE;
  if (wind_ms < 3) return WX_GOOD;
  if (wind_ms < 8) return WX_FAIR;
  return WX_POOR;
}

// Above 20 km the horizon is clear, below 10 km haze eats contrast at distance.
// The same number is the upper bound of what is DISPLAYED (wx_format_visibility)
// — the point beyond which the digits stop meaning anything.
#define WX_VIS_CLEAR_M 20000

static GColor wx_visibility_color(int32_t vis_m) {
  if (vis_m < 0) return WX_NONE;
  if (vis_m >= WX_VIS_CLEAR_M) return WX_GOOD;
  if (vis_m >= 10000) return WX_FAIR;
  return WX_POOR;
}

// Imperial units are derived HERE, at draw time (see packet.h). The colour
// thresholds above compare the original SI values and do not depend on the unit.
//
// Integer arithmetic: 1 m/s = 2.23694 mph → ×2237/1000; 1 mile = 1609.344 m.
static void wx_format_wind(char *buf, size_t n, int wind_ms, bool imperial) {
  if (wind_ms < 0) {
    snprintf(buf, n, imperial ? "-- mph" : "-- m/s");
    return;
  }
  int v = imperial ? (int)((wind_ms * 2237 + 500) / 1000) : wind_ms;
  snprintf(buf, n, "%d %s", v, imperial ? "mph" : "m/s");
}

// Above WX_VIS_CLEAR_M we print "20+ km" instead of a number: past that point
// the figure is not a measurement but a model's ceiling (GFS tops out at
// 24 140 m), so it only tells you which model answered. Below 10 units we show
// tenths — in fog the difference between 0.4 and 4 km is decisive, and whole
// kilometres would collapse it into "0 km".
static void wx_format_visibility(char *buf, size_t n, int32_t vis_m,
                                 bool imperial) {
  const char *unit = imperial ? "mi" : "km";
  if (vis_m < 0) {
    snprintf(buf, n, "-- %s", unit);
    return;
  }
  if (vis_m >= WX_VIS_CLEAR_M) {
    // The threshold is in metres, so in miles it lands on 12 rather than a round
    // 20. "20+ km" and "12+ mi" are the same state — the boundary at which the
    // row turns green.
    snprintf(buf, n, "%d+ %s", imperial ? (int)(WX_VIS_CLEAR_M / 1609)
                                        : (int)(WX_VIS_CLEAR_M / 1000), unit);
    return;
  }
  // Computed in TENTHS of the chosen unit: one rounding branch for both formats
  // instead of two separate expressions with different divisors.
  int32_t tenths = imperial ? ((vis_m * 10 + 804) / 1609)   // + half a mile to round
                            : ((vis_m + 50) / 100);
  if (tenths >= 100) {
    snprintf(buf, n, "%d %s", (int)(tenths / 10), unit);
  } else {
    snprintf(buf, n, "%d.%d %s", (int)(tenths / 10), (int)(tenths % 10), unit);
  }
}

// --- Clock screen: layout ---
//
// Ladder of time point sizes. The steps are REAL system fonts only: an arbitrary
// size cannot be rendered here.
// The names do not map onto the nominal sizes one to one: 38/36/32/20 exist only
// as _BOLD_NUMBERS and 28 as _28_LIGHT_NUMBERS. There are no steps in between.
typedef struct { uint8_t size; const char *key; } TimeStep;
static const TimeStep TIME_LADDER[] = {
  { 49, FONT_KEY_ROBOTO_BOLD_SUBSET_49 },
  { 42, FONT_KEY_LECO_42_NUMBERS },
  { 38, FONT_KEY_LECO_38_BOLD_NUMBERS },
  { 36, FONT_KEY_LECO_36_BOLD_NUMBERS },
  { 32, FONT_KEY_LECO_32_BOLD_NUMBERS },
  { 28, FONT_KEY_LECO_28_LIGHT_NUMBERS },
  { 20, FONT_KEY_LECO_20_BOLD_NUMBERS },
};
#define TIME_LADDER_N (sizeof(TIME_LADDER) / sizeof(TIME_LADDER[0]))

// The size is chosen for the WIDEST string of the format, not the current one:
// otherwise a 12-hour watchface would resize the time at 10:00 and back at 13:00.
#define TIME_WIDEST "88:88"
// The widest weather string is "88.8 mi" (7 characters). Wind is never longer
// than six; visibility with a tenth is exactly seven.
#define WX_WIDEST "88.8 mi"
// The widest cloud string is "100%"; the "-- %" dashes are narrower.
#define CLOUD_WIDEST "100%"

// Steps for the weather rows. The icon fit checks measure against them too, so
// the point size is needed as a number and not only as a font key.
static const TimeStep WX_LADDER[] = {
  { 18, FONT_KEY_GOTHIC_18 },
  { 14, FONT_KEY_GOTHIC_14 },
};
#define WX_LADDER_N (sizeof(WX_LADDER) / sizeof(WX_LADDER[0]))

typedef struct {
  GFont time_font;  int time_h, top;
  GFont cloud_font; int cloud_h, cloud_fs;
  GFont wx_font;    int wx_h, wx_fs;
  bool cloud_icon, wx_icon;
  int ampm_y, ampm_x, ampm_w;
  bool show_wx, stack;
  int wx_y, vis_y;
  int wind_x, wind_w; GTextAlignment wind_align;
  int vis_x,  vis_w;  GTextAlignment vis_align;
  int status_y;
} ClockLayout;

static ClockLayout clock_layout(GRect bounds) {
  ClockLayout L;
  int w = bounds.size.w, bh = bounds.size.h;
  int cx = w / 2, cy = bh / 2;
  bool big = (bh >= 200);

  int r0 = (cx < cy ? cx : cy) - RING_MARGIN;
  // The time is measured not from the inner edge of the RING but from the inner
  // edge of the WINDOW-MARKER band: margin + ring + gap + marker thickness + air.
  // Otherwise the digits grow straight into the marker band, and with a window
  // near 295° the marker reads as an apostrophe in front of the time.
  int inner_r0 = r0 - RING_THICKNESS - RING_PIP_GAP - RING_PIP_THICKNESS - 4;

  L.time_font = fonts_get_system_font(TIME_LADDER[TIME_LADDER_N - 1].key);
  int tf = TIME_LADDER[TIME_LADDER_N - 1].size;
  for (unsigned i = 0; i < TIME_LADDER_N; i++) {
    // Each step is checked against ITS OWN glyph height (≈0.72 of the size): a
    // large font narrows its own chord, and measuring every step at one height
    // would understate the upper ones.
    int half_cap = (36 * TIME_LADDER[i].size) / 100;
    int32_t under = (int32_t)inner_r0 * inner_r0 - (int32_t)half_cap * half_cap;
    if (under < 1) under = 1;
    int chord = 2 * (int)isqrt32(under) - 4;
    GFont f = fonts_get_system_font(TIME_LADDER[i].key);
    if (text_width(TIME_WIDEST, f) <= chord) {
      L.time_font = f;
      tf = TIME_LADDER[i].size;
      break;
    }
  }
  L.time_h = (tf >= 49) ? 54 : 46;
  L.cloud_h = 22;
  L.wx_h = big ? 22 : 18;

  // A row's width is the chord at the INNER edge of the ring, not at the marker
  // band: the marker is a tangential stroke rather than a full ring, and
  // measuring horizontal rows from it would cost a third of the width.
  int ring_in = r0 - RING_THICKNESS - 1;

  // On small canvases the 18 step is not considered at all: the supporting
  // figures must not match the size of the cloud value that drives the decision.
  unsigned wx_first = big ? 0 : WX_LADDER_N - 1;

  // The wind + visibility layout is chosen BEFORE the block is laid out, from
  // the chord of the single-row variant: otherwise the decision would depend on
  // the stack, which itself depends on the decision. Two layouts are possible —
  // a pair side by side, or two full-width rows.
  int probe_top = cy - (L.time_h + L.cloud_h + L.wx_h) / 2;
  int probe_y = probe_top + L.time_h + L.cloud_h;
  int probe_room = chord_half_at(cx, cy, ring_in, probe_y, L.wx_h) - 3;

  int stack_top = cy - (L.time_h + L.cloud_h + L.wx_h * 2) / 2;
  int stack_wx_y = stack_top + L.time_h + L.cloud_h;
  int stack_vis_y = stack_wx_y + L.wx_h;

  // THE UNIT OUTRANKS THE ICON: an icon is only placed alongside the full
  // "88.8 mi" string, so a bare "2" never appears. Hence the probing order:
  // pair with icon → stack with icon → pair → stack.
  int chosen = -1;
  L.stack = false;
  L.wx_icon = false;
  for (int pass = 0; pass < 4 && chosen < 0; pass++) {
    bool with_icon = (pass == 0 || pass == 1);
    bool as_stack = (pass == 1 || pass == 3);
    for (unsigned i = wx_first; i < WX_LADDER_N && chosen < 0; i++) {
      int need = text_width(WX_WIDEST, fonts_get_system_font(WX_LADDER[i].key));
      if (with_icon) need += ICON_ADVANCE(WX_LADDER[i].size);
      if (!as_stack) {
        if (need <= probe_room) chosen = (int)i;
      } else {
        // Checked VERTICALLY as well: on cramped canvases the second row falls
        // past the bottom of the circle, where the chord degenerates to a point.
        if (2 * chord_half_at(cx, cy, ring_in, stack_vis_y, L.wx_h) >= need &&
            2 * chord_half_at(cx, cy, ring_in, stack_wx_y, L.wx_h) >= need) {
          chosen = (int)i;
        }
      }
      if (chosen >= 0) { L.stack = as_stack; L.wx_icon = with_icon; }
    }
  }
  L.show_wx = (chosen >= 0);
  unsigned wx_step = (chosen < 0) ? WX_LADDER_N - 1 : (unsigned)chosen;
  L.wx_font = fonts_get_system_font(WX_LADDER[wx_step].key);
  L.wx_fs = WX_LADDER[wx_step].size;

  // AM/PM shares the cloud row rather than taking one of its own: a separate row
  // costs 20 px and pushes the block outside the inner circle on 144×168.
  int wx_rows = L.stack ? 2 : 1;
  L.top = cy - (L.time_h + L.cloud_h + L.wx_h * wx_rows) / 2;
  L.ampm_y = L.top + L.time_h + 2;

  int inner_r = (cx < cy ? cx : cy) - RING_MARGIN - RING_THICKNESS;
  int limit_y = cy + inner_r - 8;
  int block_bottom = L.top + L.time_h + L.cloud_h + L.wx_h * (L.show_wx ? wx_rows : 0) + 4;
  // If the block plus the status icon does not fit inside the circle, the BLOCK
  // gives way, not the icon: the wind/visibility row goes, being supporting
  // detail, while the "go or not" decision rests on cloud cover.
  if (!L.stack && (!L.show_wx || block_bottom + 4 > limit_y)) {
    L.show_wx = false;
    L.top = cy - (L.time_h + L.cloud_h) / 2;
    block_bottom = L.top + L.time_h + L.cloud_h + 4;
    L.ampm_y = L.top + L.time_h + 2;
  }

  // The status icon lives below the block, inside the inner circle. In stack
  // mode the block fills the circle's full height, so the icon moves ABOVE the
  // time.
  if (L.stack) {
    int a = cy - inner_r + 2, b = L.top - 14;
    L.status_y = (a > b) ? a : b;
  } else {
    int a = block_bottom + 4, b = cy + (inner_r * 62) / 100;
    int v = (a > b) ? a : b;
    L.status_y = (v < limit_y) ? v : limit_y;
  }

  // In the compact layout cloud cover steps down with the wind row: at full size
  // it rivals the time and, being green, reads as the main element of the frame.
  L.cloud_fs = L.show_wx ? 18 : 14;
  L.cloud_font = fonts_get_system_font(L.show_wx ? FONT_KEY_GOTHIC_18
                                                 : FONT_KEY_GOTHIC_14);

  // The cloud icon has its own fit check: "100%" is four times shorter than a
  // weather string, so where wind has no room for an icon, cloud still does.
  {
    int room = 2 * chord_half_at(cx, cy, ring_in, L.top + L.time_h, L.cloud_h);
    // In 12-hour mode AM/PM occupies the right of the same band, and the
    // "icon + percentage" group is centred in what remains.
    if (!s_settings.use_24h) room -= 24;
    L.cloud_icon = (text_width(CLOUD_WIDEST, L.cloud_font)
                    + ICON_ADVANCE(L.cloud_fs)) <= room;
  }

  {
    int dy1 = L.ampm_y - cy;              if (dy1 < 0) dy1 = -dy1;
    int dy2 = L.ampm_y + L.cloud_h - cy;  if (dy2 < 0) dy2 = -dy2;
    int dy = (dy1 > dy2) ? dy1 : dy2;
    int32_t under = (int32_t)inner_r * inner_r - (int32_t)dy * dy;
    if (under < 1) under = 1;
    int ch = 2 * (int)isqrt32(under) - 4;
    if (ch < 24) ch = 24;
    L.ampm_x = cx - ch / 2;
    L.ampm_w = ch;
  }

  // Wind and visibility are not tied to half the canvas: their outer ends would
  // run into the ring band. Each is as wide as the chord of ITS OWN row; in stack
  // mode visibility sits below wind and has a chord of its own.
  L.wx_y = L.top + L.time_h + L.cloud_h;
  L.vis_y = L.stack ? L.wx_y + L.wx_h : L.wx_y;
  int wx_half = chord_half_at(cx, cy, ring_in, L.wx_y, L.wx_h);
  int vis_half = chord_half_at(cx, cy, ring_in, L.vis_y, L.wx_h);
  if (L.stack) {
    L.wind_x = cx - wx_half;  L.wind_w = wx_half * 2;  L.wind_align = GTextAlignmentCenter;
    L.vis_x  = cx - vis_half; L.vis_w  = vis_half * 2; L.vis_align  = GTextAlignmentCenter;
  } else {
    L.wind_x = cx - wx_half;  L.wind_w = wx_half - 3;  L.wind_align = GTextAlignmentRight;
    L.vis_x  = cx + 3;        L.vis_w  = vis_half - 3; L.vis_align  = GTextAlignmentLeft;
  }
  return L;
}

// --- Status icon ---
//
// Four states told apart by SHAPE, not by colour alone: links closed — data
// fresh, links open — stale, links struck through — no connection, chevrons —
// refresh in flight. "Stale" and "no connection" must stay distinguishable: a
// forced refresh helps in the first case and nothing helps in the second.
//
// The position comes from a FORMULA over the bounds rather than a constant in a
// corner, where the icon slides under the mask of round displays.
//
// 18 px, not 14: MEASURED, at 14 px a chain link has a radius of 2 px and
// "fresh" differs from "stale" by ONE pixel of gap. At 18 px the gap is 2 px and
// the state reads at a glance.
#define STATUS_ICON_SIZE 18

static void draw_status(GContext *ctx, GRect bounds, const ClockLayout *L,
                        uint32_t now_sec) {
  int cx = bounds.size.w / 2;
  IconGlyph g;
  GColor color;

  if (!s_connected) {
    g = ICON_NOLINK;
    color = PBL_IF_COLOR_ELSE(GColorRed, GColorWhite);
  } else if (s_refreshing) {
    g = ICON_SYNC;
    color = PBL_IF_COLOR_ELSE(GColorLightGray, GColorWhite);
  } else if (packet_is_stale(s_packet.data_ts, now_sec, STALE_THRESHOLD_SEC)) {
    // Light grey, not dark: on 1-bit displays GColorDarkGray collapses to BLACK
    // and "stale" would be indistinguishable from "no icon drawn".
    g = ICON_LINKSTALE;
    color = PBL_IF_COLOR_ELSE(GColorLightGray, GColorWhite);
  } else {
    g = ICON_LINK;
    color = GColorWhite;
  }

  icon_draw(ctx, g, GPoint(cx - STATUS_ICON_SIZE / 2, L->status_y - 4),
            STATUS_ICON_SIZE, color);
}

// --- Clock screen ---
static void draw_clock(GContext *ctx, GRect bounds, const struct tm *now_tm,
                       uint32_t now_sec) {
  int cx = bounds.size.w / 2;
  int cy = bounds.size.h / 2;
  int outer_r = (cx < cy ? cx : cy) - RING_MARGIN;
  draw_ring(ctx, cx, cy, outer_r, now_tm);

  ClockLayout L = clock_layout(bounds);

  // Time.
  static char time_buf[8];
  if (s_settings.use_24h) {
    strftime(time_buf, sizeof(time_buf), "%H:%M", now_tm);
  } else {
    strftime(time_buf, sizeof(time_buf), "%I:%M", now_tm);
    // 12-hour format without the leading zero: "09:30" → "9:30".
    if (time_buf[0] == '0') memmove(time_buf, time_buf + 1, strlen(time_buf));
  }
  graphics_context_set_text_color(ctx, GColorWhite);
  draw_text_vcenter(ctx, time_buf, L.time_font,
                    GRect(0, L.top, bounds.size.w, L.time_h), GTextAlignmentCenter);

  // AM/PM sits to the right on the cloud row. The time fonts contain DIGITS ONLY
  // and cannot render letters, so the suffix always uses a separate GOTHIC_14.
  if (!s_settings.use_24h) {
    graphics_context_set_text_color(ctx, PBL_IF_COLOR_ELSE(GColorLightGray, GColorWhite));
    draw_text_vcenter(ctx, (now_tm->tm_hour < 12) ? "AM" : "PM",
                      fonts_get_system_font(FONT_KEY_GOTHIC_14),
                      GRect(L.ampm_x, L.ampm_y, L.ampm_w, L.cloud_h),
                      GTextAlignmentRight);
  }

  // Cloud cover under the time is the primary weather figure, hence its own row
  // and a larger size than the rest.
  static char cloud_buf[8];
  GColor cloud_color;
  if (s_packet.wx_cloud < 0) {
    strncpy(cloud_buf, "-- %", sizeof(cloud_buf));
    cloud_color = WX_NONE;
  } else {
    snprintf(cloud_buf, sizeof(cloud_buf), "%d%%", s_packet.wx_cloud);
    cloud_color = (s_packet.wx_cloud < 30)
                      ? WX_GOOD
                      : (s_packet.wx_cloud < 70 ? WX_FAIR : WX_POOR);
  }
  GRect cloud_box = GRect(0, L.top + L.time_h, bounds.size.w, L.cloud_h);
  if (L.cloud_icon) {
    draw_icon_text(ctx, ICON_CLOUD, cloud_buf, L.cloud_font, cloud_box,
                   GTextAlignmentCenter, L.cloud_fs, cloud_color);
  } else {
    graphics_context_set_text_color(ctx, cloud_color);
    draw_text_vcenter(ctx, cloud_buf, L.cloud_font, cloud_box,
                      GTextAlignmentCenter);
  }

  // Wind and visibility only qualify the conditions, so they are the rows dropped
  // when room runs out (see clock_layout).
  if (L.show_wx) {
    char wind_buf[10], vis_buf[12];
    wx_format_wind(wind_buf, sizeof(wind_buf), s_packet.wx_wind_ms,
                   s_settings.use_imperial);
    wx_format_visibility(vis_buf, sizeof(vis_buf), s_packet.wx_visibility_m,
                         s_settings.use_imperial);

    GRect wind_box = GRect(L.wind_x, L.wx_y, L.wind_w, L.wx_h);
    GRect vis_box = GRect(L.vis_x, L.vis_y, L.vis_w, L.wx_h);
    GColor wind_color = wx_wind_color(s_packet.wx_wind_ms);
    GColor vis_color = wx_visibility_color(s_packet.wx_visibility_m);
    if (L.wx_icon) {
      draw_icon_text(ctx, ICON_WIND, wind_buf, L.wx_font, wind_box,
                     L.wind_align, L.wx_fs, wind_color);
      draw_icon_text(ctx, ICON_EYE, vis_buf, L.wx_font, vis_box, L.vis_align,
                     L.wx_fs, vis_color);
    } else {
      graphics_context_set_text_color(ctx, wind_color);
      draw_text_vcenter(ctx, wind_buf, L.wx_font, wind_box, L.wind_align);
      graphics_context_set_text_color(ctx, vis_color);
      draw_text_vcenter(ctx, vis_buf, L.wx_font, vis_box, L.vis_align);
    }
  }

  draw_status(ctx, bounds, &L, now_sec);
}

// --- Astro screen ---
//
// THE LAYOUT IS DERIVED FROM BOUNDS rather than set by constants: the target
// screens differ by almost a factor of two (144×168 … 260×260), and a fixed row
// pitch cuts off the moon block on the small ones.
//
// On round platforms the INSCRIBED area is used rather than the whole rectangle,
// or text with an 8 px margin slides under the mask ("Rise" → "ise" on chalk).
typedef enum {
  ASTRO_SUN_HDR = 0,
  ASTRO_SUN_RISE,
  ASTRO_SUN_SET,
  ASTRO_GOLD,
  ASTRO_BLUE,
  ASTRO_DIVIDER,
  ASTRO_MOON_HDR,
  ASTRO_MOON_RISE,
  ASTRO_MOON_SET,
  ASTRO_PHASE,
  ASTRO_ROWS
} AstroRowId;

// Rows with a label and a value: they share a left edge (see astro_metrics).
// Headers, the divider and the phase run full width and are not in this list.
static const uint8_t ASTRO_PAIR_ROWS[] = {
  ASTRO_SUN_RISE, ASTRO_SUN_SET, ASTRO_GOLD, ASTRO_BLUE,
  ASTRO_MOON_RISE, ASTRO_MOON_SET
};
#define ASTRO_PAIR_ROWS_N (sizeof(ASTRO_PAIR_ROWS) / sizeof(ASTRO_PAIR_ROWS[0]))

// Minimum row pitch. MEASURED: the line height of GOTHIC_14 is exactly 14 px —
// below that adjacent rows start to touch, and there is no smaller step.
#define ASTRO_MIN_STEP 14

// The order in which rows are dropped on cramped canvases. The divider goes
// first (decoration; the headers already separate the blocks), then the sun's
// Set and Rise, whose times all but coincide with the window boundaries.
//
// NEVER dropped: the window times, the sun header (it carries the
// morning/evening note, without which a window reads ambiguously) and the moon
// block. Three rows is all any of the six platforms needs to give up.
static const uint8_t ASTRO_DROP_ORDER[] = {
  ASTRO_DIVIDER, ASTRO_SUN_SET, ASTRO_SUN_RISE
};
#define ASTRO_DROP_ORDER_N (sizeof(ASTRO_DROP_ORDER) / sizeof(ASTRO_DROP_ORDER[0]))

typedef struct {
  GFont font;
  int fs;      // point size; the label column is derived from it
  int line_h;
  int top;
  int x_pairs; // shared left edge of the "label + value" rows
  int rows;    // how many rows are actually shown
  int slot[ASTRO_ROWS]; // row position top to bottom, −1 = hidden
} AstroMetrics;

// Geometry of ONE row: every row has its own.
typedef struct { int y, x, w, label_w; } AstroRow;

// Half-width of row i from the CHORD at ITS OWN height. One width shared by the
// whole block would be dictated by its narrowest row, and on round displays
// "Gold 05:12-06:05" gets clipped even though its own row, near the centre of
// the circle, has twice the room.
static int astro_row_half(GRect bounds, const AstroMetrics *m, int i) {
#ifdef PBL_ROUND
  int w = bounds.size.w, bh = bounds.size.h;
  // Radius from the very edge of the canvas (−2): there is no ring on this
  // screen, so the text fits into the mask rather than into an inner circle.
  int radius = ((w < bh) ? w : bh) / 2 - 2;
  int y = m->top + m->line_h * i;
  int half = chord_half_at(w / 2, bh / 2, radius, y, m->line_h) - 3;
  int lim = (w - 8) / 2;
  if (half > lim) half = lim;
  if (half < 20) half = 20;
  return half;
#else
  (void)m; (void)i;
  return (bounds.size.w - 16) / 2;
#endif
}

static AstroMetrics astro_metrics(GRect bounds) {
  AstroMetrics m;
  int h_eff = PBL_IF_ROUND_ELSE(bounds.size.h * 4 / 5, bounds.size.h - 12);

  // How many rows fit at all. Ten rows of 14 px need 140 px, while under the
  // Timeline peek on 144×168 only 111 remain. Rows are DROPPED and the survivors
  // keep the full pitch — shrinking the pitch instead makes them overlap.
  bool hidden[ASTRO_ROWS] = { false };
  m.rows = ASTRO_ROWS;
  // The phase has no row of its own: it reads as "☾ 41%" inside the moon header,
  // qualifying the moon exactly as "morning" qualifies the sun.
  hidden[ASTRO_PHASE] = true;
  m.rows--;
  for (unsigned k = 0; k < ASTRO_DROP_ORDER_N && m.rows * ASTRO_MIN_STEP > h_eff; k++) {
    hidden[ASTRO_DROP_ORDER[k]] = true;
    m.rows--;
  }
  int slot = 0;
  for (int i = 0; i < ASTRO_ROWS; i++) m.slot[i] = hidden[i] ? -1 : slot++;

  m.line_h = h_eff / m.rows;
  // The font is chosen TO FIT the row pitch, otherwise glyphs collide.
  if (m.line_h >= 26)      { m.font = fonts_get_system_font(FONT_KEY_GOTHIC_24); m.fs = 24; }
  else if (m.line_h >= 20) { m.font = fonts_get_system_font(FONT_KEY_GOTHIC_18); m.fs = 18; }
  else                     { m.font = fonts_get_system_font(FONT_KEY_GOTHIC_14); m.fs = 14; }

  m.top = (bounds.size.h - m.line_h * m.rows) / 2; // block centred on screen
  if (m.top < 2) m.top = 2;

  // The SHARED left edge of the labelled rows comes from the narrowest of them,
  // so the labels line up in a column and the extra width of the central rows
  // goes to the values on the right. Centring each row on its own chord instead
  // gives a ragged staircase of labels on round displays.
  //
  // Headers and the phase must NOT be aligned this way: they sit where the chord
  // is narrowest, and a shared left edge pushes them under the mask.
  m.x_pairs = 0;
  for (unsigned k = 0; k < ASTRO_PAIR_ROWS_N; k++) {
    int s = m.slot[ASTRO_PAIR_ROWS[k]];
    if (s < 0) continue; // row dropped — its chord does not affect the column
    int x = bounds.size.w / 2 - astro_row_half(bounds, &m, s);
    if (x > m.x_pairs) m.x_pairs = x;
  }
  return m;
}

// A "label + value" row: shared left edge, right edge from its own chord.
static AstroRow astro_row(GRect bounds, const AstroMetrics *m, int i) {
  AstroRow r;
  r.y = m->top + m->line_h * i;
  r.x = m->x_pairs;
  r.w = bounds.size.w / 2 + astro_row_half(bounds, m, i) - r.x;
  if (r.w < 40) r.w = 40;
  // The label column is sized by the widest REAL label ("Rise"), not by a
  // fraction of the row width — slack next to a clipped value is not acceptable.
  // The icon belongs to the same column, or the values would shift between rows
  // by its width.
  int lw = (m->fs * 23) / 10 + ICON_ADVANCE(m->fs);
  int half_w = r.w / 2;
  r.label_w = (lw < half_w) ? lw : half_w;
  return r;
}

// A full-width row. It snaps to the same column as the labels, but only where
// the chord allows: at the outermost rows the circle narrows so much that such a
// row has to stay where it is (see astro_metrics).
static AstroRow astro_row_wide(GRect bounds, const AstroMetrics *m, int i) {
  AstroRow r;
  r.y = m->top + m->line_h * i;
  int half = astro_row_half(bounds, m, i);
  int own_x = bounds.size.w / 2 - half;
  r.x = (own_x > m->x_pairs) ? own_x : m->x_pairs;
  r.w = bounds.size.w / 2 + half - r.x;
  r.label_w = 0;
  return r;
}

static void draw_astro_line(GContext *ctx, GRect bounds, const AstroMetrics *m,
                            AstroRowId id, IconGlyph g, const char *label,
                            const char *value, GColor label_color) {
  if (m->slot[id] < 0) return;
  AstroRow r = astro_row(bounds, m, m->slot[id]);
  int adv = ICON_ADVANCE(m->fs);
  // The icon takes the colour of its own body, like the label: on colour
  // platforms that is a second cue for sun block against moon block.
  icon_draw(ctx, g,
            GPoint(r.x, r.y + (m->line_h - m->fs) / 2 + icon_text_dy(g, m->fs)),
            m->fs, label_color);
  graphics_context_set_text_color(ctx, label_color);
  draw_text_vcenter(ctx, label, m->font,
                    GRect(r.x + adv, r.y, r.label_w - adv, m->line_h),
                    GTextAlignmentLeft);
  graphics_context_set_text_color(ctx, GColorWhite);
  draw_text_vcenter(ctx, value, m->font,
                    GRect(r.x + r.label_w, r.y, r.w - r.label_w, m->line_h),
                    GTextAlignmentLeft);
}

// Headers and the phase row run the full width, without a label column.
static void draw_astro_full(GContext *ctx, GRect bounds, const AstroMetrics *m,
                            AstroRowId id, const char *text, GColor color) {
  if (m->slot[id] < 0) return;
  AstroRow r = astro_row_wide(bounds, m, m->slot[id]);
  graphics_context_set_text_color(ctx, color);
  draw_text_vcenter(ctx, text, m->font, GRect(r.x, r.y, r.w, m->line_h),
                    GTextAlignmentLeft);
}

// Moon header: the phase glyph plus "MOON 41%". The glyph is DATA, not one of
// eight ready-made pictures — its terminator is built from moon_illum, so 41 %
// and 44 % are drawn differently. With it present a phase name ("Last Qtr")
// would add nothing the percentage does not already say.
static void draw_astro_moon_hdr(GContext *ctx, GRect bounds,
                                const AstroMetrics *m, GColor color) {
  if (m->slot[ASTRO_MOON_HDR] < 0) return;
  AstroRow r = astro_row_wide(bounds, m, m->slot[ASTRO_MOON_HDR]);
  int adv = ICON_ADVANCE(m->fs);
  char buf[16];
  if (packet_is_valid_ts(s_packet.data_ts)) {
    snprintf(buf, sizeof(buf), "MOON %d%%", s_packet.moon_illum);
  } else {
    strncpy(buf, "MOON", sizeof(buf));
  }
  icon_draw_moon(ctx,
                 GPoint(r.x, r.y + (m->line_h - m->fs) / 2
                                 + icon_moon_text_dy(m->fs)),
                 m->fs, color, s_packet.moon_illum, s_packet.moon_phase < 4);
  graphics_context_set_text_color(ctx, color);
  draw_text_vcenter(ctx, buf, m->font, GRect(r.x + adv, r.y, r.w - adv, m->line_h),
                    GTextAlignmentLeft);
}

static void draw_astro_divider(GContext *ctx, GRect bounds,
                               const AstroMetrics *m, AstroRowId id) {
  if (m->slot[id] < 0) return;
  AstroRow r = astro_row_wide(bounds, m, m->slot[id]);
  // Dark grey on colour only: on 1-bit displays GColorDarkGray collapses to
  // BLACK and the divider disappears.
  graphics_context_set_fill_color(ctx, PBL_IF_COLOR_ELSE(GColorDarkGray, GColorWhite));
  graphics_fill_rect(ctx, GRect(r.x, r.y + m->line_h / 2, r.w, 1), 0, GCornerNone);
}

// "The moon does not set today" and "there is no data at all" are different
// things, yet the phone sends zero for both. They are told apart by the presence
// of a PACKET: with one, a missing time means the event does not happen today
// and the value is left empty; without one, dashes as for every other figure.
static void astro_event_time(char *buf, size_t n, uint32_t ts, bool use_24h) {
  if (ts == 0 && packet_is_valid_ts(s_packet.data_ts)) {
    buf[0] = '\0';
    return;
  }
  timefmt_ts_hm(buf, n, ts, use_24h);
}

static void draw_astro(GContext *ctx, GRect bounds) {
  GColor sun_color = PBL_IF_COLOR_ELSE(GColorChromeYellow, GColorWhite);
  GColor moon_color = PBL_IF_COLOR_ELSE(GColorBlueMoon, GColorWhite);
  AstroMetrics m = astro_metrics(bounds);
  char buf1[8], buf2[8], val[24];

  // The phone sends ONLY the nearest light window, and "Gold 6:28-7:05" without
  // a note reads ambiguously. Which one arrived is told from the ORDER of the
  // blue and golden hours rather than by matching against SunRise: in the morning
  // blue comes BEFORE golden (dawn → sunrise), in the evening after (sunset →
  // dusk). That is physics, so the cue survives changes on the phone side.
  const char *sun_hdr = "SUN";
  if (packet_is_valid_ts(s_packet.sun_blue_start) &&
      packet_is_valid_ts(s_packet.sun_golden_start)) {
    sun_hdr = (s_packet.sun_blue_start < s_packet.sun_golden_start)
                  ? "SUN morning" : "SUN evening";
  }
  draw_astro_full(ctx, bounds, &m, ASTRO_SUN_HDR, sun_hdr, sun_color);

  timefmt_ts_hm(buf1, sizeof(buf1), s_packet.sun_rise, s_settings.use_24h);
  draw_astro_line(ctx, bounds, &m, ASTRO_SUN_RISE, ICON_SUNRISE, "Rise", buf1,
                  sun_color);
  timefmt_ts_hm(buf1, sizeof(buf1), s_packet.sun_set, s_settings.use_24h);
  draw_astro_line(ctx, bounds, &m, ASTRO_SUN_SET, ICON_SUNSET, "Set", buf1,
                  sun_color);
  timefmt_ts_hm(buf1, sizeof(buf1), s_packet.sun_golden_start, s_settings.use_24h);
  timefmt_ts_hm(buf2, sizeof(buf2), s_packet.sun_golden_end, s_settings.use_24h);
  snprintf(val, sizeof(val), "%s-%s", buf1, buf2);
  draw_astro_line(ctx, bounds, &m, ASTRO_GOLD, ICON_GOLD, "Gold", val, sun_color);
  timefmt_ts_hm(buf1, sizeof(buf1), s_packet.sun_blue_start, s_settings.use_24h);
  timefmt_ts_hm(buf2, sizeof(buf2), s_packet.sun_blue_end, s_settings.use_24h);
  snprintf(val, sizeof(val), "%s-%s", buf1, buf2);
  draw_astro_line(ctx, bounds, &m, ASTRO_BLUE, ICON_BLUE, "Blue", val, sun_color);

  draw_astro_divider(ctx, bounds, &m, ASTRO_DIVIDER);

  draw_astro_moon_hdr(ctx, bounds, &m, moon_color);
  astro_event_time(buf1, sizeof(buf1), s_packet.moon_rise, s_settings.use_24h);
  draw_astro_line(ctx, bounds, &m, ASTRO_MOON_RISE, ICON_MOONRISE, "Rise", buf1,
                  moon_color);
  astro_event_time(buf1, sizeof(buf1), s_packet.moon_set, s_settings.use_24h);
  draw_astro_line(ctx, bounds, &m, ASTRO_MOON_SET, ICON_MOONSET, "Set", buf1,
                  moon_color);
}

// --- Stopwatch screen ---
static uint64_t sw_elapsed_ms(void) {
  return s_sw_elapsed_ms;
}

// Steps for the readout and the labels are system fonts only; there are no sizes
// in between. For LECO these are _BOLD_NUMBERS: a plain NUMBERS face exists in
// exactly one size, 42.
static const TimeStep SW_LADDER[] = {
  { 38, FONT_KEY_LECO_38_BOLD_NUMBERS },
  { 36, FONT_KEY_LECO_36_BOLD_NUMBERS },
  { 32, FONT_KEY_LECO_32_BOLD_NUMBERS },
  { 20, FONT_KEY_LECO_20_BOLD_NUMBERS },
};
#define SW_LADDER_N (sizeof(SW_LADDER) / sizeof(SW_LADDER[0]))

static const TimeStep SW_LABEL_LADDER[] = {
  { 24, FONT_KEY_GOTHIC_24 },
  { 18, FONT_KEY_GOTHIC_18 },
  { 14, FONT_KEY_GOTHIC_14 },
};
#define SW_LABEL_LADDER_N (sizeof(SW_LABEL_LADDER) / sizeof(SW_LABEL_LADDER[0]))

static void draw_stopwatch(GContext *ctx, GRect bounds, const struct tm *now_tm) {
  uint64_t ms = sw_elapsed_ms();
  uint32_t total_tenths = (uint32_t)(ms / 100);
  uint32_t tenths = total_tenths % 10;
  uint32_t total_sec = total_tenths / 10;
  uint32_t sec = total_sec % 60;
  uint32_t min = total_sec / 60;

  static char buf[16];
  snprintf(buf, sizeof(buf), "%02u:%02u.%u",
           (unsigned)min, (unsigned)sec, (unsigned)tenths);

  int w = bounds.size.w, bh = bounds.size.h;
  int usable = PBL_IF_ROUND_ELSE(w * 3 / 5, w - 16);

  // The step is the largest one that fits BOTH 24 % of the canvas height AND the
  // width. The width is measured against a REFERENCE string of the same length
  // ("88:88.8"), never against the live readout: LECO digits are NOT monospaced,
  // so "11:11.1" is 7 px narrower than "00:00.0" and the step would change ten
  // times a second. Length still counts — past 100 minutes a digit is added, the
  // reference grows and the step honestly drops.
  char probe[16];
  {
    size_t i = 0;
    for (; buf[i] && i + 1 < sizeof(probe); i++) {
      probe[i] = (buf[i] >= '0' && buf[i] <= '9') ? '8' : buf[i];
    }
    probe[i] = '\0';
  }

  int cap_h = (bh * 24) / 100;
  GFont read_font = fonts_get_system_font(SW_LADDER[SW_LADDER_N - 1].key);
  int fs = SW_LADDER[SW_LADDER_N - 1].size;
  for (unsigned i = 0; i < SW_LADDER_N; i++) {
    GFont f = fonts_get_system_font(SW_LADDER[i].key);
    if (SW_LADDER[i].size <= cap_h && text_width(probe, f) <= usable) {
      read_font = f;
      fs = SW_LADDER[i].size;
      break;
    }
  }

  // The label is at most 55 % of the readout, which stays the largest element on
  // screen and the only white one.
  int cap_label = (fs * 55) / 100;
  GFont clk_font = fonts_get_system_font(FONT_KEY_GOTHIC_14);
  int cfs = 14;
  for (unsigned i = 0; i < SW_LABEL_LADDER_N; i++) {
    if (SW_LABEL_LADDER[i].size <= cap_label) {
      clk_font = fonts_get_system_font(SW_LABEL_LADDER[i].key);
      cfs = SW_LABEL_LADDER[i].size;
      break;
    }
  }

  int read_h = (fs * 12) / 10;
  int clk_h = (cfs * 14) / 10;
  int hint_h = (bh >= 200) ? 22 : 18;
  int top = bh / 2 - (clk_h + 4 + read_h + 6 + hint_h) / 2;

  // The current time sits small and grey above the readout: useful during a long
  // exposure in the dark, but it must not pull attention.
  static char clk_buf[8];
  if (s_settings.use_24h) {
    strftime(clk_buf, sizeof(clk_buf), "%H:%M", now_tm);
  } else {
    strftime(clk_buf, sizeof(clk_buf), "%I:%M", now_tm);
    if (clk_buf[0] == '0') memmove(clk_buf, clk_buf + 1, strlen(clk_buf));
  }
  graphics_context_set_text_color(ctx, GColorLightGray);
  draw_text_vcenter(ctx, clk_buf, clk_font, GRect(0, top, w, clk_h),
                    GTextAlignmentCenter);

  graphics_context_set_text_color(ctx, GColorWhite);
  draw_text_vcenter(ctx, buf, read_font, GRect(0, top + clk_h + 4, w, read_h),
                    GTextAlignmentCenter);

  // A hint for what the next tap will do — the input model has a single gesture.
  const char *hint;
  switch (s_sw_state) {
    case SW_IDLE:    hint = "tap: start"; break;
    case SW_RUN:     hint = "tap: stop"; break;
    case SW_STOPPED:
    default:         hint = "tap: reset & exit"; break;
  }
  graphics_context_set_text_color(ctx, GColorLightGray);
  draw_text_vcenter(ctx, hint,
                    fonts_get_system_font((bh >= 200) ? FONT_KEY_GOTHIC_18
                                                      : FONT_KEY_GOTHIC_14),
                    GRect(0, top + clk_h + 4 + read_h + 6, w, hint_h),
                    GTextAlignmentCenter);
}

// --- Draw dispatcher ---
static void canvas_update_proc(Layer *layer, GContext *ctx) {
  // The BACKGROUND is filled over the layer's FULL bounds while the CONTENT is
  // laid out within the UNOBSTRUCTED area: Timeline Quick View slides up from the
  // bottom and eats part of the screen. Every layout derives from the bounds
  // passed in, so handing them the unobstructed rect is all it takes.
  GRect full = layer_get_bounds(layer);
  GRect bounds = layer_get_unobstructed_bounds(layer);
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, full, 0, GCornerNone);

  // Wall-clock time comes from the s_now_tm cache (see its declaration); time()
  // itself is accurate and serves the staleness check.
  uint32_t now_sec = (uint32_t)time(NULL);

  switch (s_mode) {
    case MODE_ASTRO:     draw_astro(ctx, bounds); break;
    case MODE_STOPWATCH: draw_stopwatch(ctx, bounds, &s_now_tm); break;
    case MODE_CLOCK:
    default:             draw_clock(ctx, bounds, &s_now_tm, now_sec); break;
  }

  // "Refreshing…" for the screens without a status icon — on Clock it is one of
  // that icon's four states instead.
  if (s_refreshing && s_mode != MODE_CLOCK) {
    graphics_context_set_text_color(ctx, GColorWhite);
    graphics_draw_text(ctx, "\u00BB\u00BB", fonts_get_system_font(FONT_KEY_GOTHIC_18),
                       GRect(bounds.origin.x, bounds.origin.y + 4, bounds.size.w, 20),
                       GTextOverflowModeFill, GTextAlignmentCenter, NULL);
  }
}

static void redraw(void) {
  if (s_canvas) layer_mark_dirty(s_canvas);
}

// --- Auto-return Astro→Clock ---
static void astro_timeout_cb(void *ctx) {
  s_astro_timer = NULL;
  if (s_mode == MODE_ASTRO) {
    s_mode = MODE_CLOCK;
    redraw();
  }
}

static void cancel_astro_timer(void) {
  if (s_astro_timer) {
    app_timer_cancel(s_astro_timer);
    s_astro_timer = NULL;
  }
}

static void schedule_astro_timer(void) {
  cancel_astro_timer();
  APP_LOG(APP_LOG_LEVEL_INFO, "schedule_astro_timer: astro_timeout_sec=%d %s",
          (int)s_settings.astro_timeout_sec,
          (s_settings.astro_timeout_sec > 0) ? "(arming)" : "(DISABLED)");
  if (s_settings.astro_timeout_sec > 0) {
    s_astro_timer = app_timer_register(
        (uint32_t)s_settings.astro_timeout_sec * 1000, astro_timeout_cb, NULL);
  }
}

// --- Auto-exit Stopwatch(idle)→Clock ---
// Exists only while the stopwatch is in SW_IDLE ("never started"); Start cancels
// the timer so a running measurement is never reset from under the user.
static void sw_idle_timeout_cb(void *ctx) {
  s_sw_idle_timer = NULL;
  if (s_mode == MODE_STOPWATCH && s_sw_state == SW_IDLE) {
    s_sw_elapsed_ms = 0;
    s_mode = MODE_CLOCK;
    redraw();
  }
}

static void cancel_sw_idle_timer(void) {
  if (s_sw_idle_timer) {
    app_timer_cancel(s_sw_idle_timer);
    s_sw_idle_timer = NULL;
  }
}

static void schedule_sw_idle_timer(void) {
  cancel_sw_idle_timer();
  if (s_settings.sw_idle_timeout_sec > 0) {
    s_sw_idle_timer = app_timer_register(
        (uint32_t)s_settings.sw_idle_timeout_sec * 1000, sw_idle_timeout_cb, NULL);
  }
}

// --- Stopwatch ticker ---
// The condition deliberately does NOT involve s_mode: counting depends on the
// sub-state alone, so timing never becomes hostage to navigation.
static void sw_ticker_cb(void *ctx) {
  s_sw_ticker = NULL;
  if (s_sw_state != SW_RUN) return;

  s_sw_elapsed_ms += SW_TICK_MS;

  // Safety net: a stopwatch left running keeps a 100 ms timer alive and drains
  // the battery. At the limit it auto-stops with a buzz (0 = no limit); the
  // 30-minute default is set so as not to cut a star-trail exposure short.
  if (s_settings.sw_max_duration_min > 0 &&
      s_sw_elapsed_ms >= (uint64_t)s_settings.sw_max_duration_min * 60000ULL) {
    s_sw_state = SW_STOPPED;
    vibes_short_pulse();
    APP_LOG(APP_LOG_LEVEL_INFO, "stopwatch auto-stopped at %u min limit",
            (unsigned)s_settings.sw_max_duration_min);
    redraw();
    return; // do not re-arm the ticker
  }

  // Off its own screen there is nothing to redraw: the marker shows minutes and
  // is updated by the regular minute tick.
  if (s_mode == MODE_STOPWATCH) redraw();
  s_sw_ticker = app_timer_register(SW_TICK_MS, sw_ticker_cb, NULL);
}

static void sw_start_ticker(void) {
  if (!s_sw_ticker) {
    s_sw_ticker = app_timer_register(SW_TICK_MS, sw_ticker_cb, NULL);
  }
}

static void sw_stop_ticker(void) {
  if (s_sw_ticker) {
    app_timer_cancel(s_sw_ticker);
    s_sw_ticker = NULL;
  }
}

// --- Forced refresh ---
static void send_refresh(void) {
  DictionaryIterator *iter;
  if (app_message_outbox_begin(&iter) != APP_MSG_OK) return;
  s_refresh_tick = (s_refresh_tick + 1) & 0xff;
  dict_write_uint8(iter, MESSAGE_KEY_Refresh, s_refresh_tick);
  app_message_outbox_send();
  APP_LOG(APP_LOG_LEVEL_INFO, "refresh request sent (tick %d)", s_refresh_tick);
}

static void refresh_timeout_cb(void *ctx) {
  s_refresh_timeout = NULL;
  s_refreshing = false;
  APP_LOG(APP_LOG_LEVEL_INFO, "refresh timeout: no packet");
  redraw();
}

static void request_refresh(void) {
  if (!s_connected) {
    APP_LOG(APP_LOG_LEVEL_INFO, "refresh skipped: not connected");
    redraw();
    return;
  }
  // Several triggers can fire at once (entering a screen, the link coming back);
  // without this gate they pile up as identical requests to the phone.
  if (s_refreshing) {
    APP_LOG(APP_LOG_LEVEL_INFO, "refresh skipped: already in flight");
    return;
  }
  s_refreshing = true;
  if (s_refresh_timeout) app_timer_cancel(s_refresh_timeout);
  s_refresh_timeout = app_timer_register(REFRESH_TIMEOUT_MS, refresh_timeout_cb, NULL);
  redraw();
  send_refresh();
}

// --- Vibrating notification for the start of a light window ---
//
// COMPUTED ON THE WATCH, IN THE MINUTE TICK — not through the Wakeup API, which
// launches an app while the watchface is already running. A watchface receives
// MINUTE_UNIT continuously while it is selected, and minute accuracy is ample
// for "time to go shooting".
static uint32_t s_notified_window_ts = 0;

// How far apart two times must be to count as DIFFERENT windows. Exact equality
// will not do: the phone recomputes the astronomy on every update and the GPS
// position drifts, so the same window arrives seconds earlier or later each time
// and would buzz again on every packet (a 21-second shift did it in testing).
// Adjacent windows are at least six hours apart, so two hours separates "the
// same window" from "the next one" with room to spare.
#define NOTIFY_SAME_WINDOW_SEC (2 * 60 * 60)

static bool notify_same_window(uint32_t a, uint32_t b) {
  uint32_t diff = (a > b) ? (a - b) : (b - a);
  return diff < NOTIFY_SAME_WINDOW_SEC;
}

// Start of the nearest light window, 0 = unknown. The phone only ever sends the
// UPCOMING window, so the earlier of the two boundaries is what opens it: the
// blue hour in the morning, the golden one in the evening.
static uint32_t light_window_start(void) {
  uint32_t g = s_packet.sun_golden_start;
  uint32_t b = s_packet.sun_blue_start;
  if (!packet_is_valid_ts(g)) return packet_is_valid_ts(b) ? b : 0;
  if (!packet_is_valid_ts(b)) return g;
  return (b < g) ? b : g;
}

static void check_light_notify(uint32_t now_sec) {
  if (s_settings.notify_lead_min == 0) return;

  uint32_t start = light_window_start();
  if (start == 0) return;
  if (s_notified_window_ts != 0 &&
      notify_same_window(start, s_notified_window_ts)) {
    return; // already buzzed for this window
  }
  if (start <= now_sec) return;              // window already open, too late

  uint32_t lead_sec = (uint32_t)s_settings.notify_lead_min * 60;
  if (start - now_sec > lead_sec) return;    // still too early

  // The marker is set BEFORE the quiet-time check: during Do Not Disturb the
  // notification counts as delivered and must not buzz the moment it ends.
  s_notified_window_ts = start;
  persist_write_int(NOTIFY_PERSIST_KEY, (int32_t)start);

  if (quiet_time_is_active()) {
    APP_LOG(APP_LOG_LEVEL_INFO, "light window in %d min: silent (quiet time)",
            (int)((start - now_sec) / 60));
    return;
  }
  APP_LOG(APP_LOG_LEVEL_INFO, "light window in %d min: buzzing",
          (int)((start - now_sec) / 60));
  vibes_double_pulse(); // double: the short pulse is the stopwatch auto-stop
}

static bool packet_stale_now(void) {
  return packet_is_stale(s_packet.data_ts, (uint32_t)time(NULL),
                         STALE_THRESHOLD_SEC);
}

// --- Screen switching ---
static void enter_mode(Mode m) {
  s_mode = m;

  // Any data screen, not just Astro: cloud cover and the freshness icon live on
  // Clock, and that is where they are seen going stale.
  if ((m == MODE_CLOCK || m == MODE_ASTRO) && packet_stale_now()) {
    request_refresh();
  }

  if (m == MODE_ASTRO) {
    schedule_astro_timer();
  } else {
    cancel_astro_timer();
  }

  // The auto-exit exists only on the stopwatch screen and only before a
  // measurement starts. Entering the screen does NOT touch a running or stopped
  // measurement.
  if (m == MODE_STOPWATCH && s_sw_state == SW_IDLE) {
    schedule_sw_idle_timer();
  } else {
    cancel_sw_idle_timer();
  }
}

// The stopwatch was switched off in settings — clean up after it immediately.
//
// The switch can catch any state, including a running measurement with the user
// standing on that very screen. Without the cleanup the 100 ms ticker outlives
// its own controls, and the screen becomes a trap: a tap from SW_IDLE goes to
// SW_RUN rather than out, and with StopwatchIdleTimeout = 0 nothing else leaves
// it either.
static void stopwatch_disable(void) {
  sw_stop_ticker();
  s_sw_state = SW_IDLE;
  s_sw_elapsed_ms = 0;
  // enter_mode clears the auto-exit timer; off the stopwatch screen there is none.
  if (s_mode == MODE_STOPWATCH) enter_mode(MODE_CLOCK);
}

// Tap control was switched off in settings — release the accelerometer and park
// the watchface on the Clock.
//
// The subscription must go at RUNTIME rather than merely be skipped at start-up:
// the setting arrives from the phone well after init().
//
// Parking is the same trap as a disabled stopwatch, with no way out at all: the
// auto-returns that would otherwise rescue the user are legitimately switchable
// off (AstroTimeout = 0, StopwatchIdleTimeout = 0), and without the
// accelerometer nothing moves the screen afterwards.
static void tap_control_disable(void) {
  input_unsubscribe();
  stopwatch_disable(); // stops a running measurement and leaves the screen
  if (s_mode != MODE_CLOCK) enter_mode(MODE_CLOCK);
}

// --- The contextual tap ---
// The only gesture available, so it both cycles through the screens and drives
// the stopwatch, including leaving it (otherwise the screen could not be exited).
static void handle_tap_cycle(void) {
  switch (s_mode) {
    case MODE_CLOCK:
      enter_mode(MODE_ASTRO);
      break;

    case MODE_ASTRO:
      // The stopwatch is off in settings — the cycle closes over two screens.
      if (!s_settings.show_stopwatch) {
        enter_mode(MODE_CLOCK);
        break;
      }
      // Entering the stopwatch: reset to Idle (enter_mode arms the auto-exit).
      s_sw_state = SW_IDLE;
      s_sw_elapsed_ms = 0;
      enter_mode(MODE_STOPWATCH);
      break;

    case MODE_STOPWATCH:
      switch (s_sw_state) {
        case SW_IDLE:
          cancel_sw_idle_timer();
          s_sw_state = SW_RUN;
          sw_start_ticker();
          break;
        case SW_RUN:
          s_sw_state = SW_STOPPED;
          sw_stop_ticker();
          break;
        case SW_STOPPED:
        default:
          // Reset and leave for Clock.
          s_sw_state = SW_IDLE;
          s_sw_elapsed_ms = 0;
          sw_stop_ticker();
          enter_mode(MODE_CLOCK);
          break;
      }
      break;

    default:
      s_mode = MODE_CLOCK;
      break;
  }
}

// --- Single entry point for gestures (the source is hidden in input.c) ---
static void handle_gesture(Gesture gesture) {
  (void)gesture; // the source emits a single gesture: a strike on the case
  handle_tap_cycle();

  // Auto-return and auto-exit are INACTIVITY timeouts: if a gesture left us on
  // the same screen, the countdown restarts rather than continuing.
  if (s_mode == MODE_ASTRO) schedule_astro_timer();
  if (s_mode == MODE_STOPWATCH && s_sw_state == SW_IDLE) schedule_sw_idle_timer();

  redraw();
}

// --- Watch events ---
static void tick_handler(struct tm *tick_time, TimeUnits units_changed) {
  s_now_tm = *tick_time;
  check_light_notify((uint32_t)time(NULL));
  redraw();
}

static void connection_handler(bool connected) {
  bool restored = (connected && !s_connected);
  s_connected = connected;
  // While the link was down our own requests were dropped in request_refresh().
  // The moment it returns is the only point to catch up — otherwise the screen
  // shows stale data until the next screen change or poll, up to 3 hours away.
  if (restored && packet_stale_now()) {
    APP_LOG(APP_LOG_LEVEL_INFO, "connection restored, data stale: refreshing");
    request_refresh();
  }
  redraw();
}

// The Timeline Quick View peek appeared or went away — re-run the layout.
static void unobstructed_change_handler(void *ctx) {
  redraw();
}

// --- AppMessage ---
static void inbox_received_callback(DictionaryIterator *iter, void *context) {
  bool packet_changed = packet_update_from_dict(&s_packet, iter);
  bool settings_changed = settings_update_from_dict(&s_settings, iter);

  if (packet_changed) {
    save_packet();
    s_refreshing = false;
    if (s_refresh_timeout) {
      app_timer_cancel(s_refresh_timeout);
      s_refresh_timeout = NULL;
    }
  }
  if (settings_changed) {
    settings_save(&s_settings);
    // Tap control is handled FIRST, a disabled stopwatch second: both cleanups
    // move us to Clock, so the timeout re-arming branches below will not fire
    // afterwards — which is exactly right. Both calls are also reached on every
    // unrelated settings change, hence both are idempotent.
    if (s_settings.tap_control) {
      input_subscribe(handle_gesture);
    } else {
      tap_control_disable();
    }
    if (!s_settings.show_stopwatch) {
      stopwatch_disable();
    }
    // Apply a new timeout IMMEDIATELY if the user is already on the screen that
    // uses it; otherwise it would only take effect on the next entry there. At 0
    // the schedule_* call simply cancels the timer, so "off" applies at once too.
    if (s_mode == MODE_ASTRO) {
      schedule_astro_timer();
    }
    if (s_mode == MODE_STOPWATCH && s_sw_state == SW_IDLE) {
      schedule_sw_idle_timer();
    }
  }
  if (packet_changed || settings_changed) {
    // Checked right away rather than at the next minute tick: the alert may have
    // just been enabled with the window already closer than the lead time, and
    // then it has to buzz now or the moment is missed.
    check_light_notify((uint32_t)time(NULL));
    redraw();
  }
}

static void inbox_dropped_callback(AppMessageResult reason, void *context) {
  APP_LOG(APP_LOG_LEVEL_ERROR, "AppMessage dropped: %d", (int)reason);
}

// --- Window ---
static void window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);
  s_canvas = layer_create(bounds);
  layer_set_update_proc(s_canvas, canvas_update_proc);
  layer_add_child(root, s_canvas);
}

static void window_unload(Window *window) {
  layer_destroy(s_canvas);
  s_canvas = NULL;
}

static void init(void) {
  settings_load(&s_settings);
  load_cached_packet();
  if (persist_exists(NOTIFY_PERSIST_KEY)) {
    s_notified_window_ts = (uint32_t)persist_read_int(NOTIFY_PERSIST_KEY);
  }

  // Seed the time cache before the first draw, or Clock would show 00:00 until
  // the first tick. tick_handler updates it every minute afterwards.
  time_t now0 = time(NULL);
  struct tm *lt0 = localtime(&now0);
  if (lt0) s_now_tm = *lt0;

  // With tap control off the subscription is never created — that, and not a
  // branch anywhere later, is what saves the battery. The STORED value decides
  // here, since the phone's copy only arrives minutes into the session;
  // inbox_received_callback then raises or drops the subscription at runtime.
  if (s_settings.tap_control) input_subscribe(handle_gesture);

  s_window = window_create();
  window_set_background_color(s_window, GColorBlack);
  window_set_window_handlers(s_window, (WindowHandlers) {
    .load = window_load,
    .unload = window_unload,
  });
  window_stack_push(s_window, true);

  tick_timer_service_subscribe(MINUTE_UNIT, tick_handler);

  // The Timeline Quick View peek changes the usable screen area at runtime.
  unobstructed_area_service_subscribe((UnobstructedAreaHandlers) {
    .did_change = unobstructed_change_handler,
  }, NULL);

  connection_service_subscribe((ConnectionHandlers) {
    .pebble_app_connection_handler = connection_handler,
  });
  s_connected = connection_service_peek_pebble_app_connection();

  app_message_register_inbox_received(inbox_received_callback);
  app_message_register_inbox_dropped(inbox_dropped_callback);
  // Inbox: 28 keys at ~11 bytes each (tuple header + uint32) no longer fit the
  // previous 256 — a packet would silently land in inbox_dropped. 512 leaves room
  // to spare and the app heap can afford it (~122 KB free). The outbox is
  // unchanged: the only key going out is Refresh.
  app_message_open(512, 64);

  APP_LOG(APP_LOG_LEVEL_INFO, "heap_bytes_free after init: %d",
          (int)heap_bytes_free());
}

static void deinit(void) {
  sw_stop_ticker();
  cancel_astro_timer();
  cancel_sw_idle_timer();
  if (s_refresh_timeout) app_timer_cancel(s_refresh_timeout);
  input_unsubscribe();
  tick_timer_service_unsubscribe();
  unobstructed_area_service_unsubscribe();
  connection_service_unsubscribe();
  window_destroy(s_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}


// icons.c — see icons.h.
//
// THE GRID. The glyphs are designed on a 16×16 grid with a step of 0.1, so
// coordinates here are kept in TENTHS OF A CELL: 16.0 cells = 160. Conversion to
// pixels happens once, in len(): an icon lives in a size×size square, and the
// same geometry serves GOTHIC_14 and GOTHIC_24 alike.
//
// ARCS are drawn with graphics_fill_radial at a 1 px thickness rather than
// graphics_draw_arc: draw_arc has no thickness parameter, while fill_radial is
// the same primitive that draws the day ring here, with behaviour already
// verified on all six platforms.
//
// ANGLES follow the ring: 0° at the top, clockwise. The source arcs are SVG
// (large-arc/sweep flags) and are converted here into angle pairs; the original
// path is quoted next to each glyph so the conversion can be checked.

#include "icons.h"
#include "geom.h"

// Stroke width. Exactly 1 px, on a 24 px icon too: the SDK honours ODD values
// only (icons.h) and the next available one is 3 px — an eighth of the icon
// height. There is no way to get the 1.5 px stroke the design asks for.
#define ICON_STROKE 1

typedef struct { int x, y, s; } Box;

// Pixel length from a length in tenths of a cell.
static int len(const Box *b, int t) { return (t * b->s + 80) / 160; }
static int gx(const Box *b, int t) { return b->x + len(b, t); }
static int gy(const Box *b, int t) { return b->y + len(b, t); }

static void ln(GContext *ctx, const Box *b, int x0, int y0, int x1, int y1) {
  graphics_draw_line(ctx, GPoint(gx(b, x0), gy(b, y0)),
                     GPoint(gx(b, x1), gy(b, y1)));
}

// Circular arc: centre and radius in cells, angles in degrees.
static void arc(GContext *ctx, const Box *b, int cx, int cy, int r, int a0,
                int a1) {
  int R = len(b, r);
  if (R < 1) R = 1;
  if (a1 <= a0) a1 += 360;
  graphics_fill_radial(ctx, GRect(gx(b, cx) - R, gy(b, cy) - R, 2 * R, 2 * R),
                       GOvalScaleModeFitCircle, ICON_STROKE,
                       DEG_TO_TRIGANGLE(a0), DEG_TO_TRIGANGLE(a1));
}

static void disc(GContext *ctx, const Box *b, int cx, int cy, int r) {
  graphics_fill_circle(ctx, GPoint(gx(b, cx), gy(b, cy)), len(b, r));
}

static void hline(GContext *ctx, int x0, int x1, int y) {
  if (x1 < x0) return;
  graphics_draw_line(ctx, GPoint(x0, y), GPoint(x1, y));
}

// --- Fills for compound shapes ---
//
// Three silhouettes cannot be assembled from circles and rectangles: the eye
// lens, the moon crescent and the terminator. All three are filled ROW BY ROW,
// computing the half-width of the shape on every row. That is also why isqrt32
// lives in geom.c: a frame adds up to nearly a hundred such rows.

// Lens = intersection of two circles of radius r centred at (cx, cy±d).
static void fill_lens(GContext *ctx, const Box *b, int cx, int cy, int r,
                      int d) {
  int CX = gx(b, cx), CY = gy(b, cy), R = len(b, r), D = len(b, d);
  int half = R - D;
  for (int dy = -half; dy <= half; dy++) {
    int a = (dy < 0 ? -dy : dy) + D;
    int hw = (int)isqrt32((int32_t)R * R - (int32_t)a * a);
    hline(ctx, CX - hw, CX + hw, CY + dy);
  }
}

// Crescent = inside circle A and outside circle B (B sits to the right of A).
static void fill_lune(GContext *ctx, const Box *b, int acx, int acy, int ar,
                      int bcx, int bcy, int br) {
  int ACX = gx(b, acx), ACY = gy(b, acy), AR = len(b, ar);
  int BCX = gx(b, bcx), BCY = gy(b, bcy), BR = len(b, br);
  for (int dy = -AR; dy <= AR; dy++) {
    int hw = (int)isqrt32((int32_t)AR * AR - (int32_t)dy * dy);
    int y = ACY + dy, x0 = ACX - hw, x1 = ACX + hw;
    int ddy = y - BCY;
    if (ddy > -BR && ddy < BR) {
      int wb = (int)isqrt32((int32_t)BR * BR - (int32_t)ddy * ddy);
      if (BCX - wb - 1 < x1) x1 = BCX - wb - 1;
    }
    hline(ctx, x0, x1, y);
  }
}

// Terminator: the lit fraction of the disc. The light/shadow boundary is half an
// ellipse with a horizontal semi-axis of r·|1−2k|, so on a row of half-chord hw
// it sits at (1−2k)·hw from the centre. For k > 0.5 that value is negative and
// the lit part reaches past the centre — the same formula yields the gibbous
// shape as well as the crescent.
static void fill_moon(GContext *ctx, const Box *b, int cx, int cy, int r,
                      int illum, bool waxing) {
  int CX = gx(b, cx), CY = gy(b, cy), R = len(b, r);
  for (int dy = -R; dy <= R; dy++) {
    int hw = (int)isqrt32((int32_t)R * R - (int32_t)dy * dy);
    int xt = ((100 - 2 * illum) * hw) / 100;
    int x0 = waxing ? CX + xt : CX - hw;
    int x1 = waxing ? CX + hw : CX - xt;
    hline(ctx, x0, x1, CY + dy);
  }
}

// --- Glyphs ---
//
// The rise and set arrows are factored out: they are identical for the sun and
// the moon up to position and direction.
static void arrow_up(GContext *ctx, const Box *b, int x, int y_tip, int y_tail,
                     int wing) {
  ln(ctx, b, x, y_tail, x, y_tip);
  ln(ctx, b, x - wing, y_tip + wing, x, y_tip);
  ln(ctx, b, x, y_tip, x + wing, y_tip + wing);
}

static void arrow_down(GContext *ctx, const Box *b, int x, int y_tip,
                       int y_tail, int wing) {
  ln(ctx, b, x, y_tail, x, y_tip);
  ln(ctx, b, x - wing, y_tip - wing, x, y_tip);
  ln(ctx, b, x, y_tip, x + wing, y_tip - wing);
}

// Chain of two links. Fresh — the links interlock (their middles overlap);
// stale — they are pulled apart and open outwards. The cue is the SILHOUETTE and
// not colour, otherwise the four states would collapse into one on diorite and
// flint.
static void draw_chain(GContext *ctx, const Box *b, bool linked) {
  if (linked) {
    // "M7 5.5A2.6 2.6 0 0 1 7 10.5H5A2.5 2.5 0 0 1 5 5.5" and its mirror.
    arc(ctx, b, 63, 80, 26, 16, 164);
    ln(ctx, b, 70, 105, 50, 105);
    arc(ctx, b, 50, 80, 25, 180, 360);
    arc(ctx, b, 97, 80, 26, 196, 344);
    ln(ctx, b, 90, 55, 110, 55);
    arc(ctx, b, 110, 80, 25, 0, 180);
  } else {
    // "M5.6 5.5A2.6 2.6 0 0 0 5.6 10.5H4.2A2.5 2.5 0 0 1 4.2 5.5" and its mirror.
    arc(ctx, b, 63, 80, 26, 196, 344);
    ln(ctx, b, 56, 105, 42, 105);
    arc(ctx, b, 42, 80, 25, 180, 360);
    arc(ctx, b, 97, 80, 26, 16, 164);
    ln(ctx, b, 104, 55, 118, 55);
    arc(ctx, b, 118, 80, 25, 0, 180);
  }
}

// --- Seating an icon on a text row (see icons.h) ---
//
// The extreme rows of each glyph's DRAWING inside the square, in tenths of a
// cell (the square itself is 0..160). Taken from the glyph geometry below and
// checked against screenshots: predicted rows matched actual ones pixel for
// pixel.
static const uint8_t ICON_INK[ICON_GLYPH_COUNT][2] = {
  [ICON_CLOUD]     = {  45, 119 },  // the bump discs hang below the flat base
  [ICON_WIND]      = {  19, 151 },  // hooks of the top and bottom streams
  [ICON_EYE]       = {  53, 107 },  // the lens is symmetric
  [ICON_SUNRISE]   = {  16, 120 },  // nothing is drawn below the horizon line
  [ICON_SUNSET]    = {  16, 120 },
  [ICON_GOLD]      = {  34, 132 },
  [ICON_BLUE]      = {  34, 132 },
  [ICON_MOONRISE]  = {  38, 126 },
  [ICON_MOONSET]   = {  38, 126 },
  [ICON_TIMER]     = {  22, 144 },
  [ICON_LINK]      = {  54, 106 },
  [ICON_LINKSTALE] = {  54, 106 },
  [ICON_NOLINK]    = {  20, 140 },  // the strike-through is symmetric
  [ICON_SYNC]      = {  45, 115 },
};

// The moon disc fills the square symmetrically: r 6.4 cells around the centre.
static const uint8_t MOON_INK[2] = { 16, 144 };

// Everything is computed in DOUBLED pixels: both the cap-height centre and the
// drawing centre can fall on a half pixel, and two roundings in a row cost a
// whole one.
//
// The GOTHIC cap-height centre is 0.665 of the point size from the top of the
// line box (measured: 9 / 12 / 16.5 / 18.5 px at 14 / 18 / 24 / 28 — see
// icons.h).
#define CAP_CENTER2(size) ((4 * (size)) / 3)

// The drawing centre is taken from the ROUNDED edges, not from ideal geometry.
// len() rounds each edge separately: on an 18 px icon the sunrise drawing at
// 16..120 lands on rows 2..14, so its centre is 0.5 px above the square's centre
// rather than the 1.35 px the continuous grid predicts. Measured on emery, a
// correction based on the ideal centre pushed sunrise and sunset 1 px below
// their labels — an error of the same size as the defect it was fixing.
static int ink_center2(const uint8_t ink[2], int size) {
  Box b = { 0, 0, size };
  return len(&b, ink[0]) + len(&b, ink[1]);
}

static int text_dy(const uint8_t ink[2], int size) {
  return (CAP_CENTER2(size) - ink_center2(ink, size) + 1) / 2;
}

int icon_text_dy(IconGlyph g, int size) {
  if (g >= ICON_GLYPH_COUNT || size < 8) return 0;
  return text_dy(ICON_INK[g], size);
}

int icon_moon_text_dy(int size) {
  return (size < 8) ? 0 : text_dy(MOON_INK, size);
}

void icon_draw(GContext *ctx, IconGlyph g, GPoint origin, int size, GColor color,
               IconLook look) {
  if (size < 8) return;
  Box box = { origin.x, origin.y, size };
  const Box *b = &box;
  bool solid = (look == ICON_SOLID);

  graphics_context_set_stroke_color(ctx, color);
  graphics_context_set_fill_color(ctx, color);
  graphics_context_set_stroke_width(ctx, ICON_STROKE);

  switch (g) {
    case ICON_CLOUD:
      // "M4.4 11.6A2.6 … A3.7 … A2.4 … Z": three bumps and a flat base.
      if (solid) {
        disc(ctx, b, 55, 92, 26);
        disc(ctx, b, 84, 82, 37);
        disc(ctx, b, 109, 93, 24);
        graphics_fill_rect(ctx,
                           GRect(gx(b, 44), gy(b, 85), len(b, 72), len(b, 31)),
                           0, GCornerNone);
      } else {
        arc(ctx, b, 55, 92, 26, 205, 349);
        arc(ctx, b, 84, 82, 37, 294, 432);
        arc(ctx, b, 109, 93, 24, 24, 164);
        ln(ctx, b, 44, 116, 116, 116);
      }
      break;

    case ICON_WIND:
      // Three streams; the top and bottom ones hook round by 270°.
      ln(ctx, b, 20, 55, 92, 55);
      arc(ctx, b, 92, 37, 18, 270, 540);
      ln(ctx, b, 20, 85, 114, 85);
      ln(ctx, b, 20, 115, 86, 115);
      arc(ctx, b, 86, 133, 18, 0, 270);
      break;

    case ICON_EYE: {
      // Lens: two arcs of radius 9.42 centred at (8, 8±6.72) — the design's
      // cubic curve converted into a circle through its sagitta of 2.7.
      //
      // MEASURED on basalt: the pupil radius must NOT be taken straight from the
      // grid. At 14 px the half-height of the lens rounds to 2 px and a pupil of
      // 1.9 cells rounds to 2 px as well — the pupil eats the whole lens and the
      // filled eye is left as two corners reading like "+ +". The pupil radius is
      // therefore derived FROM the actual lens half-height, not from the grid.
      int half = len(b, 94) - len(b, 67);
      int pr = len(b, 19);
      if (pr > half - 1) pr = half - 1;
      if (pr < 1) pr = 1;
      GPoint c = GPoint(gx(b, 80), gy(b, 80));
      if (solid) {
        fill_lens(ctx, b, 80, 80, 94, 67);
        graphics_context_set_fill_color(ctx, GColorBlack);
        graphics_fill_circle(ctx, c, pr);
      } else {
        arc(ctx, b, 80, 147, 94, 316, 405);
        arc(ctx, b, 80, 13, 94, 135, 225);
        graphics_draw_circle(ctx, c, pr);
      }
      break;
    }

    case ICON_SUNRISE:
      ln(ctx, b, 25, 120, 135, 120);
      arc(ctx, b, 80, 120, 34, 270, 450);
      arrow_up(ctx, b, 80, 16, 54, 21);
      break;

    case ICON_SUNSET:
      ln(ctx, b, 25, 120, 135, 120);
      arc(ctx, b, 80, 120, 34, 270, 450);
      arrow_down(ctx, b, 80, 54, 16, 21);
      break;

    // Golden and blue differ by SILHOUETTE, not by detail: a disc entirely above
    // the horizon line against one entirely below it. At 14 px a difference in
    // rays would be invisible; a difference in composition reads instantly.
    case ICON_GOLD:
      ln(ctx, b, 15, 132, 145, 132);
      if (solid) disc(ctx, b, 80, 76, 42); else arc(ctx, b, 80, 76, 42, 0, 360);
      break;

    case ICON_BLUE:
      ln(ctx, b, 15, 34, 145, 34);
      if (solid) disc(ctx, b, 80, 90, 42); else arc(ctx, b, 80, 90, 42, 0, 360);
      break;

    case ICON_MOONRISE:
    case ICON_MOONSET:
      // Crescent: circle (5.3, 8) r 4.2 minus circle (9.6, 8) r 5.0.
      if (solid) {
        fill_lune(ctx, b, 53, 80, 42, 96, 80, 50);
      } else {
        arc(ctx, b, 53, 80, 42, 162, 378);
        arc(ctx, b, 96, 80, 50, 217, 323);
      }
      if (g == ICON_MOONRISE) arrow_up(ctx, b, 120, 44, 126, 21);
      else                    arrow_down(ctx, b, 120, 126, 44, 21);
      break;

    case ICON_TIMER:
      arc(ctx, b, 80, 90, 54, 0, 360);
      ln(ctx, b, 80, 90, 80, 56);
      ln(ctx, b, 62, 22, 98, 22);
      ln(ctx, b, 80, 22, 80, 36);
      break;

    case ICON_LINK:
      draw_chain(ctx, b, true);
      break;

    case ICON_LINKSTALE:
      draw_chain(ctx, b, false);
      break;

    case ICON_NOLINK:
      draw_chain(ctx, b, true);
      ln(ctx, b, 30, 140, 130, 20);
      break;

    case ICON_SYNC:
      ln(ctx, b, 24, 45, 56, 80);
      ln(ctx, b, 56, 80, 24, 115);
      ln(ctx, b, 72, 45, 104, 80);
      ln(ctx, b, 104, 80, 72, 115);
      ln(ctx, b, 120, 45, 152, 80);
      ln(ctx, b, 152, 80, 120, 115);
      break;

    default:
      break;
  }
}

void icon_draw_moon(GContext *ctx, GPoint origin, int size, GColor color,
                    int illum_pct, bool waxing) {
  if (size < 8) return;
  Box box = { origin.x, origin.y, size };
  const Box *b = &box;
  if (illum_pct < 0) illum_pct = 0;
  if (illum_pct > 100) illum_pct = 100;

  graphics_context_set_stroke_color(ctx, color);
  graphics_context_set_fill_color(ctx, color);
  graphics_context_set_stroke_width(ctx, ICON_STROKE);

  // Full moon is a solid disc, new moon just the limb: at those fractions the
  // terminator degenerates and its half-ellipse would collapse into a line.
  if (illum_pct >= 99) {
    disc(ctx, b, 80, 80, 64);
    return;
  }
  arc(ctx, b, 80, 80, 64, 0, 360);
  if (illum_pct <= 1) return;
  fill_moon(ctx, b, 80, 80, 64, illum_pct, waxing);
}

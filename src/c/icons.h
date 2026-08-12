// icons.h — the on-screen icon pack, drawn with graphics primitives.
//
// WHY PRIMITIVES AND NOT PDC. Three independent reasons, any one of which would
// be enough on its own:
//
//   1. PDC has neither Bézier curves nor ARCS — only point paths and circles.
//      Thirteen of the fifteen glyphs are built from arcs, so each would have to
//      be approximated by hand with a polyline.
//   2. PDC draws 1:1 and gdraw_command_image has no scaling API. An icon must
//      match the point size of the text beside it, and across six platforms
//      there are three sizes (14 / 18 / 24) — that is not 15 files, nor even 30,
//      but 15 glyphs × 2 looks × 3 sizes = 90 flash resources.
//   3. The moon phase is data, not a pictogram: the terminator is built from the
//      illumination fraction in the packet and is drawn by code regardless.
//
// As primitives all of this costs zero bytes of flash, and colour and size are
// chosen at call time.
//
// NOTE: graphics_context_set_stroke_width honours ODD values only (pebble.h:4129)
// — an even width is stored but rounded down when drawing. So a design calling
// for "1.4 px outline against a 2.0 px solid" does not exist on the device; both
// land on 1 px. The two looks therefore differ EXACTLY by whether the silhouette
// is filled, and only for six glyphs: cloud, eye, gold, blue, moonrise, moonset.
// The other nine are byte-for-byte identical in both looks.

#pragma once
#include <pebble.h>

typedef enum {
  ICON_CLOUD = 0,   // cloud cover
  ICON_WIND,        // wind
  ICON_EYE,         // visibility
  ICON_SUNRISE,     // sunrise
  ICON_SUNSET,      // sunset
  ICON_GOLD,        // golden hour: disc entirely ABOVE the horizon line
  ICON_BLUE,        // blue hour: disc entirely BELOW the horizon line
  ICON_MOONRISE,    // moonrise
  ICON_MOONSET,     // moonset
  ICON_TIMER,       // stopwatch
  ICON_LINK,        // data fresh: links closed
  ICON_LINKSTALE,   // data stale: links open
  ICON_NOLINK,      // no connection: links struck through
  ICON_SYNC,        // update in flight: chevrons
  ICON_GLYPH_COUNT
} IconGlyph;

typedef enum { ICON_OUTLINE = 0, ICON_SOLID } IconLook;

// Gap between an icon and its text.
#define ICON_GAP 3

// How much room an icon takes in a row, gap included. This exact term is added
// to the fit checks.
#define ICON_ADVANCE(size) ((size) + ICON_GAP)

// How far to LOWER the icon square relative to the vertical centre of its band
// so the drawing sits on the cap-height centre of the text beside it.
//
// MEASURED on emery. For the system GOTHIC fonts the line box equals the
// nominal point size exactly (graphics_text_layout_get_content_size().h =
// 14/18/24/28), but the glyphs do NOT sit centred in it: the top 0.36 of the
// size is empty space reserved for diacritics, and the baseline coincides with
// the LAST row of the box — descenders ("g", "y") extend 2–4 px past its bottom
// edge. The cap-height centre therefore lies 18–21 % of the size BELOW the box
// centre: +2.5 / +3.5 / +5.0 / +5.0 px at 14/18/24/28. An icon centred
// geometrically in the band hung about 3 px above its own value on both screens.
//
// The second term is the glyph itself: on sunrise and sunset the drawing only
// occupies the upper three quarters of the square (arrow on top, horizon line at
// the bottom). Hence the correction is per glyph, not one constant for the pack.
int icon_text_dy(IconGlyph g, int size);

// The same for the moon glyph (icon_draw_moon): its disc fills the square
// symmetrically, so only the cap-height correction remains.
int icon_moon_text_dy(int size);

// The icon is inscribed in a size×size square with its top-left corner at
// origin. Size is the point size of the neighbouring text (14 / 18 / 24), and
// the colour is that of its own row.
void icon_draw(GContext *ctx, IconGlyph g, GPoint origin, int size, GColor color,
               IconLook look);

// Moon: the terminator is built from the illumination fraction rather than
// picked from eight ready-made pictures. illum_pct is the packet's moon_illum
// (0..100), waxing tells whether the moon is growing (moon_phase < 4). The glyph
// is identical in both looks.
void icon_draw_moon(GContext *ctx, GPoint origin, int size, GColor color,
                    int illum_pct, bool waxing);

// icons.h — the on-screen icon pack, drawn with graphics primitives.
//
// Primitives rather than PDC resources: PDC has no arcs (thirteen of the
// fourteen glyphs need them), draws 1:1 with no scaling API (three text sizes
// across six platforms), and the moon phase is data rather than a pictogram.
// As primitives all of it costs zero bytes of flash, and colour and size are
// chosen at call time.
//
// NOTE: graphics_context_set_stroke_width honours ODD values only
// (pebble.h:4129) — an even width is stored but rounded down when drawing. A
// design calling for "1.4 px outline against a 2.0 px solid" therefore does not
// exist on the device; both land on 1 px. That is why the glyphs are solid
// silhouettes and not outlines.

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
  ICON_LINK,        // data fresh: links closed
  ICON_LINKSTALE,   // data stale: links open
  ICON_NOLINK,      // no connection: links struck through
  ICON_SYNC,        // update in flight: chevrons
  ICON_GLYPH_COUNT
} IconGlyph;

// Gap between an icon and its text.
#define ICON_GAP 3

// How much room an icon takes in a row, gap included. This exact term is added
// to the fit checks.
#define ICON_ADVANCE(size) ((size) + ICON_GAP)

// How far to LOWER the icon square relative to the vertical centre of its band
// so the drawing sits on the cap-height centre of the text beside it.
//
// MEASURED on emery. The system GOTHIC line box equals the nominal point size,
// but the glyphs do NOT sit centred in it: the top 0.36 of the size is empty
// space reserved for diacritics, so the cap-height centre lies 18–21 % of the
// size BELOW the box centre. An icon centred geometrically hangs about 3 px
// above its own value.
//
// The second term is the glyph itself: on sunrise and sunset the drawing only
// occupies the upper three quarters of the square. Hence the correction is per
// glyph, not one constant for the pack.
int icon_text_dy(IconGlyph g, int size);

// The same for the moon glyph (icon_draw_moon): its disc fills the square
// symmetrically, so only the cap-height correction remains.
int icon_moon_text_dy(int size);

// The icon is inscribed in a size×size square with its top-left corner at
// origin. Size is the point size of the neighbouring text (14 / 18 / 24), and
// the colour is that of its own row.
void icon_draw(GContext *ctx, IconGlyph g, GPoint origin, int size, GColor color);

// Moon: the terminator is built from the illumination fraction rather than
// picked from eight ready-made pictures. illum_pct is the packet's moon_illum
// (0..100), waxing tells whether the moon is growing (moon_phase < 4).
void icon_draw_moon(GContext *ctx, GPoint origin, int size, GColor color,
                    int illum_pct, bool waxing);

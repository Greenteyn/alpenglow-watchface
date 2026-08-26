// input.c — see input.h.
//
// The only input channel is a tap detector built on accel_data.
//
// WHY NOT TOUCH (measured on Pebble Time 2): the firmware does not deliver
// touches to watchfaces. touch_service_subscribe() is gated by
// `if (sys_app_is_watchface()) return NULL;` and then returns silently — no
// error, no log. touch_service_is_enabled() does NOT pass through that gate and
// reports the global Settings → Display → Touch switch instead, so it answers
// true while no event is ever delivered.

#include "input.h"

static InputGestureHandler s_handler = NULL;
static bool s_subscribed = false;

// Bench counters — see input.h.
static uint32_t s_dbg_callbacks = 0;
static uint32_t s_dbg_taps = 0;

static void emit(Gesture gesture) {
  if (s_handler) s_handler(gesture);
}

// ---------------------------------------------------------------------------
// Tap detector (jerk over accel_data)
// ---------------------------------------------------------------------------
// accel_tap_service has no threshold control and misses most taps on real
// hardware — in the emulator emu-tap injects a perfect event, so the problem
// stays invisible there. Hence a custom detector on jerk = |Δ magnitude|.
//
// THE SIGNAL IS PULSE SHAPE, NOT AMPLITUDE: a finger strike is a short spike
// back to rest within a sample or two, while an arm swing keeps the jerk high
// for many samples in a row. Amplitude alone cannot separate them. So only a
// spike that dies out within TAP_MAX_WIDTH counts, and the threshold sits down
// at noise level because shape does the filtering.
//
// The thresholds are TIED TO THE SAMPLING RATE. Change it and measure the peak
// jerk again; values from 25 Hz do not carry over. 100 Hz is what makes the
// scheme work: a strike lasts a few milliseconds, so at 25 Hz whether the quiet
// part lands on a sample is luck, and smooth motion produces far larger
// sample-to-sample differences (the walking peak fell from 3187 to ~250).
#define TAP_JERK_THRESHOLD     2000 // pulse entry; shape does the filtering
#define TAP_MAX_WIDTH          6    // wider than ~60 ms = motion, not a strike
#define TAP_SAMPLES_PER_UPDATE 10   // 100 Hz / 10 → callback 10 times a second
#define TAP_QUIET_JERK         1200 // "quiet": hand at rest
#define TAP_QUIET_SAMPLES      10   // 10 quiet samples in a row (~100 ms) = rearm
#define TAP_REARM_MAX_SAMPLES  100  // ...but no longer than ~1 s, or it sticks

static int32_t s_prev_mag = 0;
static bool s_mag_init = false;
static bool s_tap_armed = true; // false = waiting for quiet after a trigger
static int s_quiet_run = 0;     // quiet samples collected in a row so far
static int s_disarmed_run = 0;  // samples spent waiting (for the safety net)
static bool s_in_pulse = false; // inside a spike: waiting to see if it dies out
static int s_pulse_width = 0;   // length of the current spike, in samples
static int32_t s_max_jerk = 0;  // peak jerk this session (input_max_jerk)

// Back to the state input_subscribe() starts from. Called on every path that
// abandons an analysis in progress, so no half-finished pulse survives into the
// next one.
static void detector_reset(void) {
  s_mag_init = false;
  s_tap_armed = true;
  s_quiet_run = 0;
  s_disarmed_run = 0;
  s_in_pulse = false;
  s_pulse_width = 0;
}

static void accel_data_handler(AccelData *data, uint32_t num_samples) {
  s_dbg_callbacks++;
  for (uint32_t i = 0; i < num_samples; i++) {
    // A spike from the vibration motor is not a tap. The whole analysis is
    // dropped, not just the arming: a pulse left open here would be closed by
    // the first quiet sample after the buzz and reported as a strike.
    if (data[i].did_vibrate) {
      detector_reset();
      s_tap_armed = false; // wait for quiet, as after a normal trigger
      continue;
    }
    int32_t ax = data[i].x, ay = data[i].y, az = data[i].z;
    int32_t mag = (ax < 0 ? -ax : ax) + (ay < 0 ? -ay : ay) + (az < 0 ? -az : az);
    if (!s_mag_init) { s_prev_mag = mag; s_mag_init = true; continue; }
    int32_t jerk = mag - s_prev_mag;
    if (jerk < 0) jerk = -jerk;
    s_prev_mag = mag;
    if (jerk > s_max_jerk) s_max_jerk = jerk; // record the peak before any gate

    if (!s_tap_armed) {
      // Rearm on QUIET rather than on a timer: a fixed debounce revived in the
      // middle of ongoing motion and a single walk produced a burst of events.
      if (jerk < TAP_QUIET_JERK) {
        if (++s_quiet_run >= TAP_QUIET_SAMPLES) {
          s_tap_armed = true; s_quiet_run = 0; s_disarmed_run = 0;
        }
      } else {
        s_quiet_run = 0;
      }
      // ...but no longer than the safety net: with the arm held up the jerk
      // never settles, and without this the detector would stick forever.
      if (++s_disarmed_run >= TAP_REARM_MAX_SAMPLES) {
        s_tap_armed = true; s_quiet_run = 0; s_disarmed_run = 0;
      }
      continue;
    }

    if (!s_in_pulse) {
      if (jerk > TAP_JERK_THRESHOLD) { s_in_pulse = true; s_pulse_width = 0; }
      continue;
    }

    s_pulse_width++;
    if (jerk < TAP_QUIET_JERK) {
      // The spike died out in time → this was a strike on the case.
      s_in_pulse = false;
      s_tap_armed = false;
      s_quiet_run = 0;
      s_disarmed_run = 0;
      s_dbg_taps++;
      emit(GESTURE_TAP);
    } else if (s_pulse_width >= TAP_MAX_WIDTH) {
      // The jerk stays high → arm motion. Not a tap, and we do NOT disarm:
      // otherwise a real tap while walking could never be caught.
      s_in_pulse = false;
    }
  }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void input_subscribe(InputGestureHandler handler) {
  s_handler = handler;
  if (s_subscribed) return;

  detector_reset();
  // Cleared here and NOT in detector_reset(): the peak measures a whole wearing
  // session, and detector_reset() also runs on every buzz, which would wipe it.
  s_max_jerk = 0;
  accel_data_service_subscribe(TAP_SAMPLES_PER_UPDATE, accel_data_handler);
  accel_service_set_sampling_rate(ACCEL_SAMPLING_100HZ);
  s_subscribed = true;
  APP_LOG(APP_LOG_LEVEL_INFO, "input: accel tap detector armed");
}

void input_unsubscribe(void) {
  if (s_subscribed) {
    accel_data_service_unsubscribe();
    s_subscribed = false;
    APP_LOG(APP_LOG_LEVEL_INFO, "input: accel tap detector released");
  }
  s_handler = NULL;
}

int32_t input_max_jerk(void) {
  return s_max_jerk;
}

uint32_t input_dbg_callbacks(void) {
  return s_dbg_callbacks;
}

uint32_t input_dbg_taps(void) {
  return s_dbg_taps;
}

bool input_dbg_subscribed(void) {
  return s_subscribed;
}

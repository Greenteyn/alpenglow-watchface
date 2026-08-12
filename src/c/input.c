// input.c — see input.h.
//
// The only input channel is a tap detector built on accel_data.
//
// WHY NOT TOUCH (measured on Pebble Time 2): the firmware does not deliver
// touches to watchfaces. touch_service_subscribe() fetches its state through
// prv_get_state(), which for PebbleTask_App is gated by
// `if (sys_app_is_watchface()) return NULL;`, and on NULL the subscription
// silently returns — no error, no log, no return code. touch_service_is_enabled()
// does NOT pass through that gate and reports the global
// Settings → Display → Touch switch instead, so it answers true while no event
// is ever delivered. On-device measurement: supported=1, yet 0 touches in a
// session where the accelerometer produced 19 gestures.

#include "input.h"

static InputGestureHandler s_handler = NULL;

static void emit(Gesture gesture) {
  if (s_handler) s_handler(gesture);
}

// ---------------------------------------------------------------------------
// Tap detector (jerk over accel_data)
// ---------------------------------------------------------------------------
// WHY IT EXISTS: accel_tap_service has no threshold control and misses most
// taps on real hardware. In the emulator emu-tap injects a perfect event, so it
// scores 100% there and the problem stays invisible. Hence a custom detector on
// jerk = |Δ magnitude of acceleration|.
//
// THE SIGNAL IS PULSE SHAPE, NOT AMPLITUDE. Measurements on Pebble Time 2 showed
// that jerk magnitude alone cannot separate a tap from an arm swing: everyday
// noise reaches ~3100 while a comfortable tap sits BELOW 4500 — a window too
// narrow to use, with 4500 losing taps and 3800 producing false ones. The shapes
// differ sharply though: a finger strike is a short spike back to rest within a
// sample or two, an arm swing keeps the jerk high for many samples in a row. So
// only a spike that dies out within TAP_MAX_WIDTH samples counts as a tap, and
// the amplitude threshold sits back down at noise level because shape does the
// filtering.
//
// THREE GUARDS, each closing a failure seen on hardware:
//  1. Rearm on QUIET, not on a timer. The previous debounce revived after a
//     fixed 400 ms — in the middle of ongoing motion — so a single walk produced
//     a burst of events (44 events, ~34 of them false).
//  2. The TAP_REARM_MAX_SAMPLES safety net. Without it the detector could stick
//     forever: with the arm held up the jerk never drops below TAP_QUIET_JERK
//     often enough, so the first tap fired and later ones did not.
//  3. Ignoring did_vibrate: the watch's own buzz otherwise reads as a strike.
//
// THE 100 Hz SAMPLING RATE is a precondition of the scheme, not a detail. At
// 25 Hz samples arrive every 40 ms while a finger strike lasts a few
// milliseconds: two or three samples see it and whether the quiet part lands on
// a sample is luck ("works every other time" was observed with a tap peak of
// 7200 against a threshold of 3000, i.e. with amplitude to spare). 100 Hz wins
// twice: the strike is resolved in time, and smooth motion yields far smaller
// sample-to-sample differences — a hand travels much less in 10 ms than in
// 40 ms. Measured: the walking peak dropped from 3187 (25 Hz) to ~250 (100 Hz),
// roughly a twelvefold gain in separation between noise and strike. The price is
// more frequent sampling; samples are fetched in batches of 10, so the callback
// still runs 10 times per second.
//
// NOTE: the thresholds are tied to the rate. Change it and measure again with
// input_max_jerk(); values from 25 Hz do not carry over. The threshold is only
// the ENTRY into the analysis window — shape decides — which is why it sits low:
// at 100 Hz everyday motion never reaches it.
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
static int32_t s_max_jerk = 0;  // peak jerk this session (threshold calibration)
static bool s_in_pulse = false; // inside a spike: waiting to see if it dies out
static int s_pulse_width = 0;   // length of the current spike, in samples

static void accel_data_handler(AccelData *data, uint32_t num_samples) {
  for (uint32_t i = 0; i < num_samples; i++) {
    // A spike from the vibration motor is not a tap.
    if (data[i].did_vibrate) {
      s_mag_init = false;
      s_tap_armed = false; // wait for quiet, as after a normal trigger
      s_quiet_run = 0;
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
      // Rearm once the hand has settled...
      if (jerk < TAP_QUIET_JERK) {
        if (++s_quiet_run >= TAP_QUIET_SAMPLES) {
          s_tap_armed = true; s_quiet_run = 0; s_disarmed_run = 0;
        }
      } else {
        s_quiet_run = 0;
      }
      // ...but no longer than the safety net, or a raised arm blocks input forever.
      if (++s_disarmed_run >= TAP_REARM_MAX_SAMPLES) {
        s_tap_armed = true; s_quiet_run = 0; s_disarmed_run = 0;
      }
      continue;
    }

    // Classify by pulse SHAPE, not by amplitude alone.
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
      emit(GESTURE_TAP);
    } else if (s_pulse_width >= TAP_MAX_WIDTH) {
      // The jerk stays high → arm motion. Not a tap, and we do NOT disarm:
      // otherwise a real tap while walking could never be caught.
      s_in_pulse = false;
    }
  }
}

static void accel_subscribe(void) {
  accel_data_service_subscribe(TAP_SAMPLES_PER_UPDATE, accel_data_handler);
  accel_service_set_sampling_rate(ACCEL_SAMPLING_100HZ);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void input_subscribe(InputGestureHandler handler) {
  s_handler = handler;
  s_tap_armed = true;
  s_quiet_run = 0;
  s_max_jerk = 0;
  s_in_pulse = false;
  s_pulse_width = 0;

  accel_subscribe();
  APP_LOG(APP_LOG_LEVEL_INFO, "input: accel tap detector armed");
}

void input_unsubscribe(void) {
  accel_data_service_unsubscribe();
  s_handler = NULL;
}

int32_t input_max_jerk(void) {
  return s_max_jerk;
}

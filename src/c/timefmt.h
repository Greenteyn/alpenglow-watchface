// timefmt.h — pure time formatters.

#pragma once
#include <pebble.h>

// HH:MM in local time from Unix seconds. 0 or invalid → "--:--".
// In 12-hour mode: no leading zero and a single-letter half-day marker,
// "5:20a" / "9:30p" (timefmt.c explains why not " AM" / " PM").
// buf must be >= 7 bytes ("12:59p" + '\0').
void timefmt_ts_hm(char *buf, size_t buf_size, uint32_t ts_sec, bool use_24h);

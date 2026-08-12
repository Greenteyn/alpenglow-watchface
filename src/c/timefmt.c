// timefmt.c — see timefmt.h.

#include "timefmt.h"

void timefmt_ts_hm(char *buf, size_t buf_size, uint32_t ts_sec, bool use_24h) {
  if (ts_sec == 0) {
    strncpy(buf, "--:--", buf_size);
    buf[buf_size - 1] = '\0';
    return;
  }
  time_t t = (time_t)ts_sec;
  struct tm *lt = localtime(&t);
  if (use_24h) {
    strftime(buf, buf_size, "%H:%M", lt);
    return;
  }

  // 12-hour mode: without a half-day marker "09:30" is indistinguishable from
  // 21:30. The marker is a SINGLE letter ('a'/'p') and the leading zero is
  // dropped so the string stays as short as the 24-hour one: the Astro screen
  // prints times in pairs ("5:20a-6:14a" is the same 11 characters as
  // "05:20-06:14"), and that row is already tight on space.
  strftime(buf, buf_size, "%I:%M", lt);
  if (buf[0] == '0') memmove(buf, buf + 1, strlen(buf));
  size_t len = strlen(buf);
  if (len + 2 <= buf_size) {
    buf[len] = (lt->tm_hour < 12) ? 'a' : 'p';
    buf[len + 1] = '\0';
  }
}

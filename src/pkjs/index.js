// index.js — pkjs, the phone side: the "brain" of the watchface.
// It runs SunCalc and Open-Meteo, then packs a compact packet of primitives for
// the watch. Everything heavy lives here; the watch only stores and draws.

var Clay = require("@rebble/clay");
var clayConfig = require("./config");
// autoHandleEvents stays at its default (true), and it MATTERS: Clay's built-in
// handler stores the chosen values and PRE-FILLS the settings page with them next
// time it opens. The "settings revert" bug was never this auto channel but the
// parsing of Clay's response by numeric keys (getSettings); that is fixed below
// by parsing e.response BY NAME. The two channels do not conflict — Clay's and
// our buildPacket now carry the same values.
var clay = new Clay(clayConfig);

var SunCalc = require("suncalc");

// --- Debug location override (EMULATOR ONLY) ---
//
// The emulator has no GPS: pypkjs reports coordinates from the host's IP, so the
// behaviour of the watchface during a polar night or on the equator cannot be
// checked from a desk — and those are exactly the places where the astronomy
// breaks down (no windows at all, twilight lasting all night, a 20-minute golden
// hour). Assigning a `{lat, lon}` object makes updateAll() skip
// navigator.geolocation and compute everything for that point.
//
// SET IT BACK TO `null` BEFORE A RELEASE. Hard to forget: while the override is
// active every `pebble build` prints a warning and `RELEASE=1 pebble build`
// fails (the check lives in wscript), and pkjs logs a DEBUG line on every update.
//
// Ready-made points, chosen as edge cases:
//   Moscow     { lat: 55.7558, lon:  37.6173 }  baseline, "ordinary" day
//   Murmansk   { lat: 68.9585, lon:  33.0827 }  polar day/night: no times at all
//   Reykjavik  { lat: 64.1466, lon: -21.9426 }  twilight nearly all night
//   Singapore  { lat:  1.3521, lon: 103.8198 }  ~20 min golden hour, night year-round
//   Ushuaia    { lat: -54.8019, lon: -68.3030 } southern hemisphere, inverted seasons
var DEBUG_LOCATION = null;

// --- Settings (from Clay) ---
var settings = {
    use24Hour: true,
    // Units for wind and visibility: 0 = metric (m/s, km), 1 = imperial (mph,
    // miles). THE WATCH DOES THE CONVERSION — the phone always sends SI (see
    // buildPacket); the setting is kept here only to travel to the watch
    // alongside HourFormat.
    weatherUnits: 0,
    // Buzz N minutes before the next light window starts (0 = off). THE WATCH
    // works out the moment from the times in the packet; the phone has nothing to
    // do here.
    notifyLeadMin: 0,
    updateIntervalMin: 60, // weather refresh period, minutes (60/120/180)
    astroTimeoutSec: 15,   // auto-return Astro→Clock, seconds (0 = off)
    stopwatchIdleSec: 30,  // idle exit Stopwatch→Clock, seconds (0 = off)
    stopwatchMaxMin: 30,   // safety net for a forgotten run, minutes (0 = no limit)
    showStopwatch: true    // keep the stopwatch screen in the tap cycle
};

function loadSettings() {
    try {
        var stored = localStorage.getItem("settings");
        if (stored) {
            var obj = JSON.parse(stored);
            if (obj && typeof obj === "object") {
                if (typeof obj.use24Hour === "boolean") settings.use24Hour = obj.use24Hour;
                if (typeof obj.weatherUnits === "number") settings.weatherUnits = obj.weatherUnits;
                if (typeof obj.notifyLeadMin === "number") settings.notifyLeadMin = obj.notifyLeadMin;
                if (typeof obj.updateIntervalMin === "number") settings.updateIntervalMin = obj.updateIntervalMin;
                if (typeof obj.astroTimeoutSec === "number") settings.astroTimeoutSec = obj.astroTimeoutSec;
                if (typeof obj.stopwatchIdleSec === "number") settings.stopwatchIdleSec = obj.stopwatchIdleSec;
                if (typeof obj.stopwatchMaxMin === "number") settings.stopwatchMaxMin = obj.stopwatchMaxMin;
                if (typeof obj.showStopwatch === "boolean") settings.showStopwatch = obj.showStopwatch;
            }
        }
    } catch (e) {
        console.log("settings parse error: " + e);
    }
}

function saveSettings() {
    try {
        localStorage.setItem("settings", JSON.stringify(settings));
    } catch (e) {
        console.log("settings save error: " + e);
    }
}

// --- Utilities ---

// Date | undefined → uint32 Unix seconds (0 = no data / polar day or night).
function toTs(date) {
    if (date instanceof Date && !isNaN(date.getTime())) {
        return Math.floor(date.getTime() / 1000);
    }
    return 0;
}

// Moon phase 0..1 (SunCalc) → 0..7.
function moonPhaseIndex(phase) {
    // 0=new, 2=first quarter, 4=full, 6=last quarter
    return Math.round(phase * 8) % 8;
}

// --- Weather (Open-Meteo, no API key) with throttling and a cache ---

// The key name carries the cache format VERSION, like the persist keys on the
// watch. The first version stored cloud cover alone and counted as fresh by age,
// so after an upgrade wind and visibility would not appear until UpdateInterval
// expired (up to 3 hours): a fresh cache cancels the network round trip.
var WEATHER_CACHE_KEY = "weatherCacheV2";

try {
    localStorage.removeItem("weatherCache"); // the v1 cache is no longer read
} catch (e) {
    // localStorage unavailable — not a reason to fail at startup
}

// A weather snapshot: three figures, each independently nullable ("no data").
// A missing value must reach the watch as -1 rather than 0 — zero is a valid
// "clear sky / no wind / zero visibility".
function emptyWeather() {
    return { cloud: null, wind: null, visibility: null };
}

// A number from JSON, or null. It is a function of its own because there are two
// sources — the network and the cache — and an older cache (from before wind and
// visibility) has no such fields at all: they arrive undefined and must become
// null rather than NaN.
function numOrNull(v) {
    return (typeof v === "number" && isFinite(v)) ? v : null;
}

function loadWeatherCache() {
    try {
        var raw = localStorage.getItem(WEATHER_CACHE_KEY);
        if (raw) {
            var c = JSON.parse(raw);
            if (c && typeof c === "object") {
                return {
                    cloud: numOrNull(c.cloud),
                    wind: numOrNull(c.wind),
                    visibility: numOrNull(c.visibility),
                    ts: numOrNull(c.ts) || 0
                };
            }
        }
    } catch (e) {
        console.log("weather cache parse error: " + e);
    }
    return null;
}

function saveWeatherCache(wx) {
    try {
        localStorage.setItem(WEATHER_CACHE_KEY, JSON.stringify({
            cloud: wx.cloud,
            wind: wx.wind,
            visibility: wx.visibility,
            ts: Date.now()
        }));
    } catch (e) {
        console.log("weather cache save error: " + e);
    }
}

// Return cached weather if it is younger than updateInterval, otherwise null.
function freshCachedWeather() {
    // The cache is not tied to coordinates: when DEBUG_LOCATION moves you
    // elsewhere, fresh weather for the previous point would cancel the network
    // request and the new place would show the old clouds. In debug the cache is
    // therefore always treated as stale.
    if (DEBUG_LOCATION) return null;
    var cache = loadWeatherCache();
    if (!cache) return null;
    var ageMin = (Date.now() - cache.ts) / 60000;
    if (ageMin < settings.updateIntervalMin) {
        return cache;
    }
    return null;
}

// XMLHttpRequest helper (PebbleKit JS has no fetch).
function xhrGetJson(url, onOk, onErr) {
    var xhr = new XMLHttpRequest();
    xhr.onload = function () {
        try {
            onOk(JSON.parse(this.responseText));
        } catch (e) {
            console.log("weather json parse error: " + e);
            onErr(e);
        }
    };
    xhr.onerror = function (e) { onErr(e); };
    xhr.ontimeout = function (e) { onErr(e); };
    xhr.timeout = 15000;
    xhr.open("GET", url);
    xhr.send();
}

// VISIBILITY COMES FROM A SPECIFIC MODEL rather than from best_match like the
// rest of the weather. The reason is not accuracy but spread: from best_match
// this variable is unusable.
//
// Measured over a full day of hourly values at one location:
//   best_match (knmi there):    200 … 53 760 m
//   gfs_seamless:            23 840 … 24 140 m
// The nearby airport's METAR reported `9999` — "10 km or more", i.e. clear — the
// whole time. The 200 m of night "fog" is the model's invention: it diagnoses
// visibility from humidity, and a surface inversion yields near-zero visibility
// under a genuinely clear sky. At the 20 km threshold that flipped the row from
// green to red every night.
//
// Same moment, same place, different models — a 57-fold spread: best_match 420 m,
// gfs_seamless 24 140 m, ukmo_seamless 23 580 m, while ecmwf/icon/meteofrance/
// jma/gem/kma do not carry the variable at all (visibility: null). GFS is chosen
// as a global model (verified as far as Antarctica) with no nightly dropouts. Its
// own ceiling is 24 140 m = exactly 15 miles; above the display threshold the
// watch prints "20+ km" anyway.
var WX_VIS_MODEL = "gfs_seamless";

// Fetch a fresh weather snapshot. cb({cloud,wind,visibility}|null).
//
// There are TWO requests because `models=` in Open-Meteo applies to the whole
// request: cloud cover from best_match and visibility from GFS cannot be had in
// one. Multi-model suffixes (`visibility_gfs_seamless`) exist only in hourly — in
// current the API silently takes the FIRST model of the list. Pulling 24 hours of
// hourly data and locating the current hour costs more than a second request:
// the network is touched once per UpdateInterval (30…180 min) and the cache
// answers in between.
//
// wind_speed_unit=ms: the API defaults to km/h, while m/s is what photographers
// think in (it tells whether foliage moves and whether a tripod holds).
function fetchWeatherNetwork(lat, lon, cb) {
    var base = "https://api.open-meteo.com/v1/forecast?latitude=" + lat +
        "&longitude=" + lon;
    var wx = emptyWeather();
    var pending = 2;

    function finish() {
        pending -= 1;
        if (pending > 0) return;
        // A completely empty snapshot (both requests failed) is not cached: it
        // would evict the last valid values and the watch would show "--" until
        // the next cycle. A partial one IS cached — partial data beats none, and
        // the missing field stays null and travels to the watch as -1.
        if (wx.cloud === null && wx.wind === null && wx.visibility === null) {
            cb(null);
            return;
        }
        saveWeatherCache(wx);
        cb(wx);
    }

    var skyUrl = base + "&current=cloud_cover,wind_speed_10m&wind_speed_unit=ms";
    console.log("weather: fetching " + skyUrl);
    xhrGetJson(skyUrl, function (data) {
        var cur = (data && data.current) || null;
        if (cur) {
            if (numOrNull(cur.cloud_cover) !== null) {
                wx.cloud = Math.round(cur.cloud_cover);
            }
            if (numOrNull(cur.wind_speed_10m) !== null) {
                // Clipped to the int8_t range used on the watch: a hurricane is
                // ~50 m/s, but a corrupted response must not overflow the field.
                wx.wind = Math.min(120, Math.max(0, Math.round(cur.wind_speed_10m)));
            }
        }
        finish();
    }, finish);

    var visUrl = base + "&current=visibility&models=" + WX_VIS_MODEL;
    console.log("weather: fetching " + visUrl);
    xhrGetJson(visUrl, function (data) {
        var cur = (data && data.current) || null;
        if (cur && numOrNull(cur.visibility) !== null) {
            wx.visibility = Math.max(0, Math.round(cur.visibility)); // metres
        }
        finish();
    }, finish);
}

// --- Building and sending the packet ---
// The astronomy (SunCalc) is computed offline and instantly, so a packet with it
// goes out IMMEDIATELY, carrying whatever weather the cache holds. Fresh weather
// is fetched asynchronously and, if it arrives, the packet is sent again.

// SunCalc ANCHORS ITS DAY TO UTC. Given the current moment, after local midnight
// in eastern time zones it returns YESTERDAY's times: 03:03 at UTC+5 is 22:03 of
// the previous day in UTC, and getTimes(now) then reports the previous day's
// sunrise. getMoonTimes behaves the same way. The fix is to pass LOCAL NOON of
// the day in question: it always falls inside that day in any time zone.
function sunTimesForLocalDay(base, dayOffset, lat, lon) {
    var noon = new Date(base.getFullYear(), base.getMonth(),
                        base.getDate() + dayOffset, 12, 0, 0, 0);
    return SunCalc.getTimes(noon, lat, lon);
}

// Moon events are scanned rather than taken from SunCalc.getMoonTimes: sampling
// altitude every two hours, that function drops events, sometimes returns one
// belonging to the next day, and raises alwaysUp on days when the moon does both
// rise and set. A missing event is legitimate (near the poles, and once a month
// anywhere), so none of it can be caught downstream.
var MOON_HC_DEG = 0.133;                   // horizon in DEGREES: SunCalc 2.x
                                           // reports altitude in degrees
var MOON_SCAN_STEP_MS = 10 * 60 * 1000;
var MOON_REFINE_STEPS = 8;                 // bisection: 10 min → ~2 s

function moonAltitudeAt(ts, lat, lon) {
    return SunCalc.getMoonPosition(new Date(ts), lat, lon).altitude - MOON_HC_DEG;
}

// Bisection between two samples that straddle the horizon.
function refineCrossing(t0, a0, t1, lat, lon) {
    for (var i = 0; i < MOON_REFINE_STEPS; i++) {
        var mid = Math.round((t0 + t1) / 2);
        var am = moonAltitudeAt(mid, lat, lon);
        if ((a0 < 0) === (am < 0)) {
            t0 = mid;
            a0 = am;
        } else {
            t1 = mid;
        }
    }
    return Math.round((t0 + t1) / 2);
}

// First rise and first set within the local day; null when it truly does not happen.
function moonTimesForLocalDay(base, dayOffset, lat, lon) {
    var dayStart = new Date(base.getFullYear(), base.getMonth(),
                            base.getDate() + dayOffset, 0, 0, 0, 0).getTime();
    var end = dayStart + 24 * 60 * 60 * 1000;
    var times = { rise: null, set: null };
    var prevT = dayStart;
    var prev = moonAltitudeAt(dayStart, lat, lon);

    for (var t = dayStart + MOON_SCAN_STEP_MS; t <= end; t += MOON_SCAN_STEP_MS) {
        var cur = moonAltitudeAt(t, lat, lon);
        if (prev < 0 && cur >= 0 && !times.rise) {
            times.rise = new Date(refineCrossing(prevT, prev, t, lat, lon));
        } else if (prev >= 0 && cur < 0 && !times.set) {
            times.set = new Date(refineCrossing(prevT, prev, t, lat, lon));
        }
        prevT = t;
        prev = cur;
    }
    return times;
}

// The two light windows of a day. In the morning the order is blue → golden
// (dawn → sunrise), in the evening the reverse — the watch derives its
// morning/evening note from the same cue.
function lightWindowsOf(times) {
    return [
        {
            goldenStart: times.sunrise, goldenEnd: times.goldenHourEnd,
            blueStart: times.dawn, blueEnd: times.sunrise,
            endsAt: times.goldenHourEnd
        },
        {
            goldenStart: times.goldenHour, goldenEnd: times.sunsetStart,
            blueStart: times.sunsetStart, blueEnd: times.dusk,
            endsAt: times.dusk
        }
    ];
}

// The nearest UPCOMING window, not "morning before noon, evening after". The old
// rule showed light that had already passed: at night yesterday's evening, and
// late in the evening today's, when the next light is tomorrow morning. What a
// photographer needs is when the light WILL be, so today and tomorrow are scanned
// and the first window that has not ended yet is taken.
function pickLightWindow(lat, lon, now) {
    for (var off = 0; off <= 1; off++) {
        var wins = lightWindowsOf(sunTimesForLocalDay(now, off, lat, lon));
        for (var i = 0; i < wins.length; i++) {
            var end = wins[i].endsAt;
            if (end instanceof Date && !isNaN(end.getTime()) && end > now) {
                return wins[i];
            }
        }
    }
    // Polar day or night: there are no times at all (NaN). Return the morning
    // set and let toTs() turn it into zeros, i.e. "no data".
    return lightWindowsOf(sunTimesForLocalDay(now, 0, lat, lon))[0];
}

// sun/moonTimes hold TODAY's local times (sunrise and sunset read as a summary
// of the current day), while win is the nearest upcoming light window, which may
// already belong to tomorrow.
function buildPacket(sun, moonTimes, moonIllum, win, wx) {
    if (!wx) wx = emptyWeather();
    return {
        SunRise: toTs(sun.sunrise),
        SunSet: toTs(sun.sunset),
        SunGoldenStart: toTs(win.goldenStart),
        SunGoldenEnd: toTs(win.goldenEnd),
        SunBlueStart: toTs(win.blueStart),
        SunBlueEnd: toTs(win.blueEnd),
        // TODAY's boundaries, for the ring. Separate from the window above: the
        // window is the nearest upcoming one and may be tomorrow's, whereas the
        // ring always draws the current day in full.
        SunAstroDusk: toTs(sun.night),
        SunAstroDawn: toTs(sun.nightEnd),
        SunDawn: toTs(sun.dawn),
        SunGoldenAmEnd: toTs(sun.goldenHourEnd),
        SunGoldenPmStart: toTs(sun.goldenHour),
        SunDusk: toTs(sun.dusk),
        MoonRise: toTs(moonTimes.rise),
        MoonSet: toTs(moonTimes.set),
        MoonPhase: moonPhaseIndex(moonIllum.phase),
        MoonIllum: Math.round(moonIllum.fraction * 100),
        // -1 = "no data" (the watch draws "--"). Sending 0 is not an option: on
        // screen it reads as clear sky, no wind, zero visibility — meaningful and
        // false at the same time.
        WxCloud: (wx.cloud === null ? -1 : wx.cloud),            // %
        // ALWAYS SI, whatever units are selected: converting to mph and miles is
        // display formatting and the watch does it (as with 12/24-hour time).
        // Otherwise changing units would require a new packet from the phone, and
        // the weather cache would hold values in whichever units were current at
        // fetch time.
        WxWind: (wx.wind === null ? -1 : wx.wind),               // m/s
        WxVisibility: (wx.visibility === null ? -1 : wx.visibility), // metres
        DataTs: Math.floor(Date.now() / 1000),
        // Settings relayed to the watch.
        HourFormat: settings.use24Hour ? 1 : 0,
        WeatherUnits: settings.weatherUnits,
        NotifyLeadTime: settings.notifyLeadMin,
        AstroTimeout: settings.astroTimeoutSec,
        StopwatchIdleTimeout: settings.stopwatchIdleSec,
        StopwatchMaxDuration: settings.stopwatchMaxMin,
        ShowStopwatch: settings.showStopwatch ? 1 : 0
    };
}

function sendPacket(packet, tag) {
    console.log("sending packet (" + tag + "): cloud=" + packet.WxCloud +
        " wind=" + packet.WxWind + " vis=" + packet.WxVisibility +
        " sunrise=" + packet.SunRise + " sunset=" + packet.SunSet);
    Pebble.sendAppMessage(packet,
        function () { console.log("packet sent ok (" + tag + ")"); },
        function (e) { console.log("packet send failed: " + JSON.stringify(e)); }
    );
}

function buildAndSend(lat, lon) {
    var now = new Date();

    // Sunrise, sunset and astronomical twilight are for TODAY's local day; the
    // light window is picked separately and may turn out to be tomorrow's (see
    // pickLightWindow).
    var sun = sunTimesForLocalDay(now, 0, lat, lon);
    var moonTimes = moonTimesForLocalDay(now, 0, lat, lon);
    var moonIllum = SunCalc.getMoonIllumination(now);
    var win = pickLightWindow(lat, lon, now);

    // 1) Immediately: astronomy plus whatever the cache holds (may be null).
    var cached = loadWeatherCache();
    sendPacket(buildPacket(sun, moonTimes, moonIllum, win, cached), "astro");

    // 2) If the weather cache is stale, fetch fresh data and send again.
    if (freshCachedWeather() === null) {
        fetchWeatherNetwork(lat, lon, function (wx) {
            if (wx === null) return;
            sendPacket(buildPacket(sun, moonTimes, moonIllum, win, wx), "weather");
        });
    } else {
        console.log("weather: cache fresh (cloud=" + cached.cloud +
            "% wind=" + cached.wind + " vis=" + cached.visibility + "), no fetch");
    }
}

// --- Location and updates ---

// Periodic weather polling. The timer id is kept so it can be re-armed when
// UpdateInterval changes; otherwise a new period would only take effect after
// pkjs restarts. The cost is one variable on the phone and nothing on the watch.
var updateTimer = null;
function rearmUpdateTimer() {
    if (updateTimer) clearInterval(updateTimer);
    updateTimer = setInterval(updateAll, settings.updateIntervalMin * 60 * 1000);
}

function updateAll() {
    if (DEBUG_LOCATION) {
        console.log("*** DEBUG_LOCATION: " + DEBUG_LOCATION.lat + ", " +
            DEBUG_LOCATION.lon + " — real geolocation is NOT queried. " +
            "Set DEBUG_LOCATION = null before a release (src/pkjs/index.js)");
        buildAndSend(DEBUG_LOCATION.lat, DEBUG_LOCATION.lon);
        return;
    }
    navigator.geolocation.getCurrentPosition(
        function (pos) {
            var lat = pos.coords.latitude;
            var lon = pos.coords.longitude;
            console.log("location: " + lat + ", " + lon);
            buildAndSend(lat, lon);
        },
        function (err) {
            console.log("geolocation error: " + err.message);
        },
        { timeout: 15000, maximumAge: 60000 }
    );
}

// --- Lifecycle ---

Pebble.addEventListener("ready", function () {
    loadSettings();
    console.log("pkjs ready");
    updateAll();
    rearmUpdateTimer();
});

Pebble.addEventListener("appmessage", function (e) {
    // A message from the watch. The Refresh key means a forced update: the watch
    // asks for a fresh snapshot, so recompute and send the packet again.
    if (e && e.payload && e.payload.Refresh !== undefined) {
        console.log("refresh requested by watch (tick " + e.payload.Refresh + ")");
        updateAll();
    }
});

// showConfiguration is NOT registered by hand: with autoHandleEvents (the
// default) Clay itself serves the settings page.

// Settings from Clay.
//
// IMPORTANT — this was the root cause of "the timeout is never applied":
// clay.getSettings(e.response) returns a dict keyed by NUMERIC messageKey IDs
// (10015/10016/…), not by names. dict.HourFormat and dict.AstroTimeout were
// therefore always undefined, every branch was skipped, the settings stayed at
// their defaults and the watch kept receiving them.
//
// The raw e.response, by contrast, is an object WITH NAMES and a .value field:
//   {"HourFormat":{"value":true},"AstroTimeout":{"value":"60"}, ...}
// That is what is parsed here, by key name — reliable and independent of Clay's
// internal numbering.
function pickValue(node) {
    // node may be {value: X} (Clay's shape) or X itself — take X.
    return (node && typeof node === "object" && "value" in node) ? node.value : node;
}

Pebble.addEventListener("webviewclosed", function (e) {
    // An empty response means the page was closed without saving — change nothing.
    if (!e || !e.response) return;

    var dict;
    try {
        dict = JSON.parse(e.response);
    } catch (err) {
        console.log("webviewclosed: bad response JSON: " + err);
        return;
    }

    if (dict.HourFormat !== undefined) {
        settings.use24Hour = !!pickValue(dict.HourFormat);
    }
    if (dict.WeatherUnits !== undefined) {
        var units = parseInt(pickValue(dict.WeatherUnits), 10);
        if (!isNaN(units)) settings.weatherUnits = units;
    }
    if (dict.NotifyLeadTime !== undefined) {
        var lead = parseInt(pickValue(dict.NotifyLeadTime), 10);
        if (!isNaN(lead)) settings.notifyLeadMin = lead;
    }
    if (dict.UpdateInterval !== undefined) {
        var mins = parseInt(pickValue(dict.UpdateInterval), 10);
        if (!isNaN(mins)) settings.updateIntervalMin = mins;
    }
    if (dict.AstroTimeout !== undefined) {
        var secs = parseInt(pickValue(dict.AstroTimeout), 10);
        if (!isNaN(secs)) settings.astroTimeoutSec = secs;
    }
    if (dict.StopwatchIdleTimeout !== undefined) {
        var ssecs = parseInt(pickValue(dict.StopwatchIdleTimeout), 10);
        if (!isNaN(ssecs)) settings.stopwatchIdleSec = ssecs;
    }
    if (dict.StopwatchMaxDuration !== undefined) {
        var smax = parseInt(pickValue(dict.StopwatchMaxDuration), 10);
        if (!isNaN(smax)) settings.stopwatchMaxMin = smax;
    }
    if (dict.ShowStopwatch !== undefined) {
        settings.showStopwatch = !!pickValue(dict.ShowStopwatch);
    }
    saveSettings();
    console.log("settings updated: use24Hour=" + settings.use24Hour +
        " units=" + settings.weatherUnits +
        " notifyLead=" + settings.notifyLeadMin +
        " interval=" + settings.updateIntervalMin +
        " astroTimeout=" + settings.astroTimeoutSec +
        " swIdle=" + settings.stopwatchIdleSec +
        " swMax=" + settings.stopwatchMaxMin +
        " showStopwatch=" + settings.showStopwatch);

    updateAll();
    // Apply the new polling period at once, without waiting for a pkjs restart.
    rearmUpdateTimer();
});

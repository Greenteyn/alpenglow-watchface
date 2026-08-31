// config.js — the Clay settings page shown on the phone. Values arrive in pkjs
// (index.js); some of them are relayed to the watch.
//
// NOTE: select fields carry STRING values ("60"/"15"/"0"). pkjs converts them
// with parseInt before sending, and settings.c on the watch is tolerant of
// strings as well (tuple_to_int).
//
// Descriptions here stay short on purpose: custom-clay.js fades an item out
// when the switch above it makes it inert, and faded text is barely readable.
module.exports = [
    {
        type: "heading",
        defaultValue: "Alpenglow",
    },
    {
        type: "text",
        defaultValue: "A watchface for landscape and night photography.",
    },
    {
        type: "section",
        items: [
            {
                type: "heading",
                defaultValue: "Clock",
            },
            {
                type: "toggle",
                messageKey: "HourFormat",
                label: "24-hour format",
                defaultValue: true,
            },
            {
                type: "select",
                messageKey: "RingOrientation",
                label: "Ring orientation",
                description:
                    "Which way round the day sits on the ring of the Clock " +
                    "screen. With midnight at the bottom daylight fills the " +
                    "upper half and the sun tracks the way it does in the " +
                    "sky: up the left side, over the top at noon, down the " +
                    "right.",
                defaultValue: "0",
                options: [
                    { label: "Midnight at the top", value: "0" },
                    { label: "Midnight at the bottom", value: "1" },
                ],
            },
        ],
    },
    {
        type: "section",
        items: [
            {
                type: "heading",
                defaultValue: "Data",
            },
            {
                type: "select",
                messageKey: "WeatherUnits",
                label: "Units",
                description:
                    "Wind and visibility. The packet always travels in SI and the " +
                    "watch does the conversion, so a change applies immediately " +
                    "instead of waiting for the next weather update.",
                defaultValue: "0",
                options: [
                    { label: "Metric (m/s, km)", value: "0" },
                    { label: "Imperial (mph, miles)", value: "1" },
                ],
            },
            {
                type: "select",
                messageKey: "UpdateInterval",
                label: "Weather refresh period",
                defaultValue: "60",
                options: [
                    { label: "1 hour", value: "60" },
                    { label: "2 hours", value: "120" },
                    { label: "3 hours", value: "180" },
                ],
            },
        ],
    },
    {
        type: "section",
        items: [
            {
                type: "heading",
                defaultValue: "Notifications",
            },
            {
                type: "select",
                messageKey: "NotifyLeadTime",
                label: "Light window alert",
                description:
                    "Vibration before the next light window starts: the blue hour " +
                    "in the morning, the golden hour in the evening. The lead time " +
                    "is there to let you reach the spot. Each window is announced " +
                    "once, and the alert stays silent during Do Not Disturb.",
                defaultValue: "0",
                options: [
                    { label: "Off", value: "0" },
                    { label: "10 minutes before", value: "10" },
                    { label: "15 minutes before", value: "15" },
                    { label: "30 minutes before", value: "30" },
                    { label: "60 minutes before", value: "60" },
                ],
            },
        ],
    },
    {
        type: "section",
        items: [
            {
                type: "heading",
                defaultValue: "Navigation",
            },
            {
                type: "toggle",
                messageKey: "TapControl",
                label: "Tap to switch screens",
                description:
                    "A tap on the case cycles Clock → Astro → Stopwatch and " +
                    "runs the stopwatch. Listening for it keeps the " +
                    "accelerometer on: switching taps off shuts the sensor " +
                    "down, can stretch a charge up to about twice as far, and " +
                    "leaves the Clock as the only screen. The light window " +
                    "alert keeps working either way.",
                defaultValue: true,
            },
            {
                type: "select",
                messageKey: "AstroTimeout",
                label: "Return from the Astro screen",
                defaultValue: "15",
                options: [
                    { label: "Off", value: "0" },
                    { label: "After 10 seconds", value: "10" },
                    { label: "After 15 seconds", value: "15" },
                    { label: "After 30 seconds", value: "30" },
                    { label: "After 60 seconds", value: "60" },
                ],
            },
            {
                type: "toggle",
                messageKey: "ShowStopwatch",
                label: "Stopwatch screen",
                description:
                    "Off, the screen drops out of the cycle: a tap on Astro " +
                    "returns to the Clock, and a running measurement is " +
                    "stopped and reset.",
                defaultValue: true,
            },
            {
                type: "select",
                messageKey: "StopwatchIdleTimeout",
                label: "Leave the Stopwatch when not started",
                defaultValue: "30",
                options: [
                    { label: "Off", value: "0" },
                    { label: "After 15 seconds", value: "15" },
                    { label: "After 30 seconds", value: "30" },
                    { label: "After 60 seconds", value: "60" },
                ],
            },
            {
                type: "select",
                messageKey: "StopwatchMaxDuration",
                label: "Maximum measurement length",
                description:
                    "A forgotten measurement keeps ticking and drains the " +
                    "battery, so at the limit it stops by itself, with a buzz.",
                defaultValue: "30",
                options: [
                    { label: "No limit", value: "0" },
                    { label: "5 minutes", value: "5" },
                    { label: "10 minutes", value: "10" },
                    { label: "15 minutes", value: "15" },
                    { label: "30 minutes", value: "30" },
                    { label: "60 minutes", value: "60" },
                ],
            },
        ],
    },
    {
        type: "submit",
        defaultValue: "Save",
    },
];

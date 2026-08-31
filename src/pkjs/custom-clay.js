// custom-clay.js — runs on the settings page itself, not in pkjs. Clay injects
// it by calling toString() on the function, so nothing here may use require()
// or close over anything outside the function body.
//
// It fades the Navigation settings that a switch above them has made inert.
// Clay offers no declarative way to express that dependency and cannot disable
// a whole section, so every dependent item is toggled by hand. Disabled items
// are still serialized and still travel to the watch — this is presentation
// only, the packet does not change.
module.exports = function () {
    var clayConfig = this;

    function setEnabled(messageKeys, enabled) {
        messageKeys.forEach(function (key) {
            var item = clayConfig.getItemByMessageKey(key);
            if (enabled) {
                item.enable();
            } else {
                item.disable();
            }
        });
    }

    // No taps means the Clock is the only screen, so nothing else in the
    // section applies. No Stopwatch screen means only its own two timers go.
    function refresh() {
        var taps = clayConfig.getItemByMessageKey("TapControl").get();
        var stopwatch =
            taps && clayConfig.getItemByMessageKey("ShowStopwatch").get();

        setEnabled(["AstroTimeout", "ShowStopwatch"], taps);
        setEnabled(["StopwatchIdleTimeout", "StopwatchMaxDuration"], stopwatch);
    }

    clayConfig.on(clayConfig.EVENTS.AFTER_BUILD, function () {
        refresh();
        clayConfig.getItemByMessageKey("TapControl").on("change", refresh);
        clayConfig.getItemByMessageKey("ShowStopwatch").on("change", refresh);
    });
};

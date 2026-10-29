// Permanent behavior test for CTrailTracker: identical physical motion must produce
// identical displacement regardless of the pointer report rate, plus lifecycle
// guarantees (first movement, idle expiry, reset, bounded ring).
// Plain g++ binary, no test infra: `make test`
#include <cassert>
#include <chrono>
#include <cstdio>

#include "../src/other/Trail.hpp"

using Clock = std::chrono::steady_clock;
using namespace Hyprutils::Math;

static Clock::time_point at(int ms) {
    return Clock::time_point{std::chrono::milliseconds{ms}};
}

static bool eq(const Vector2D& a, const Vector2D& b, float eps = 0.001f) {
    return (a - b).size() < eps;
}

/*
Feeds the same physical motion (a 660px movement over 80ms) into two trackers,
one reported at ~125Hz (8ms), one at 1000Hz (1ms). Both must yield the same
displacement for the same query window.
*/
static void sameMotionDifferentRates() {
    CTrailTracker lowRate, highRate;

    for (int i = 0; i <= 80; i += 8) // 125Hz
        lowRate.record(Vector2D{8.25 * i, 0.0}, at(i));
    for (int i = 0; i <= 80; i += 1) // 1000Hz
        highRate.record(Vector2D{8.25 * i, 0.0}, at(i));

    const auto LOW  = lowRate.displacement(at(80), std::chrono::milliseconds(40));
    const auto HIGH = highRate.displacement(at(80), std::chrono::milliseconds(40));

    assert(LOW.has_value());
    assert(HIGH.has_value());
    // window of 40ms -> 330px of travel in both cases
    assert(eq(*LOW, Vector2D{330, 0}, 2.f));
    assert(eq(*HIGH, Vector2D{330, 0}, 2.f));
}

/*
A single first movement event must immediately produce displacement from the idle
position the tick history retains; no velocity interval before it is faked. The
tracker alone (one sample, no history) reports no displacement.
*/
static void firstMovementHasVelocity() {
    CTrailTracker t;

    // idle history, as the plugin's ticks keep it
    for (int i = 0; i <= 96; i += 16)
        t.record(Vector2D{0, 0}, at(i));

    // a single first movement event
    t.record(Vector2D{60, 30}, at(105));
    const auto D = t.displacement(at(105), std::chrono::milliseconds(33));
    assert(D.has_value());
    assert(eq(*D, Vector2D{60, 30}));

    // without retained history a lone event carries no direction: no fake velocity
    CTrailTracker lone;
    lone.record(Vector2D{100, 50}, at(10));
    const auto LONE = lone.displacement(at(10), std::chrono::milliseconds(33));
    assert(LONE.has_value());
    assert(eq(*LONE, Vector2D{0, 0}));
    // stationary reports only collapse the displacement once a full window
    // (33ms) has passed without movement
    for (int i = 1; i <= 40; ++i)
        t.record(Vector2D{60, 30}, at(105 + i));
    const auto D2 = t.displacement(at(146), std::chrono::milliseconds(33));
    assert(D2.has_value());
    assert(eq(*D2, Vector2D{0, 0}));
}

static void idleExpiryAndReset() {
    CTrailTracker t;
    t.record(Vector2D{0, 0}, at(0));
    t.record(Vector2D{300, 0}, at(30));

    assert(t.displacement(at(30), std::chrono::milliseconds(30)).has_value());
    // no report within the window: expired, sharp cursor
    assert(!t.displacement(at(100), std::chrono::milliseconds(33)));

    // reset (warp / shape change / disable) clears everything
    t.record(Vector2D{600, 0}, at(110));
    t.reset();
    assert(!t.displacement(at(111), std::chrono::milliseconds(33)));
}

/*
The ring is bounded: reporting far more events than MAX slots at a high rate must
not grow or misreport; the window can only reach back as far as history is retained.
*/
static void boundedRing() {
    CTrailTracker t;
    for (int i = 0; i < 10000; ++i) // 10s at 1kHz
        t.record(Vector2D{i * 1.0, 0.0}, at(i));

    const auto D = t.displacement(at(9999), std::chrono::milliseconds(33));
    assert(D.has_value());
    assert(eq(*D, Vector2D{33, 0}, 5.f)); // 64 slots at >=4ms cover >=256ms, plenty for a 33ms window

    // a window longer than the retained history clamps to the oldest retained sample
    const auto LONG = t.displacement(at(9999), std::chrono::milliseconds(250));
    assert(LONG.has_value());
    assert(LONG->x <= 256.f);
}

int main() {
    sameMotionDifferentRates();
    firstMovementHasVelocity();
    idleExpiryAndReset();
    boundedRing();
    printf("trail tracker: all rate-independence and lifecycle checks passed\n");
    return 0;
}

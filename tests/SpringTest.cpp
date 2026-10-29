// Permanent behavior test for CSpring: the tilt inertia spring must overshoot a
// step like a real damped oscillator (ζ ≈ 0.65), settle sharply onto the target,
// respond √m slower for a heavier mass, be invariant under the partitioning of
// time into steps, reset to rest, and survive suspend-sized gaps without
// exploding. Plain g++ binary, no test infra: `make test`
#include <algorithm>
#include <cassert>
#include <cmath>

#include "../src/other/Spring.hpp"

static constexpr double STIFFNESS = 300.0;
static constexpr double RATIO     = 0.65;

static bool near(double value, double target, double eps) {
    return std::abs(value - target) < eps;
}

/*
A unit step must produce a genuine overshoot (~7% for ζ = 0.65) and then settle
onto the target, staying finite at all times.
*/
static void overshootsAndSettles() {
    CSpring spring(STIFFNESS, RATIO);
    spring.setMass(1.0);

    double peak = 0;
    for (int i = 0; i < 240; ++i) { // 1s at 240Hz
        const double X = spring.update(1.0, 1.0 / 240);
        assert(std::isfinite(X));
        peak = std::max(peak, X);
    }

    assert(peak > 1.03 && peak < 1.15);
    assert(near(spring.position(), 1.0, 1e-3));
    assert(std::abs(spring.velocity()) < 1e-3);
}

/*
A mass of 2 must respond √2 slower than a mass of 1: a step response peaks at
half the damped period, so the heavier spring peaks later. Both overshoot by
the same ratio (ζ is mass-independent) and both eventually settle sharply.
*/
static void heavierMassIsSlower() {
    CSpring light(STIFFNESS, RATIO), heavy(STIFFNESS, RATIO);
    light.setMass(1.0);
    heavy.setMass(2.0);

    double lightPeakTime = 0, heavyPeakTime = 0, lightPeak = 0, heavyPeak = 0;
    for (int i = 1; i <= 600; ++i) { // 2.5s at 240Hz
        const double T  = i / 240.0;
        const double XL = light.update(1.0, 1.0 / 240);
        const double XH = heavy.update(1.0, 1.0 / 240);

        if (XL > lightPeak) {
            lightPeak     = XL;
            lightPeakTime = T;
        }
        if (XH > heavyPeak) {
            heavyPeak     = XH;
            heavyPeakTime = T;
        }
    }

    assert(lightPeak > 1.03 && lightPeak < 1.15);
    assert(heavyPeak > 1.03 && heavyPeak < 1.15);

    const double PEAK_RATIO = heavyPeakTime / lightPeakTime;
    assert(PEAK_RATIO > 1.25 && PEAK_RATIO < 1.6); // √2 ≈ 1.41

    assert(light.position() == 1.0); // snapped sharply at rest
    assert(heavy.position() == 1.0);
}

/*
The step is the closed-form solution of the oscillator, so the state after a
fixed duration must not depend on how it was partitioned into steps.
*/
static void partitionInvariance() {
    CSpring coarse(STIFFNESS, RATIO), fine(STIFFNESS, RATIO);
    coarse.setMass(2.0);
    fine.setMass(2.0);

    for (int i = 0; i < 96; ++i) // 0.4s at 240Hz
        coarse.update(1.0, 1.0 / 240);
    for (int i = 0; i < 2400; ++i) // the same 0.4s at 6kHz
        fine.update(1.0, 1.0 / 6000);

    assert(near(coarse.position(), fine.position(), 1e-6));
    assert(near(coarse.velocity(), fine.velocity(), 1e-6));
}

/*
Reset must bring the spring back to rest at zero, clearing position and
velocity, from an arbitrary mid-motion state.
*/
static void resetClearsMotion() {
    CSpring spring(STIFFNESS, RATIO);
    spring.setMass(1.0);

    for (int i = 0; i < 24; ++i) // 0.1s, mid-flight
        spring.update(1.0, 1.0 / 240);
    assert(std::abs(spring.velocity()) > 0.5); // genuinely in motion

    spring.reset();
    assert(spring.position() == 0);
    assert(spring.velocity() == 0);

    for (int i = 0; i < 24; ++i) // driving toward rest must not re-excite it
        spring.update(0.0, 1.0 / 240);
    assert(spring.position() == 0);
    assert(spring.velocity() == 0);
}

/*
A suspend-sized gap arriving as a single step must settle the spring onto the
target instead of exploding it, and finish snapped sharply at rest.
*/
static void longGapSettles() {
    CSpring spring(STIFFNESS, RATIO);
    spring.setMass(2.0);

    for (int i = 0; i < 24; ++i) // 0.1s, mid-flight
        spring.update(1.0, 1.0 / 240);

    spring.update(1.0, 5000.0); // the gap, in one step
    assert(std::isfinite(spring.position()) && std::isfinite(spring.velocity()));
    assert(near(spring.position(), 1.0, 0.05));

    for (int i = 0; i < 120; ++i) // 0.5s of normal ticks afterwards
        spring.update(1.0, 1.0 / 240);
    assert(spring.position() == 1.0);
    assert(spring.velocity() == 0.0);
}

int main() {
    overshootsAndSettles();
    heavierMassIsSlower();
    partitionInvariance();
    resetClearsMotion();
    longGapSettles();
    return 0;
}

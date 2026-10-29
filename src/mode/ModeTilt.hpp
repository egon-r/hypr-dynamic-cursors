#include "Mode.hpp"
#include "../other/Spring.hpp"

#include <hyprutils/math/Vector2D.hpp>
#include <chrono>
#include <vector>
class CModeTilt : public IMode {
  public:
    virtual EModeUpdate strategy();
    virtual SModeResult update(Vector2D pos);
    virtual void        reset();
    virtual void        warp(Vector2D old, Vector2D pos);

  private:
    // stiffness [1/s²] and damping ratio of the inertia spring: ζ ≈ 0.65 gives a
    // single ~7% overshoot and settles in ~0.35s at reference mass 1 (~0.5s at 2)
    static constexpr double SPRING_STIFFNESS = 300.0;
    static constexpr double SPRING_RATIO     = 0.65;

    // `tilt:inertia` is clamped to this; beyond it the cursor feels unresponsive
    static constexpr double SPRING_MASS_MAX = 10.0;

    // ring buffer of last position samples
    std::vector<Vector2D> samples;
    int                   samples_index = 0;

    // damped spring smoothing the tilt angle while `tilt:inertia` > 0
    CSpring                               spring{SPRING_STIFFNESS, SPRING_RATIO};
    std::chrono::steady_clock::time_point lastTick = std::chrono::steady_clock::now();
};

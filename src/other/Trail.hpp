#include <array>
#include <chrono>
#include <cstddef>
#include <optional>

#include <hyprutils/math/Vector2D.hpp>

/*
A single recorded cursor position.
*/
struct STrailSample {
    std::chrono::steady_clock::time_point at;
    Hyprutils::Math::Vector2D             pos;
};

/*
Bounded, time-stamped history of recent cursor positions.

Records are decimated over time (at most one new sample per DECIMATE), so the
tracker behaves identically no matter how often the pointer fires, and the
derived displacement only depends on the time window, not on the mouse poll
rate. The history is a fixed-size ring buffer: recording and querying
allocate nothing and are O(MAX).
*/
class CTrailTracker {
  public:
    /* record a position at a given time; coalesces into the newest sample if it was recorded recently */
    void record(const Hyprutils::Math::Vector2D& pos, const std::chrono::steady_clock::time_point& now);

    /* forget the entire history */
    void reset();

    /*
    displacement of the cursor over the time window (now - window, now].
    Returns nullopt if there is no usable history within the window,
    i.e. when the cursor has been idle (or was reset) for the whole window.
    */
    std::optional<Hyprutils::Math::Vector2D> displacement(const std::chrono::steady_clock::time_point& now, const std::chrono::milliseconds& window) const;

  private:
    static constexpr size_t                    MAX      = 64;
    static constexpr std::chrono::milliseconds DECIMATE = std::chrono::milliseconds(4);

    std::array<STrailSample, MAX>         m_samples;
    size_t                                m_head  = 0; // one past the newest sample
    size_t                                m_count = 0;
    std::chrono::steady_clock::time_point m_lastPush; // when the newest ring slot was spent
};

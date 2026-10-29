#include <algorithm>

#include "Trail.hpp"

using namespace Hyprutils::Math;

void CTrailTracker::record(const Vector2D& pos, const std::chrono::steady_clock::time_point& now) {
    // decimate: spend a ring slot at most once per DECIMATE, fresher positions coalesce into the newest sample.
    // anchored on the last push, so uniformly spaced events cannot all coalesce into a single sample
    if (m_count > 0 && now - m_lastPush < DECIMATE) {
        auto& newest = m_samples[(m_head + MAX - 1) % MAX];

        newest.pos = pos;
        newest.at  = now;
        return;
    }

    m_samples[m_head] = {now, pos};
    m_head            = (m_head + 1) % MAX;
    if (m_count < MAX)
        m_count++;
    m_lastPush = now;
}

void CTrailTracker::reset() {
    m_head  = 0;
    m_count = 0;
}

std::optional<Vector2D> CTrailTracker::displacement(const std::chrono::steady_clock::time_point& now, const std::chrono::milliseconds& window) const {
    if (m_count == 0 || window <= std::chrono::milliseconds::zero())
        return std::nullopt;

    const auto& NEWEST = m_samples[(m_head + MAX - 1) % MAX];

    // history expired: the cursor has been idle (or reset) for at least a window
    if (now - NEWEST.at > window)
        return std::nullopt;

    const auto CUTOFF = now - window;

    // find the oldest sample within the window and its predecessor
    const STrailSample* next = nullptr;
    const STrailSample* prev = nullptr;
    for (size_t i = 0; i < m_count; ++i) {
        const auto& sample = m_samples[(m_head + MAX - m_count + i) % MAX];

        if (sample.at >= CUTOFF) {
            next = &sample;
            if (i > 0)
                prev = &m_samples[(m_head + MAX - m_count + i - 1) % MAX];
            break;
        }
    }

    if (!next)
        return std::nullopt; // unreachable after the staleness check, kept for safety

    Vector2D startPos = next->pos;
    if (prev && next->at > prev->at) {
        // interpolate the position the cursor had exactly at the start of the window
        const float MS = std::chrono::duration<float, std::milli>(next->at - prev->at).count();
        const float T  = std::clamp(std::chrono::duration<float, std::milli>(CUTOFF - prev->at).count() / MS, 0.F, 1.F);
        startPos       = prev->pos + (next->pos - prev->pos) * T;
    }

    return NEWEST.pos - startPos;
}

#include "Spring.hpp"

#include <algorithm>
#include <cmath>

namespace {
    // cap a single step conservatively; the closed form below is stable for any
    // dt, this only keeps huge gaps (e.g. suspend) well inside float range
    constexpr double MAX_STEP = 1.0;

    // below this error and velocity the spring snaps onto its target
    constexpr double REST_EPSILON = 1e-4;

    // lower bound for the mass, guarding the division below
    constexpr double MIN_MASS = 1e-3;
} // namespace

CSpring::CSpring(double stiffness, double ratio) :
    m_stiffness(stiffness), m_ratio(std::clamp(ratio, 0.0, 0.99)), m_mass(1) {}

void CSpring::setMass(double mass) {
    m_mass = std::max(mass, MIN_MASS);
}

void CSpring::snap(double position) {
    m_position = position;
    m_velocity = 0;
}

void CSpring::reset() {
    snap(0);
}

double CSpring::update(double target, double dt) {
    if (dt <= 0 || !std::isfinite(target))
        return m_position;

    if (!std::isfinite(m_position) || !std::isfinite(m_velocity)) {
        snap(target);
        return m_position;
    }

    dt = std::min(dt, MAX_STEP);

    const double OMEGA   = std::sqrt(m_stiffness / m_mass);
    const double OMEGA_D = OMEGA * std::sqrt(1 - m_ratio * m_ratio);
    const double DECAY   = std::exp(-m_ratio * OMEGA * dt);
    const double COS     = std::cos(OMEGA_D * dt);
    const double SIN     = std::sin(OMEGA_D * dt);

    // closed-form solution of x'' = ω²(target - x) - 2ζω x' advanced over dt
    const double X      = m_position - target;
    const double NEXT_X = DECAY * (X * COS + (m_velocity + m_ratio * OMEGA * X) / OMEGA_D * SIN);
    const double NEXT_V = DECAY * (m_velocity * COS - (m_ratio * OMEGA * m_velocity + OMEGA * OMEGA * X) / OMEGA_D * SIN);

    m_position = target + NEXT_X;
    m_velocity = NEXT_V;

    // near rest, snap sharply onto the target instead of ringing invisibly forever
    if (std::abs(m_position - target) < REST_EPSILON && std::abs(m_velocity) < REST_EPSILON) {
        m_position = target;
        m_velocity = 0;
    }

    return m_position;
}

double CSpring::position() const {
    return m_position;
}

double CSpring::velocity() const {
    return m_velocity;
}

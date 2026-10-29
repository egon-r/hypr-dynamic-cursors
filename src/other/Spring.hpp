#pragma once

/*
A damped harmonic spring driving one scalar position toward a target.

Physically parameterised with stiffness k, mass m and damping ratio ζ, i.e.
x'' = ω²(target - x) - 2ζω·x' with ω = √(k/m). A heavier mass therefore slows
the response down proportionally to √m. Stepping uses the closed-form solution
of the oscillator, so evolution is exact and stable no matter how time is
partitioned into steps. All operations allocate nothing.
*/
class CSpring {
  public:
    /* create an underdamped (ζ < 1) spring at rest at position zero */
    CSpring(double stiffness, double ratio);

    /* set the physical mass; a heavier mass responds proportionally slower (√m) */
    void setMass(double mass);

    /* jump to a position without velocity, introducing no impulse */
    void snap(double position);

    /* return to rest at position zero */
    void reset();

    /*
    advance the spring toward the target over dt seconds, returning the new
    position. dt is conservatively clamped so long gaps (e.g. suspend) settle
    instead of oscillating.
    */
    double update(double target, double dt);

    /* current position of the spring */
    double position() const;

    /* current velocity of the spring */
    double velocity() const;

  private:
    double m_stiffness;
    double m_ratio;
    double m_mass     = 1;
    double m_position = 0;
    double m_velocity = 0;
};

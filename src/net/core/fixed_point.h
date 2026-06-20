#pragma once
// =============================================================================
// Fixed-point math - deterministic cross-platform arithmetic
// -----------------------------------------------------------------------------
// Floating-point is NOT deterministic across CPUs/compilers/optimization levels.
// Different FPUs can produce slightly different results for the same operations,
// causing desync in lockstep multiplayer.
//
// Solution: fixed-point arithmetic (integer math with implicit decimal point).
// Format: 16.16 (16 bits integer, 16 bits fraction)
//   Range: -32768.0 to +32767.9999
//   Precision: 1/65536 ≈ 0.000015
//
// This is what industrial RTS games (Age of Empires, StarCraft) use for
// positions, velocities, and combat calculations.
//
// Usage:
//   Fixed x = Fixed::fromFloat(100.5f);
//   Fixed y = Fixed::fromFloat(200.3f);
//   Fixed dist = (x - y).abs();
//   float render_x = x.toFloat();  // convert back for rendering only
// =============================================================================
#include <cstdint>
#include <cmath>

namespace net {

class Fixed {
public:
    static constexpr int FRAC_BITS = 16;
    static constexpr int32_t ONE = 1 << FRAC_BITS;  // 65536
    static constexpr int32_t HALF = ONE / 2;

    int32_t raw;  // internal representation

    // ---- Construction ------------------------------------------------------
    constexpr Fixed() : raw(0) {}
    constexpr explicit Fixed(int32_t raw_value) : raw(raw_value) {}

    static constexpr Fixed fromInt(int32_t i) { return Fixed(i << FRAC_BITS); }
    static Fixed fromFloat(float f) { return Fixed((int32_t)(f * ONE)); }
    static Fixed fromDouble(double d) { return Fixed((int32_t)(d * ONE)); }
    static constexpr Fixed fromRaw(int32_t r) { return Fixed(r); }

    // ---- Conversion back to float (rendering only) -------------------------
    float toFloat() const { return (float)raw / (float)ONE; }
    double toDouble() const { return (double)raw / (double)ONE; }
    int32_t toInt() const { return raw >> FRAC_BITS; }
    int32_t toRaw() const { return raw; }

    // ---- Arithmetic --------------------------------------------------------
    Fixed operator+(Fixed o) const { return Fixed(raw + o.raw); }
    Fixed operator-(Fixed o) const { return Fixed(raw - o.raw); }
    Fixed operator-() const { return Fixed(-raw); }

    // Multiply: (a * b) / 2^16 to keep result in 16.16 format
    Fixed operator*(Fixed o) const {
        int64_t prod = (int64_t)raw * (int64_t)o.raw;
        return Fixed((int32_t)(prod >> FRAC_BITS));
    }

    // Divide: (a * 2^16) / b
    Fixed operator/(Fixed o) const {
        int64_t dividend = (int64_t)raw << FRAC_BITS;
        return Fixed((int32_t)(dividend / o.raw));
    }

    Fixed& operator+=(Fixed o) { raw += o.raw; return *this; }
    Fixed& operator-=(Fixed o) { raw -= o.raw; return *this; }
    Fixed& operator*=(Fixed o) { *this = *this * o; return *this; }
    Fixed& operator/=(Fixed o) { *this = *this / o; return *this; }

    // ---- Comparison --------------------------------------------------------
    bool operator==(Fixed o) const { return raw == o.raw; }
    bool operator!=(Fixed o) const { return raw != o.raw; }
    bool operator<(Fixed o) const { return raw < o.raw; }
    bool operator>(Fixed o) const { return raw > o.raw; }
    bool operator<=(Fixed o) const { return raw <= o.raw; }
    bool operator>=(Fixed o) const { return raw >= o.raw; }

    // ---- Math functions (deterministic) ------------------------------------
    Fixed abs() const { return Fixed(raw < 0 ? -raw : raw); }
    
    Fixed sqrt() const {
        // Babylonian method (bit-exact across platforms)
        if (raw <= 0) return Fixed(0);
        int32_t x = raw;
        int32_t y = (x + ONE) >> 1;
        while (y < x) {
            x = y;
            y = ((int64_t)x + ((int64_t)raw << FRAC_BITS) / x) >> 1;
        }
        return Fixed(x);
    }

    // Fast approximate sin/cos using lookup table (TODO: implement if needed)
    // For now, use float versions (ONLY for non-critical visuals, NOT gameplay)
    float sin_approx() const { return std::sin(toFloat()); }
    float cos_approx() const { return std::cos(toFloat()); }

    // ---- Constants ---------------------------------------------------------
    static constexpr Fixed ZERO() { return Fixed(0); }
    static constexpr Fixed ONE_F() { return Fixed(ONE); }
    static constexpr Fixed HALF_F() { return Fixed(HALF); }
};

// ---- Fixed-point 2D vector (positions, velocities) -------------------------
struct FixedVec2 {
    Fixed x, y;

    constexpr FixedVec2() : x(), y() {}
    constexpr FixedVec2(Fixed x_, Fixed y_) : x(x_), y(y_) {}

    static FixedVec2 fromFloat(float fx, float fy) {
        return FixedVec2(Fixed::fromFloat(fx), Fixed::fromFloat(fy));
    }

    FixedVec2 operator+(FixedVec2 o) const { return FixedVec2(x + o.x, y + o.y); }
    FixedVec2 operator-(FixedVec2 o) const { return FixedVec2(x - o.x, y - o.y); }
    FixedVec2 operator*(Fixed s) const { return FixedVec2(x * s, y * s); }
    FixedVec2 operator/(Fixed s) const { return FixedVec2(x / s, y / s); }

    Fixed dot(FixedVec2 o) const { return x * o.x + y * o.y; }
    Fixed lengthSquared() const { return dot(*this); }
    Fixed length() const { return lengthSquared().sqrt(); }

    FixedVec2 normalized() const {
        Fixed len = length();
        if (len.raw == 0) return FixedVec2(Fixed::ONE_F(), Fixed::ZERO());
        return *this / len;
    }
};

// ---- Fixed-point 3D vector (positions in 3D world) -------------------------
struct FixedVec3 {
    Fixed x, y, z;

    constexpr FixedVec3() : x(), y(), z() {}
    constexpr FixedVec3(Fixed x_, Fixed y_, Fixed z_) : x(x_), y(y_), z(z_) {}

    static FixedVec3 fromFloat(float fx, float fy, float fz) {
        return FixedVec3(Fixed::fromFloat(fx), Fixed::fromFloat(fy), Fixed::fromFloat(fz));
    }

    FixedVec3 operator+(FixedVec3 o) const { return FixedVec3(x + o.x, y + o.y, z + o.z); }
    FixedVec3 operator-(FixedVec3 o) const { return FixedVec3(x - o.x, y - o.y, z - o.z); }
    FixedVec3 operator*(Fixed s) const { return FixedVec3(x * s, y * s, z * s); }
    FixedVec3 operator/(Fixed s) const { return FixedVec3(x / s, y / s, z / s); }

    Fixed dot(FixedVec3 o) const { return x * o.x + y * o.y + z * o.z; }
    Fixed lengthSquared() const { return dot(*this); }
    Fixed length() const { return lengthSquared().sqrt(); }

    FixedVec3 normalized() const {
        Fixed len = length();
        if (len.raw == 0) return FixedVec3(Fixed::ONE_F(), Fixed::ZERO(), Fixed::ZERO());
        return *this / len;
    }
};

} // namespace net

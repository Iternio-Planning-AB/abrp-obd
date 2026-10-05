#pragma once

#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>

using byte = uint8_t;
unsigned long millis();
constexpr double TWO_PI = 6.28318530717958647692;
inline double radians(double value) { return value * TWO_PI / 360; }
inline double degrees(double value) { return value * 360 / TWO_PI; }
inline double sq(double value) { return value * value; }

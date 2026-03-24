#pragma once
#include <cmath>

/// Approximate float equality for test assertions.
inline bool near(float a, float b, float tol = 1e-4f) {
    return fabsf(a - b) <= tol;
}

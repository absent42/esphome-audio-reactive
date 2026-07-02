// Unit tests for TempoEstimator. Native build:
//   g++ -std=c++17 -O2 -I components/audio_reactive \
//       test/test_tempo_estimator/test_tempo_estimator.cpp \
//       -o /tmp/test_tempo_estimator && /tmp/test_tempo_estimator
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "tempo_estimator.h"

using esphome::audio_reactive::TempoEstimator;

void test_grid_constants_consistent() {
    // 60..180 inclusive in 1-BPM steps = 121 candidates.
    assert(TempoEstimator::kNumCandidates ==
           (int)(TempoEstimator::kBpmMax - TempoEstimator::kBpmMin) + 1);
    printf("PASS: test_grid_constants_consistent\n");
}

int main() {
    test_grid_constants_consistent();
    printf("ALL TEMPO ESTIMATOR TESTS PASSED\n");
    return 0;
}

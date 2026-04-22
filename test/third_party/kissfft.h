// Single-header wrapper around vendored kissfft (BSD-3-Clause).
//
// Include this from any host-test source that needs kiss_fft / kiss_fftr.
// The kissfft implementation is compiled by dropping a tiny `kissfft_impl.c`
// (see test_fft_processor/kissfft_impl.c for the template) into each test dir.
//
// Upstream: https://github.com/mborgerding/kissfft
// Vendored files under third_party/kissfft/: kiss_fft.{h,c}, _kiss_fft_guts.h,
// kiss_fftr.{h,c}, kiss_fft_log.h. License: kissfft/LICENSE (BSD-3-Clause).
//
// This wrapper is for host-side native tests only. Device builds use arduinoFFT.

#pragma once

#include "kissfft/kiss_fft.h"
#include "kissfft/kiss_fftr.h"

/*
 * Copyright (C) 2026 Honu Robotics
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Eigen::FFT-backed implementation of the FftwWrapperT 2D complex-to-real
 * inverse-FFT interface used by ehukai.
 */

#ifndef EHUKAI_FFTWWRAPPER_H
#define EHUKAI_FFTWWRAPPER_H

#include <cassert>
#include <cmath>
#include <complex>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <vector>

#include <Eigen/Core>
#include <unsupported/Eigen/FFT>

#include <tbb/blocked_range.h>
#include <tbb/global_control.h>
#include <tbb/parallel_for.h>

// Plan-flag values, kept so call sites compile; ignored here.
#ifndef FFTW_ESTIMATE
#define FFTW_ESTIMATE (1U << 6)
#endif
#ifndef FFTW_DESTROY_INPUT
#define FFTW_DESTROY_INPUT (1U << 0)
#endif
#ifndef FFTW_MEASURE
#define FFTW_MEASURE 0U
#endif
#ifndef FFTW_PATIENT
#define FFTW_PATIENT (1U << 5)
#endif
#ifndef FFTW_EXHAUSTIVE
#define FFTW_EXHAUSTIVE (1U << 3)
#endif
#ifndef FFTW_PRESERVE_INPUT
#define FFTW_PRESERVE_INPUT (1U << 4)
#endif

namespace ehukai
{

namespace detail
{
  template <typename T>
  struct Plan
  {
    int width{0};
    int height{0};
    int outputRowStride{0};
    std::complex<T> *defaultIn{nullptr};
    T *defaultOut{nullptr};
  };

  // Process-wide TBB concurrency cap set by plan_with_nthreads (the FFTW
  // plan_with_nthreads analog). Null means the TBB default (all cores).
  inline std::unique_ptr<tbb::global_control> &ThreadControl()
  {
    static std::unique_ptr<tbb::global_control> ctrl;
    return ctrl;
  }

  // Per-task TBB grain size: columns/rows handled by one task, which reuses a
  // single thread-local Eigen::FFT across them. Small grids collapse to one
  // task (no parallel overhead).
  inline constexpr int kFftGrainSize = 8;

  // 2D complex-to-real inverse FFT.
  // Input:  slow x (fast/2+1) complex, row-major, hermitian along fast.
  // Output: slow x fast real, row-major; row r at out[r * outputRowStride].
  // `fast` (the inner/hermitian dim) must be even: the KissFFT-backed real
  // inverse yields 2*(halfFast-1) reals, which equals `fast` only when `fast`
  // is even. Invalid dimensions or null buffers throw std::invalid_argument —
  // silently returning would hand the caller a stale output buffer with no
  // diagnostic (and an odd `fast` would otherwise overrun the copy-out).
  // The column-pass scratch is a per-call local, so one plan may be executed
  // concurrently with different in/out buffers — matching FFTW's documented
  // thread-safety contract that SpectralSpatialField relies on.
  template <typename T>
  inline void Execute2dC2r(int slow, int fast, int outputRowStride,
                           const std::complex<T> *in, T *out)
  {
    using Complex = std::complex<T>;
    using VecC = Eigen::Matrix<Complex, Eigen::Dynamic, 1>;
    using VecR = Eigen::Matrix<T, Eigen::Dynamic, 1>;

    const int halfFast = (fast / 2) + 1;
    if (slow <= 0 || fast <= 0 || (fast % 2) != 0)
      throw std::invalid_argument(
          "Execute2dC2r: dimensions must be positive with an even inner "
          "(fast) dimension");
    if (in == nullptr || out == nullptr)
      throw std::invalid_argument(
          "Execute2dC2r: input and output buffers must be non-null");

    // Per-call column-pass scratch — never shared, so concurrent executions of
    // the same plan do not race on it.
    std::vector<Complex> scratch(static_cast<std::size_t>(slow) * halfFast);
    Complex *intermediate = scratch.data();

    // Pass 1: column pass (complex-to-complex of length slow), parallel over
    // the independent columns. The Eigen::FFT is thread-local and persists
    // across calls, so its KissFFT twiddle cache is reused frame to frame; a
    // worker only ever touches its own, never concurrently. Unscaled =
    // unnormalized inverse (no 1/N).
    const tbb::blocked_range<int> colRange(0, halfFast, kFftGrainSize);
    tbb::parallel_for(colRange,
        [&](const tbb::blocked_range<int> &range)
        {
          static thread_local Eigen::FFT<T> fft;
          fft.SetFlag(Eigen::FFT<T>::Unscaled);
          VecC colIn(slow);
          VecC colOut(slow);
          for (int j = range.begin(); j != range.end(); ++j)
          {
            for (int i = 0; i < slow; ++i)
              colIn(i) = in[static_cast<std::size_t>(i) * halfFast + j];
            fft.inv(colOut, colIn);
            for (int i = 0; i < slow; ++i)
            {
              const auto idx = static_cast<std::size_t>(i) * halfFast + j;
              intermediate[idx] = colOut(i);
            }
          }
        });

    // Pass 2: row pass (hermitian-to-real of length fast), parallel over the
    // independent rows. HalfSpectrum = N/2+1 hermitian-packed input, N reals.
    const tbb::blocked_range<int> rowRange(0, slow, kFftGrainSize);
    tbb::parallel_for(rowRange,
        [&](const tbb::blocked_range<int> &range)
        {
          static thread_local Eigen::FFT<T> fft;
          fft.SetFlag(Eigen::FFT<T>::Unscaled);
          fft.SetFlag(Eigen::FFT<T>::HalfSpectrum);
          VecC rowIn(halfFast);
          VecR rowOut(fast);
          for (int i = range.begin(); i != range.end(); ++i)
          {
            for (int j = 0; j < halfFast; ++j)
            {
              const auto idx = static_cast<std::size_t>(i) * halfFast + j;
              rowIn(j) = intermediate[idx];
            }
            fft.inv(rowOut, rowIn);
            T *outRow = out + static_cast<std::size_t>(i) * outputRowStride;
            for (int j = 0; j < fast; ++j)
              outRow[j] = rowOut(j);
          }
        });
  }
}  // namespace detail

//-----------------------------------------------------------------------------
// FftwWrapperT — FFTW-shaped facade over the Eigen::FFT inverse transform.
// A single primary template serves both supported precisions: the bodies are
// identical in T, so there is nothing to specialize. (Upstream needed two
// explicit specializations only because FFTW exposed separate fftwf_* / fftw_*
// C symbols.)
//-----------------------------------------------------------------------------
template <typename T>
struct FftwWrapperT
{
  static_assert(std::is_floating_point_v<T>,
                "FftwWrapperT supports only float and double");

  using real_type = T;
  using complex_type = std::complex<T>;
  using plan_type = detail::Plan<T> *;

  static int init_threads() { return 1; }

  // Bound TBB's worker concurrency for subsequent execute() calls, mirroring
  // FFTW's plan_with_nthreads. n <= 0 restores the TBB default (all cores).
  // NOT thread-safe: this swaps a process-global tbb::global_control, so do
  // not call it concurrently with itself or with any execute(); set it up
  // once before parallel work begins (converter constructors call it, so
  // construct converters from one thread). This matches FFTW's own contract,
  // where planner calls must not race with execution.
  static void plan_with_nthreads(int i_nthreads)
  {
    if (i_nthreads <= 0)
    {
      detail::ThreadControl().reset();  // restore the TBB default
      return;
    }
    const auto field = tbb::global_control::max_allowed_parallelism;
    const auto n = static_cast<std::size_t>(i_nthreads);
    detail::ThreadControl() = std::make_unique<tbb::global_control>(field, n);
  }

  // i_width is the slow (outer) dim; i_height is the fast (inner) dim.
  static plan_type plan_dft_c2r_2d(int i_width, int i_height,
                                   complex_type *i_in, real_type *o_out,
                                   unsigned int /*i_flags*/)
  {
    auto *p = new detail::Plan<T>();
    p->width = i_width;
    p->height = i_height;
    p->outputRowStride = i_height;
    p->defaultIn = i_in;
    p->defaultOut = o_out;
    return p;
  }

  // Guru variant. ehukai uses square grids, so this forwards to the plain
  // 2D plan; for a non-square input the upstream FFTW guru path halved the last
  // (width) dim, so the two would diverge.
  static plan_type plan_guru_dft_c2r(int i_width, int i_height,
                                     complex_type *i_in, real_type *o_out,
                                     unsigned int i_flags)
  {
    return plan_dft_c2r_2d(i_width, i_height, i_in, o_out, i_flags);
  }

  static plan_type plan_guru_dft_c2r_output_padded(
      int i_width, int i_height, int i_widthPad, int /*i_heightPad*/,
      complex_type *i_in, real_type *o_out, unsigned int /*i_flags*/)
  {
    auto *p = new detail::Plan<T>();
    p->width = i_width;
    p->height = i_height;
    p->outputRowStride = i_height + i_widthPad;
    p->defaultIn = i_in;
    p->defaultOut = o_out;
    return p;
  }

  static void *Malloc(std::size_t i_size) { return std::malloc(i_size); }
  static void Free(void *i_data) { std::free(i_data); }

  static void execute(plan_type i_plan)
  {
    if (!i_plan) return;
    detail::Execute2dC2r<T>(i_plan->width, i_plan->height,
                            i_plan->outputRowStride,
                            i_plan->defaultIn, i_plan->defaultOut);
  }

  static void execute_dft_c2r(const detail::Plan<T> *i_plan,
                              complex_type *i_in, real_type *o_out)
  {
    if (!i_plan) return;
    detail::Execute2dC2r<T>(i_plan->width, i_plan->height,
                            i_plan->outputRowStride, i_in, o_out);
  }

  static void destroy_plan(plan_type i_plan) { delete i_plan; }
  static void cleanup_threads() {}
  static void cleanup() {}
};

//-----------------------------------------------------------------------------
// GLOBAL THREAD INIT HELPER
//-----------------------------------------------------------------------------
// No global FFT thread state to initialize; retained as a no-op hook so the
// per-call sites need no change.
template <typename T>
inline void FftwInitThreadsT() {}

}  // namespace ehukai

#endif  // EHUKAI_FFTWWRAPPER_H

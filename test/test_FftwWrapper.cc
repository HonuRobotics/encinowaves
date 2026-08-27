/*
 * Copyright (C) 2026 Honu Robotics
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 *
 * Smoke tests for the Eigen-backed FftwWrapper shim:
 *
 *   1. Zero spectrum → zero output (no spurious DC).
 *   2. Pure DC spectrum (only X[0,0] non-zero) → constant output equal to
 *      that DC value. Catches normalization mistakes — if Eigen's default
 *      1/N scaling slipped through, output would be DC/(N1*N2).
 *   3. Pure single-mode spectrum (one sinusoid in the slow direction) →
 *      a single-mode cosine in the spatial domain with amplitude consistent
 *      with FFTW's unnormalized convention.
 *   4-5. Caller-supplied buffers and padded-output row strides.
 *   6-9. Defensive guards (null input and odd inner dim throw
 *      std::invalid_argument, output untouched), the non-padded guru
 *      plan, and the thin threading / alloc / null-plan surface.
 *   10-12. Surface shapes the earlier tests miss: a column-axis cosine (drives
 *      the Hermitian row pass on non-flat input), a sine (verifies phase is
 *      kept), and a diagonal (both axes at once).
 *   13. A random spectrum checked against an independent brute-force inverse
 *      DFT — mirrors real multi-mode, randomized-phase use.
 *   14. A non-square (8x4) grid.
 *   15. A 64x64 grid that splits across TBB tasks — parallel result matches
 *      the serial DC→constant expectation.
 *   16-18. Parallelism + concurrency: a 64x64 sinusoid run serial vs parallel
 *      vs analytic; one plan reused across two execute() calls; and one plan
 *      driven concurrently from several threads (FFTW's thread-safety
 *      contract). 16-18 are from @j-rivero's jrivero/perf_test_no_ci branch.
 *   19. The padded-output row stride on the parallel (multi-grain) grid.
 *   20. plan_with_nthreads actually bounds TBB concurrency (observable cap).
 *   21. The per-thread FFT cache stays correct across a changing transform
 *      length (alternating N on the shared worker pool).
 *
 * The float and double specializations are exercised independently.
 */

#include "ehukai/FftwWrapper.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdlib>
#include <iostream>
#include <random>
#include <stdexcept>
#include <thread>
#include <vector>

#include <tbb/global_control.h>

namespace
{
  /// One-stop checker: prints a banner, runs `fn`, increments
  /// `failures` if `fn` returns false. Cheap test harness so we don't pull
  /// in gtest for a small file.
  template <typename Fn>
  void Check(const char *name, int &failures, Fn fn)
  {
    std::cout << "  test: " << name << " ... ";
    const bool ok = fn();
    std::cout << (ok ? "PASS" : "FAIL") << "\n";
    if (!ok) ++failures;
  }

  /// Hermitian-packed input layout: slow rows × (fast/2+1) cols complex,
  /// stored row-major. Helper to index into it cleanly.
  template <typename T>
  std::complex<T> &At(std::vector<std::complex<T>> &spec,
                       int slow, int fast, int i, int j)
  {
    (void)slow;
    const int halfFast = (fast / 2) + 1;
    return spec[static_cast<std::size_t>(i) * halfFast + j];
  }

  /// Output layout: slow × fast real, row-major.
  template <typename T>
  T &OutAt(std::vector<T> &out, int slow, int fast, int i, int j)
  {
    (void)slow;
    return out[static_cast<std::size_t>(i) * fast + j];
  }

  template <typename T>
  bool MaxAbsDiff(const std::vector<T> &actual, T expected, T tol)
  {
    T maxDiff = T(0);
    for (T v : actual)
    {
      const T d = std::abs(v - expected);
      if (d > maxDiff) maxDiff = d;
    }
    const bool ok = maxDiff <= tol;
    if (!ok)
      std::cout << "(max|v - " << expected << "| = " << maxDiff
                << ", tol = " << tol << ") ";
    return ok;
  }
}  // namespace

template <typename T>
int RunForType(const char *typeName)
{
  std::cout << "== FftwWrapperT<" << typeName << "> ==\n";
  using FFT = ehukai::FftwWrapperT<T>;
  using Complex = std::complex<T>;

  constexpr int Slow = 8;   // = i_width in FFTW guru naming (outer dim)
  constexpr int Fast = 8;   // = i_height (inner dim, hermitian-packed)
  const int halfFast = (Fast / 2) + 1;

  std::vector<Complex> spec(static_cast<std::size_t>(Slow) * halfFast);
  std::vector<T> out(static_cast<std::size_t>(Slow) * Fast);

  int failures = 0;

  // --- Test 1: zero spectrum produces zero output -----------------------
  Check("zero spectrum → zero output", failures, [&]() {
    std::fill(spec.begin(), spec.end(), Complex(0, 0));
    std::fill(out.begin(), out.end(), T(7));  // poison the buffer first
    auto plan = FFT::plan_dft_c2r_2d(Slow, Fast, spec.data(), out.data(), 0u);
    FFT::execute(plan);
    FFT::destroy_plan(plan);
    return MaxAbsDiff(out, T(0), static_cast<T>(1e-5));
  });

  // --- Test 2: DC spectrum → constant output ----------------------------
  // FFTW unnormalized convention: IFFT([X[0,0]=c]) = constant c.
  Check("DC spectrum → constant output (unnormalized)", failures, [&]() {
    std::fill(spec.begin(), spec.end(), Complex(0, 0));
    const T dc = T(3.5);
    At<T>(spec, Slow, Fast, 0, 0) = Complex(dc, 0);
    std::fill(out.begin(), out.end(), T(0));
    auto plan = FFT::plan_dft_c2r_2d(Slow, Fast, spec.data(), out.data(), 0u);
    FFT::execute(plan);
    FFT::destroy_plan(plan);
    return MaxAbsDiff(out, dc, static_cast<T>(1e-5));
  });

  // --- Test 3: single slow-direction mode → real cosine ------------------
  // For real spatial pattern x[i, j] = cos(2π·i / Slow), the hermitian
  // spectrum has only two non-zero entries: X[1, 0] and X[Slow-1, 0],
  // each = (Slow * Fast) / 2 under FFTW's unnormalized forward.
  // IFFT of that spectrum gives back (Slow * Fast) * cos(2π·i / Slow).
  Check("slow-direction cosine → cos with unnormalized amplitude",
        failures, [&]() {
    std::fill(spec.begin(), spec.end(), Complex(0, 0));
    const T amp = T(Slow) * T(Fast) * T(0.5);
    At<T>(spec, Slow, Fast, 1, 0) = Complex(amp, 0);
    At<T>(spec, Slow, Fast, Slow - 1, 0) = Complex(amp, 0);
    std::fill(out.begin(), out.end(), T(0));
    auto plan = FFT::plan_dft_c2r_2d(Slow, Fast, spec.data(), out.data(), 0u);
    FFT::execute(plan);
    FFT::destroy_plan(plan);

    // Reference: every cell should be (Slow * Fast) * cos(2π·i / Slow).
    const T scale = T(Slow) * T(Fast);
    const T tol = static_cast<T>(1e-4);
    T maxDiff = T(0);
    for (int i = 0; i < Slow; ++i)
    {
      const T expected = scale *
          static_cast<T>(std::cos(2.0 * M_PI *
                                  static_cast<double>(i) / Slow));
      for (int j = 0; j < Fast; ++j)
      {
        const T d = std::abs(OutAt<T>(out, Slow, Fast, i, j) - expected);
        if (d > maxDiff) maxDiff = d;
      }
    }
    if (maxDiff > tol)
      std::cout << "(max diff = " << maxDiff << ", tol = " << tol << ") ";
    return maxDiff <= tol;
  });

  // --- Test 4: execute_dft_c2r against a different buffer pair ----------
  // The plan should be reusable on caller-supplied in/out. We feed a DC
  // spectrum but to a NEW output buffer, and verify that the result lands
  // in that buffer (not the plan's captured default).
  Check("execute_dft_c2r with caller-supplied buffers", failures, [&]() {
    std::vector<Complex> spec2(spec.size(), Complex(0, 0));
    std::vector<T> out2(out.size(), T(0));
    const T dc = T(-1.25);
    At<T>(spec2, Slow, Fast, 0, 0) = Complex(dc, 0);

    // Plan captures `spec` / `out` as defaults — neither should be touched.
    std::fill(out.begin(), out.end(), T(99));
    std::fill(spec.begin(), spec.end(), Complex(0, 0));
    auto plan = FFT::plan_dft_c2r_2d(Slow, Fast, spec.data(), out.data(), 0u);

    FFT::execute_dft_c2r(plan, spec2.data(), out2.data());
    FFT::destroy_plan(plan);

    // The defaults must be untouched, the caller buffer holds the result.
    if (!MaxAbsDiff(out2, dc, static_cast<T>(1e-5))) return false;
    const bool untouched =
        std::all_of(out.begin(), out.end(), [](T v) { return v == T(99); });
    if (!untouched) std::cout << "(default buffer was clobbered) ";
    return untouched;
  });

  // --- Test 5: padded-output plan honours the wider row stride -----------
  // A `widthPad` of 2 should make every output row 2 elements wider than
  // `Fast`. The valid `Fast` columns hold the IFFT result, and the trailing
  // 2 columns must be untouched (still the poison value 0xCD).
  Check("plan_guru_dft_c2r_output_padded honours pad", failures, [&]() {
    const int widthPad = 2;
    std::vector<Complex> spec3(static_cast<std::size_t>(Slow) * halfFast,
                                Complex(0, 0));
    std::vector<T> out3(
        static_cast<std::size_t>(Slow) * (Fast + widthPad), T(-77));
    const T dc = T(2);
    At<T>(spec3, Slow, Fast, 0, 0) = Complex(dc, 0);

    auto plan = FFT::plan_guru_dft_c2r_output_padded(
        Slow, Fast, widthPad, 0 /*heightPad*/,
        spec3.data(), out3.data(), 0u);
    FFT::execute(plan);
    FFT::destroy_plan(plan);

    const T tol = static_cast<T>(1e-5);
    for (int i = 0; i < Slow; ++i)
    {
      // valid cols 0..Fast-1 must equal `dc`
      for (int j = 0; j < Fast; ++j)
      {
        const T v = out3[i * (Fast + widthPad) + j];
        if (std::abs(v - dc) > tol)
        {
          std::cout << "(valid col mismatch at (" << i << "," << j << ")) ";
          return false;
        }
      }
      // pad cols Fast..Fast+widthPad-1 must still be the poison value
      for (int j = Fast; j < Fast + widthPad; ++j)
      {
        const T v = out3[i * (Fast + widthPad) + j];
        if (v != T(-77))
        {
          std::cout << "(pad col was written at (" << i << "," << j << ")) ";
          return false;
        }
      }
    }
    return true;
  });

  // --- Test 6: defensive guard — invalid args throw -----------------------
  // A null input pointer makes the IFFT throw std::invalid_argument without
  // touching the output buffer.
  Check("invalid args → throw, output untouched", failures, [&]() {
    std::fill(out.begin(), out.end(), T(42));
    auto plan = FFT::plan_dft_c2r_2d(Slow, Fast, spec.data(), out.data(), 0u);
    bool threw = false;
    try
    {
      FFT::execute_dft_c2r(plan, nullptr, out.data());  // in == nullptr
    }
    catch (const std::invalid_argument &)
    {
      threw = true;
    }
    FFT::destroy_plan(plan);
    if (!threw)
    {
      std::cout << "(no exception thrown) ";
      return false;
    }
    const bool untouched =
        std::all_of(out.begin(), out.end(), [](T v) { return v == T(42); });
    if (!untouched) std::cout << "(buffer was modified) ";
    return untouched;
  });

  // --- Test 7: non-padded guru plan behaves like the basic plan ----------
  Check("plan_guru_dft_c2r → constant output (DC)", failures, [&]() {
    std::fill(spec.begin(), spec.end(), Complex(0, 0));
    const T dc = T(1.75);
    At<T>(spec, Slow, Fast, 0, 0) = Complex(dc, 0);
    std::fill(out.begin(), out.end(), T(0));
    auto plan = FFT::plan_guru_dft_c2r(Slow, Fast, spec.data(), out.data(), 0u);
    FFT::execute(plan);
    FFT::destroy_plan(plan);
    return MaxAbsDiff(out, dc, static_cast<T>(1e-5));
  });

  // --- Test 8: thin API surface (threads, alloc, null-plan no-ops) -------
  // These are no-ops or trivial wrappers kept for FFTW call-site
  // compatibility; exercise them so the whole shim is covered.
  Check("threading / alloc / null-plan no-op surface", failures, [&]() {
    ehukai::FftwInitThreadsT<T>();  // no-op global hook
    if (FFT::init_threads() != 1) return false;
    FFT::plan_with_nthreads(2);  // bound TBB concurrency...
    FFT::plan_with_nthreads(0);  // ...then restore the default

    void *buf = FFT::Malloc(64);
    if (buf == nullptr) return false;
    FFT::Free(buf);

    FFT::execute(nullptr);                                   // null-plan guard
    FFT::execute_dft_c2r(nullptr, spec.data(), out.data());  // null-plan guard
    FFT::destroy_plan(nullptr);                              // delete nullptr

    FFT::cleanup_threads();
    FFT::cleanup();
    return true;
  });

  // --- Test 9: odd inner dimension is rejected (memory-safety guard) ------
  // The KissFFT real inverse requires an even `fast`; an odd `fast` must
  // throw std::invalid_argument without touching the output (no heap
  // overrun).
  Check("odd inner dim → throw, output untouched", failures, [&]() {
    constexpr int OddFast = 7;
    const int oddHalf = (OddFast / 2) + 1;
    std::vector<Complex> oddSpec(
        static_cast<std::size_t>(Slow) * oddHalf, Complex(1, 0));
    std::vector<T> oddOut(static_cast<std::size_t>(Slow) * OddFast, T(13));
    auto plan = FFT::plan_dft_c2r_2d(Slow, OddFast, oddSpec.data(),
                                     oddOut.data(), 0u);
    bool threw = false;
    try
    {
      FFT::execute(plan);
    }
    catch (const std::invalid_argument &)
    {
      threw = true;
    }
    FFT::destroy_plan(plan);
    if (!threw)
    {
      std::cout << "(no exception thrown) ";
      return false;
    }
    const bool untouched = std::all_of(oddOut.begin(), oddOut.end(),
                                       [](T v) { return v == T(13); });
    if (!untouched) std::cout << "(odd-dim output was written) ";
    return untouched;
  });

  // --- Test 10: cosine along the columns (the fast / Hermitian axis) -----
  // Mirror of Test 3 but across the columns. A single bin X[0,1] (no slow
  // conjugate partner — the fast axis is the half-stored one) must yield
  // out[i][j] = scale * cos(2π·j / Fast). This drives the Hermitian row pass
  // on non-flat input, which Tests 1-9 never do.
  Check("column cosine → cos along the fast axis", failures, [&]() {
    std::fill(spec.begin(), spec.end(), Complex(0, 0));
    const T amp = T(Slow) * T(Fast) * T(0.5);
    At<T>(spec, Slow, Fast, 0, 1) = Complex(amp, 0);
    std::fill(out.begin(), out.end(), T(0));
    auto plan = FFT::plan_dft_c2r_2d(Slow, Fast, spec.data(), out.data(), 0u);
    FFT::execute(plan);
    FFT::destroy_plan(plan);

    const T scale = T(Slow) * T(Fast);
    const T tol = static_cast<T>(1e-4);
    T maxDiff = T(0);
    for (int i = 0; i < Slow; ++i)
    {
      for (int j = 0; j < Fast; ++j)
      {
        const T expected = scale * static_cast<T>(
            std::cos(2.0 * M_PI * static_cast<double>(j) / Fast));
        const T d = std::abs(OutAt<T>(out, Slow, Fast, i, j) - expected);
        if (d > maxDiff) maxDiff = d;
      }
    }
    if (maxDiff > tol)
      std::cout << "(max diff = " << maxDiff << ", tol = " << tol << ") ";
    return maxDiff <= tol;
  });

  // --- Test 11: sine / phase (imaginary coefficients) --------------------
  // Same bins as Test 3 but imaginary, so the result is a sine. If Test 3
  // (cosine) passes and this fails, phase is being dropped somewhere.
  Check("slow-direction sine → sin (phase preserved)", failures, [&]() {
    std::fill(spec.begin(), spec.end(), Complex(0, 0));
    const T amp = T(Slow) * T(Fast) * T(0.5);
    At<T>(spec, Slow, Fast, 1, 0) = Complex(0, -amp);
    At<T>(spec, Slow, Fast, Slow - 1, 0) = Complex(0, amp);
    std::fill(out.begin(), out.end(), T(0));
    auto plan = FFT::plan_dft_c2r_2d(Slow, Fast, spec.data(), out.data(), 0u);
    FFT::execute(plan);
    FFT::destroy_plan(plan);

    const T scale = T(Slow) * T(Fast);
    const T tol = static_cast<T>(1e-4);
    T maxDiff = T(0);
    for (int i = 0; i < Slow; ++i)
    {
      const T expected = scale * static_cast<T>(
          std::sin(2.0 * M_PI * static_cast<double>(i) / Slow));
      for (int j = 0; j < Fast; ++j)
      {
        const T d = std::abs(OutAt<T>(out, Slow, Fast, i, j) - expected);
        if (d > maxDiff) maxDiff = d;
      }
    }
    if (maxDiff > tol)
      std::cout << "(max diff = " << maxDiff << ", tol = " << tol << ") ";
    return maxDiff <= tol;
  });

  // --- Test 12: diagonal mode (both axes at once) ------------------------
  // One bin X[1,1]; the half-spectrum supplies the conjugate partner, giving
  // out[i][j] = scale * cos(2π·(i/Slow + j/Fast)). Exercises both passes.
  Check("diagonal cosine → cos(i/Slow + j/Fast)", failures, [&]() {
    std::fill(spec.begin(), spec.end(), Complex(0, 0));
    const T amp = T(Slow) * T(Fast) * T(0.5);
    At<T>(spec, Slow, Fast, 1, 1) = Complex(amp, 0);
    std::fill(out.begin(), out.end(), T(0));
    auto plan = FFT::plan_dft_c2r_2d(Slow, Fast, spec.data(), out.data(), 0u);
    FFT::execute(plan);
    FFT::destroy_plan(plan);

    const T scale = T(Slow) * T(Fast);
    const T tol = static_cast<T>(1e-4);
    T maxDiff = T(0);
    for (int i = 0; i < Slow; ++i)
    {
      for (int j = 0; j < Fast; ++j)
      {
        const double phase = 2.0 * M_PI *
            (static_cast<double>(i) / Slow + static_cast<double>(j) / Fast);
        const T expected = scale * static_cast<T>(std::cos(phase));
        const T d = std::abs(OutAt<T>(out, Slow, Fast, i, j) - expected);
        if (d > maxDiff) maxDiff = d;
      }
    }
    if (maxDiff > tol)
      std::cout << "(max diff = " << maxDiff << ", tol = " << tol << ") ";
    return maxDiff <= tol;
  });

  // --- Test 13: random field vs. an independent brute-force inverse DFT --
  // Fill the free columns (kf = 1 .. Fast/2-1) with random complex values and
  // leave the self-conjugate columns (kf = 0, Fast/2) zero so the result is
  // real. Compare the shim against a direct inverse DFT over the same
  // Hermitian-completed spectrum — a reference independent of the shim.
  Check("random spectrum → matches brute-force inverse DFT", failures, [&]() {
    // NOLINTNEXTLINE(cert-msc32-c,cert-msc51-cpp): reproducible test seed
    std::mt19937 rng(12345u);
    std::uniform_real_distribution<double> dist(-1.0, 1.0);
    std::fill(spec.begin(), spec.end(), Complex(0, 0));
    for (int ks = 0; ks < Slow; ++ks)
      for (int kf = 1; kf < Fast / 2; ++kf)
        At<T>(spec, Slow, Fast, ks, kf) =
            Complex(static_cast<T>(dist(rng)), static_cast<T>(dist(rng)));

    std::fill(out.begin(), out.end(), T(0));
    auto plan = FFT::plan_dft_c2r_2d(Slow, Fast, spec.data(), out.data(), 0u);
    FFT::execute(plan);
    FFT::destroy_plan(plan);

    // Complete the full Fast spectrum via Hermitian symmetry,
    // X[ks][Fast-kf] = conj(X[(Slow-ks) % Slow][kf]).
    std::vector<std::complex<double>> full(
        static_cast<std::size_t>(Slow) * Fast);
    for (int ks = 0; ks < Slow; ++ks)
    {
      for (int kf = 0; kf < Fast; ++kf)
      {
        // Above the Nyquist column, mirror to the stored conjugate partner.
        const bool mirror = kf > Fast / 2;
        const int sks = mirror ? (Slow - ks) % Slow : ks;
        const int skf = mirror ? Fast - kf : kf;
        const Complex &s = At<T>(spec, Slow, Fast, sks, skf);
        std::complex<double> x(s.real(), s.imag());
        if (mirror) x = std::conj(x);
        full[static_cast<std::size_t>(ks) * Fast + kf] = x;
      }
    }

    const T tol = static_cast<T>(1e-3);
    T maxDiff = T(0);
    for (int i = 0; i < Slow; ++i)
    {
      for (int j = 0; j < Fast; ++j)
      {
        std::complex<double> acc(0, 0);
        for (int ks = 0; ks < Slow; ++ks)
        {
          for (int kf = 0; kf < Fast; ++kf)
          {
            const double ang = 2.0 * M_PI *
                (static_cast<double>(ks) * i / Slow +
                 static_cast<double>(kf) * j / Fast);
            acc += full[static_cast<std::size_t>(ks) * Fast + kf] *
                   std::complex<double>(std::cos(ang), std::sin(ang));
          }
        }
        const T d = std::abs(OutAt<T>(out, Slow, Fast, i, j) -
                             static_cast<T>(acc.real()));
        if (d > maxDiff) maxDiff = d;
      }
    }
    if (maxDiff > tol)
      std::cout << "(max diff = " << maxDiff << ", tol = " << tol << ") ";
    return maxDiff <= tol;
  });

  // --- Test 14: non-square grid (Slow != Fast) ---------------------------
  // plan_dft_c2r_2d handles rectangles; pin a DC spectrum → constant on an
  // 8x4 grid so a future change cannot silently break non-square support.
  Check("non-square 8x4 DC spectrum → constant", failures, [&]() {
    constexpr int NsSlow = 8;
    constexpr int NsFast = 4;
    const int nsHalf = (NsFast / 2) + 1;
    std::vector<Complex> nsSpec(
        static_cast<std::size_t>(NsSlow) * nsHalf, Complex(0, 0));
    std::vector<T> nsOut(static_cast<std::size_t>(NsSlow) * NsFast, T(0));
    const T dc = T(2);
    nsSpec[0] = Complex(dc, 0);  // X[0,0]
    auto plan = FFT::plan_dft_c2r_2d(NsSlow, NsFast, nsSpec.data(),
                                     nsOut.data(), 0u);
    FFT::execute(plan);
    FFT::destroy_plan(plan);
    return MaxAbsDiff(nsOut, dc, static_cast<T>(1e-5));
  });

  // --- Test 15: larger grid exercises the parallel multi-grain path -------
  // 64x64 forces tbb::blocked_range to split across tasks; a DC spectrum must
  // still yield a constant field (parallel result == serial expectation).
  Check("64x64 DC spectrum → constant (parallel grains)", failures, [&]() {
    constexpr int N = 64;
    const int nHalf = (N / 2) + 1;
    std::vector<Complex> bigSpec(static_cast<std::size_t>(N) * nHalf,
                                 Complex(0, 0));
    std::vector<T> bigOut(static_cast<std::size_t>(N) * N, T(0));
    const T dc = T(2.5);
    bigSpec[0] = Complex(dc, 0);  // X[0,0]
    auto plan = FFT::plan_dft_c2r_2d(N, N, bigSpec.data(), bigOut.data(), 0u);
    FFT::execute(plan);
    FFT::destroy_plan(plan);
    return MaxAbsDiff(bigOut, dc, static_cast<T>(1e-4));
  });

  // --- Test 16: 64x64 sinusoid → serial == parallel == analytic ----------
  // Test 15's DC spectrum yields a constant field, blind to column/row order.
  // Here a 2D sinusoid with energy in BOTH directions makes every cell
  // position-dependent. Under the unnormalized inverse the hermitian-packed
  //     X[1,0] = X[N-1,0] = X[0,1] = N*N/2
  // invert to  x[i,j] = N*N * (cos(2π·i/N) + cos(2π·j/N)).  At N=64 both passes
  // split into TBB grains: (a) serial must match the closed form, (b) parallel
  // must be bit-identical to serial — (a) catches grain-ordering, (b) a race.
  Check("64x64 sinusoid → serial == parallel == analytic", failures, [&]() {
    constexpr int N = 64;
    const int nHalf = (N / 2) + 1;
    std::vector<Complex> sinSpec(static_cast<std::size_t>(N) * nHalf,
                                 Complex(0, 0));
    const T amp = T(N) * T(N) * T(0.5);
    At<T>(sinSpec, N, N, 1, 0) = Complex(amp, 0);
    At<T>(sinSpec, N, N, N - 1, 0) = Complex(amp, 0);
    At<T>(sinSpec, N, N, 0, 1) = Complex(amp, 0);

    std::vector<T> outSerial(static_cast<std::size_t>(N) * N, T(0));
    std::vector<T> outParallel(static_cast<std::size_t>(N) * N, T(0));

    auto plan =
        FFT::plan_dft_c2r_2d(N, N, sinSpec.data(), outSerial.data(), 0u);
    FFT::plan_with_nthreads(1);  // cap TBB to one thread → serial
    FFT::execute(plan);          // → outSerial (the plan's default output)
    FFT::plan_with_nthreads(0);  // restore the TBB default (all cores)
    FFT::execute_dft_c2r(plan, sinSpec.data(), outParallel.data());
    FFT::destroy_plan(plan);

    const T scale = T(N) * T(N);
    const T tol = scale * static_cast<T>(1e-4);
    T maxAnalytic = T(0);
    T maxSerialVsParallel = T(0);
    for (int i = 0; i < N; ++i)
    {
      for (int j = 0; j < N; ++j)
      {
        const T expected = scale *
            (static_cast<T>(std::cos(2.0 * M_PI * i / N)) +
             static_cast<T>(std::cos(2.0 * M_PI * j / N)));
        const T s = OutAt<T>(outSerial, N, N, i, j);
        const T p = OutAt<T>(outParallel, N, N, i, j);
        maxAnalytic = std::max(maxAnalytic, std::abs(s - expected));
        maxSerialVsParallel = std::max(maxSerialVsParallel, std::abs(s - p));
      }
    }
    if (maxSerialVsParallel != T(0))
    {
      std::cout << "(serial != parallel, max diff = " << maxSerialVsParallel
                << ") ";
      return false;
    }
    if (maxAnalytic > tol)
    {
      std::cout << "(analytic mismatch, max diff = " << maxAnalytic
                << ", tol = " << tol << ") ";
      return false;
    }
    return true;
  });

  // --- Test 17: one plan, two execute() calls → independent outputs -------
  // A plan reused across execute() calls must produce independent, correct
  // outputs. Per-call scratch (see FftwWrapper.h) means frame A cannot leak
  // into frame B; this pins that. N=64 so the reuse also covers the parallel
  // path.
  Check("same plan, two execute() calls → independent correct outputs",
        failures, [&]() {
    constexpr int N = 64;
    const int nHalf = (N / 2) + 1;
    std::vector<Complex> specR(static_cast<std::size_t>(N) * nHalf,
                               Complex(0, 0));
    std::vector<T> outR(static_cast<std::size_t>(N) * N, T(0));
    auto plan = FFT::plan_dft_c2r_2d(N, N, specR.data(), outR.data(), 0u);

    const T scale = T(N) * T(N);
    const T tol = scale * static_cast<T>(1e-4);

    // Frame A: a slow-direction cosine → N*N·cos(2π·i/N).
    const T amp = scale * T(0.5);
    At<T>(specR, N, N, 1, 0) = Complex(amp, 0);
    At<T>(specR, N, N, N - 1, 0) = Complex(amp, 0);
    std::fill(outR.begin(), outR.end(), T(-123));  // poison before execute
    FFT::execute(plan);
    for (int i = 0; i < N; ++i)
    {
      const T expected =
          scale * static_cast<T>(std::cos(2.0 * M_PI * i / N));
      for (int j = 0; j < N; ++j)
      {
        if (std::abs(OutAt<T>(outR, N, N, i, j) - expected) > tol)
        {
          std::cout << "(frame A mismatch at (" << i << "," << j << ")) ";
          FFT::destroy_plan(plan);
          return false;
        }
      }
    }

    // Frame B: a pure DC spectrum → constant field, through the same plan.
    std::fill(specR.begin(), specR.end(), Complex(0, 0));
    const T dc = T(5.5);
    specR[0] = Complex(dc, 0);  // X[0,0]
    std::fill(outR.begin(), outR.end(), T(-123));  // poison again
    FFT::execute(plan);
    FFT::destroy_plan(plan);
    return MaxAbsDiff(outR, dc, static_cast<T>(1e-4));
  });

  // --- Test 18: concurrent shared-plan execute (parallelism + concurrency) -
  // FFTW documents execute_dft_c2r as safe to call concurrently on one plan
  // with different in/out arrays (the plan stays read-only). Drive ONE shared
  // plan from several threads, each inverting its own sinusoid; every thread
  // must read back only its own field. Guards the per-call scratch —
  // re-introducing shared plan-owned scratch would corrupt cells here. N=64
  // also splits each execute across TBB grains, so this hits parallelism
  // (within an execute) and concurrency (across executes) at once.
  //
  // Hardened over the original: (a) a start barrier so every driver enters its
  // first execute together, maximizing the overlap window a race needs; (b) the
  // thread count scales with the hardware; (c) each result is compared
  // bit-exact against a serially-computed golden for that thread's spectrum
  // (Test 16 pins serial == parallel), catching any deviation regardless of
  // magnitude — the old per-thread tolerance could mask a same-magnitude
  // cross-thread swap.
  Check("concurrent shared-plan execute → each thread stays correct "
        "(parallelism + concurrency)", failures, [&]() {
    constexpr int N = 64;
    const int hw = static_cast<int>(std::thread::hardware_concurrency());
    const int kThreads = std::max(4, hw);
    // Many rounds give the nondeterministic corruption a chance to show in a
    // plain build; under a sanitizer the race is caught on the first concurrent
    // access, so a few rounds keep the (heavily instrumented) CI job fast.
#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
    constexpr int kRounds = 3;
#else
    constexpr int kRounds = 100;
#endif
    const int nHalf = (N / 2) + 1;
    const T scale = T(N) * T(N);

    // Per-thread distinct spectra. Thread t's amplitude is (t+1)×, so a cell
    // leaking from thread t' shows up as the wrong value.
    std::vector<std::vector<Complex>> specs(kThreads);
    std::vector<std::vector<T>> outs(kThreads);
    std::vector<std::vector<T>> goldens(kThreads);
    for (int t = 0; t < kThreads; ++t)
    {
      specs[t].assign(static_cast<std::size_t>(N) * nHalf, Complex(0, 0));
      outs[t].assign(static_cast<std::size_t>(N) * N, T(0));
      const T amp = scale * T(0.5) * static_cast<T>(t + 1);
      At<T>(specs[t], N, N, 1, 0) = Complex(amp, 0);
      At<T>(specs[t], N, N, N - 1, 0) = Complex(amp, 0);
      At<T>(specs[t], N, N, 0, 1) = Complex(amp, 0);
    }

    // One plan, deliberately shared across all driver threads.
    auto plan = FFT::plan_dft_c2r_2d(N, N, specs[0].data(), outs[0].data(), 0u);

    // Serial golden per thread — the reference each concurrent result must
    // equal bit-for-bit. Computed before the concurrent phase, single-threaded.
    FFT::plan_with_nthreads(1);
    for (int t = 0; t < kThreads; ++t)
    {
      goldens[t].assign(static_cast<std::size_t>(N) * N, T(0));
      FFT::execute_dft_c2r(plan, specs[t].data(), goldens[t].data());
    }
    FFT::plan_with_nthreads(0);  // restore parallel for the concurrent phase

    std::atomic<int> badCells{0};
    std::atomic<int> arrived{0};
    std::atomic<bool> go{false};
    auto driver = [&](int t)
    {
      arrived.fetch_add(1, std::memory_order_acq_rel);
      // all drivers start their first execute() together (a release barrier)
      while (!go.load(std::memory_order_acquire))
        std::this_thread::yield();

      for (int r = 0; r < kRounds; ++r)
      {
        std::fill(outs[t].begin(), outs[t].end(), T(-999));  // poison
        FFT::execute_dft_c2r(plan, specs[t].data(), outs[t].data());
        for (std::size_t k = 0; k < outs[t].size(); ++k)
        {
          const T v = outs[t][k];
          if (!std::isfinite(v) || v != goldens[t][k])
            badCells.fetch_add(1, std::memory_order_relaxed);
        }
      }
    };

    std::vector<std::thread> pool;
    pool.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t)
      pool.emplace_back(driver, t);
    while (arrived.load(std::memory_order_acquire) < kThreads)
      std::this_thread::yield();
    go.store(true, std::memory_order_release);  // release the barrier
    for (auto &th : pool)
      th.join();

    FFT::destroy_plan(plan);

    const int bad = badCells.load();
    if (bad != 0)
    {
      std::cout << "(" << bad << " corrupted cells over " << kThreads
                << " threads x " << kRounds
                << " rounds — concurrent shared-plan executes raced) ";
      return false;
    }
    return true;
  });

  // --- Test 19: padded output on the parallel grid (stride preserved) ------
  // The padded plan writes each row at out + i*outputRowStride, with
  // outputRowStride = fast + widthPad. Test 5 covers padding only on an 8x8 DC
  // field — one TBB grain and position-independent, so a row/stride mis-map
  // would go unseen. Here a 128x128 sinusoid (energy on BOTH axes, so every
  // cell is position-dependent) splits across many grains and is written
  // through a non-trivial pad: a stride bug or a cross-grain race on padded
  // rows corrupts the valid region or leaks into the pad.
  Check("128x128 sinusoid, padded output → parallel stride preserved",
        failures, [&]() {
    constexpr int N = 128;
    constexpr int widthPad = 3;  // non-trivial, odd → row stride 131
    const int nHalf = (N / 2) + 1;
    const int stride = N + widthPad;
    std::vector<Complex> spec19(static_cast<std::size_t>(N) * nHalf,
                                Complex(0, 0));
    std::vector<T> out19(static_cast<std::size_t>(N) * stride, T(-777));
    const T amp = T(N) * T(N) * T(0.5);
    At<T>(spec19, N, N, 1, 0) = Complex(amp, 0);
    At<T>(spec19, N, N, N - 1, 0) = Complex(amp, 0);
    At<T>(spec19, N, N, 0, 1) = Complex(amp, 0);

    FFT::plan_with_nthreads(0);  // default = parallel over all cores
    auto plan = FFT::plan_guru_dft_c2r_output_padded(
        N, N, widthPad, 0 /*heightPad*/, spec19.data(), out19.data(), 0u);
    FFT::execute(plan);
    FFT::destroy_plan(plan);

    const T scale = T(N) * T(N);
    const T tol = scale * static_cast<T>(1e-4);
    for (int i = 0; i < N; ++i)
    {
      // valid columns 0..N-1, indexed through the padded row stride
      for (int j = 0; j < N; ++j)
      {
        const T expected = scale *
            (static_cast<T>(std::cos(2.0 * M_PI * i / N)) +
             static_cast<T>(std::cos(2.0 * M_PI * j / N)));
        const T v = out19[static_cast<std::size_t>(i) * stride + j];
        if (!std::isfinite(v) || std::abs(v - expected) > tol)
        {
          std::cout << "(valid mismatch at (" << i << "," << j << "), got "
                    << v << " want " << expected << ") ";
          return false;
        }
      }
      // pad columns N..stride-1 must stay the poison value
      for (int j = N; j < stride; ++j)
      {
        if (out19[static_cast<std::size_t>(i) * stride + j] != T(-777))
        {
          std::cout << "(pad col written at (" << i << "," << j << ")) ";
          return false;
        }
      }
    }
    return true;
  });

  // --- Test 20: plan_with_nthreads actually bounds TBB concurrency ---------
  // Tests 8/16 call plan_with_nthreads but never check the cap took effect —
  // correctness is independent of thread count, so a silently-broken
  // global_control would still pass them (and would silently turn Test 16's
  // serial-vs-parallel leg into parallel-vs-parallel). Assert the cap is
  // observable through TBB's own active_value and that 0 restores the default.
  Check("plan_with_nthreads bounds observed concurrency", failures, [&]() {
    using GC = tbb::global_control;
    FFT::plan_with_nthreads(0);  // clear any cap left by a prior test
    const std::size_t defaultLimit =
        GC::active_value(GC::max_allowed_parallelism);

    FFT::plan_with_nthreads(2);
    const std::size_t capped = GC::active_value(GC::max_allowed_parallelism);

    FFT::plan_with_nthreads(0);
    const std::size_t restored =
        GC::active_value(GC::max_allowed_parallelism);

    if (capped != 2)
    {
      std::cout << "(cap not applied: active_value = " << capped
                << ", expected 2) ";
      return false;
    }
    if (restored != defaultLimit)
    {
      std::cout << "(cap not lifted: active_value = " << restored
                << ", expected default " << defaultLimit << ") ";
      return false;
    }
    return true;
  });

  // --- Test 21: per-thread FFT cache survives a changing transform length --
  // Each TBB worker reuses a static thread_local Eigen::FFT across calls. Its
  // riskiest failure mode is carrying a twiddle cache sized for one N into a
  // later transform of a different N. Every other test uses a fixed N, so this
  // alternates N=64 and N=96 back-to-back on the parallel path (same process,
  // same worker pool) and checks each against its analytic field — a stale
  // length would surface as a wrong-amplitude or NaN cell.
  Check("per-thread cache: alternating grid sizes → no stale state",
        failures, [&]() {
    FFT::plan_with_nthreads(0);  // default = parallel, workers shared/recycled
    auto runSinusoid = [&](int N) -> bool
    {
      const int nHalf = (N / 2) + 1;
      std::vector<Complex> spec21(static_cast<std::size_t>(N) * nHalf,
                                  Complex(0, 0));
      std::vector<T> out21(static_cast<std::size_t>(N) * N, T(-555));
      const T amp = T(N) * T(N) * T(0.5);
      At<T>(spec21, N, N, 1, 0) = Complex(amp, 0);
      At<T>(spec21, N, N, N - 1, 0) = Complex(amp, 0);
      At<T>(spec21, N, N, 0, 1) = Complex(amp, 0);

      auto plan = FFT::plan_dft_c2r_2d(N, N, spec21.data(), out21.data(), 0u);
      FFT::execute(plan);
      FFT::destroy_plan(plan);

      const T scale = T(N) * T(N);
      const T tol = scale * static_cast<T>(1e-3);
      for (int i = 0; i < N; ++i)
      {
        for (int j = 0; j < N; ++j)
        {
          const T expected = scale *
              (static_cast<T>(std::cos(2.0 * M_PI * i / N)) +
               static_cast<T>(std::cos(2.0 * M_PI * j / N)));
          const T v = OutAt<T>(out21, N, N, i, j);
          if (!std::isfinite(v) || std::abs(v - expected) > tol)
          {
            std::cout << "(N=" << N << " mismatch at (" << i << "," << j
                      << "), got " << v << " want " << expected << ") ";
            return false;
          }
        }
      }
      return true;
    };

    for (int iter = 0; iter < 10; ++iter)
    {
      if (!runSinusoid(64)) return false;  // length A
      if (!runSinusoid(96)) return false;  // length B → forces a re-plan
    }
    return true;
  });

  if (failures == 0)
    std::cout << "  OK\n\n";
  else
    std::cout << "  FAILURES: " << failures << "\n\n";
  return failures;
}

int main()
{
  std::cout << "FftwWrapper Eigen-backed shim smoke tests\n\n";

  int total = 0;
  total += RunForType<float>("float");
  total += RunForType<double>("double");

  std::cout << "Summary: " << (total == 0 ? "all tests passed"
                                          : "FAILURES present")
            << " (" << total << " failed)\n";
  return total == 0 ? 0 : 1;
}

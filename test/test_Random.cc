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
 * Determinism and sanity tests for the random machinery:
 *
 *   1. SeedFromWavenumber is a pure function: equal inputs give equal
 *      seeds (including negative wavenumber components, which historically
 *      hit float-to-unsigned UB), and distinct wavenumbers or base seeds
 *      give distinct seeds.
 *   2. NormalRandom / LogNormalRandom reseeded from the same wavenumber
 *      produce identical draw sequences; different base seeds diverge.
 *      LogNormal amplitudes are positive, phases lie in [0, tau).
 *   3. Two InitialState constructions from identical Parameters produce
 *      bit-identical spectral buffers, for both the Normal and LogNormal
 *      random types (the LogNormal cascade path was previously never
 *      executed by any test); changing the seed changes the ocean.
 *
 * Determinism is asserted within one platform/standard library only —
 * std::normal_distribution sequences are not portable across
 * implementations.
 */

#include "ehukai/All.h"

#include <cmath>
#include <cstring>
#include <iostream>
#include <string>

int main()
{
  using namespace ehukai;

  std::cout << "Random machinery determinism tests\n\n";

  bool ok = true;
  auto demand = [&](bool cond, const std::string &msg) {
    std::cout << "  " << (cond ? "[ok]   " : "[FAIL] ") << msg << "\n";
    if (!cond) ok = false;
  };

  // --- 1. SeedFromWavenumber ----------------------------------------------
  {
    const Imath::V2f kPos(1.5f, 2.5f);
    const Imath::V2f kNeg(1.5f, -2.5f);   // negative component: the old UB case
    demand(SeedFromWavenumber(kPos, 54321u) == SeedFromWavenumber(kPos, 54321u),
           "SeedFromWavenumber: deterministic for equal inputs");
    demand(SeedFromWavenumber(kNeg, 54321u) == SeedFromWavenumber(kNeg, 54321u),
           "SeedFromWavenumber: deterministic for negative components");
    demand(SeedFromWavenumber(kPos, 54321u) != SeedFromWavenumber(kNeg, 54321u),
           "SeedFromWavenumber: mirrored wavenumbers get distinct seeds");
    demand(SeedFromWavenumber(kPos, 54321u) != SeedFromWavenumber(kPos, 54322u),
           "SeedFromWavenumber: base seed participates");
  }

  // --- 2. Draw-sequence determinism ---------------------------------------
  {
    Parameters<float> params;
    const Imath::V2f k(0.35f, -0.85f);

    NormalRandom<float> a(params), b(params);
    a.seed(k);
    b.seed(k);
    bool equalDraws = true;
    for (int i = 0; i < 64; ++i)
      if (a.nextAmp() != b.nextAmp() || a.nextPhase() != b.nextPhase())
        equalDraws = false;
    demand(equalDraws, "NormalRandom: identical seeds give identical draws");

    Parameters<float> other = params;
    other.random.seed = params.random.seed + 1;
    NormalRandom<float> c(other);
    c.seed(k);
    a.seed(k);
    bool anyDifferent = false;
    for (int i = 0; i < 64; ++i)
      if (a.nextAmp() != c.nextAmp()) anyDifferent = true;
    demand(anyDifferent, "NormalRandom: different base seeds diverge");

    LogNormalRandom<float> la(params), lb(params);
    la.seed(k);
    lb.seed(k);
    bool logEqual = true, logPositive = true, phaseInRange = true;
    for (int i = 0; i < 64; ++i)
    {
      const float ampA = la.nextAmp();
      const float phase = la.nextPhase();
      if (ampA != lb.nextAmp() || phase != lb.nextPhase()) logEqual = false;
      if (!(ampA > 0.0f) || !std::isfinite(ampA)) logPositive = false;
      if (phase < 0.0f || phase >= TAU<float>) phaseInRange = false;
    }
    demand(logEqual, "LogNormalRandom: identical seeds give identical draws");
    demand(logPositive, "LogNormalRandom: amplitudes are positive and finite");
    demand(phaseInRange, "phases lie in [0, tau)");
  }

  // --- 3. InitialState reproducibility ------------------------------------
  auto sameOcean = [](const InitialState<float> &x,
                      const InitialState<float> &y) {
    const std::size_t bytes =
        x.HSpectralPos.size() * sizeof(std::complex<float>);
    return std::memcmp(x.HSpectralPos.cdata(), y.HSpectralPos.cdata(),
                       bytes) == 0 &&
           std::memcmp(x.HSpectralNeg.cdata(), y.HSpectralNeg.cdata(),
                       bytes) == 0 &&
           std::memcmp(x.Omega.cdata(), y.Omega.cdata(),
                       x.Omega.size() * sizeof(float)) == 0;
  };

  {
    Parameters<float> params;
    params.resolutionPowerOfTwo = 6;   // 64x64 keeps this fast

    const InitialState<float> first(params);
    const InitialState<float> second(params);
    demand(sameOcean(first, second),
           "InitialState (NormalRandom): same Parameters, identical ocean");

    Parameters<float> reseeded = params;
    reseeded.random.seed = params.random.seed + 1;
    const InitialState<float> third(reseeded);
    demand(!sameOcean(first, third),
           "InitialState: a different seed changes the ocean");

    Parameters<float> logParams = params;
    logParams.random.type = kLogNormalRandom;
    const InitialState<float> logFirst(logParams);
    const InitialState<float> logSecond(logParams);
    demand(sameOcean(logFirst, logSecond),
           "InitialState (LogNormalRandom): same Parameters, identical ocean");
    demand(!sameOcean(first, logFirst),
           "InitialState: the random type changes the ocean");
  }

  std::cout << "\nresult: " << (ok ? "PASS" : "FAIL") << "\n";
  return ok ? 0 : 1;
}

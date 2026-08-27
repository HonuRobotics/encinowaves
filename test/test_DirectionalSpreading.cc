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
 * Property tests for the directional spreading functions. A spreading
 * function D(omega, theta) is a probability density over direction, so for
 * every spreading model, wave frequency, and swell setting:
 *
 *   1. D must be finite and non-negative for all theta in [-pi, pi].
 *      This is the property the upstream Donelan-Banner negative-swell
 *      branch violated by blending toward -1/(2pi) instead of +1/(2pi).
 *   2. At swell = -1 the Donelan-Banner, Mitsuyasu, and Hasselmann models
 *      blend fully to the isotropic distribution, so D == 1/(2pi) exactly,
 *      independent of theta and omega.
 *   3. Where the model is normalized over the full circle, the integral of
 *      D over [-pi, pi] must be ~1: Mitsuyasu and Hasselmann always
 *      (closed-form normalization), Donelan-Banner for swell < 0
 *      (a blend of two full-circle-normalized densities), and
 *      PosCosSquared (its kernel is zero outside [-pi/2, pi/2], the range
 *      its numerical normalization integrates over). Donelan-Banner with
 *      swell >= 0 normalizes over [-pi/2, pi/2] only, so its full-circle
 *      integral legitimately exceeds 1 and is not checked here.
 *
 * Run in double precision: the float tgamma overflow for large spreading
 * exponents is a separate known issue and not what this test targets.
 */

#include "ehukai/DirectionalSpreading.h"
#include "ehukai/Parameters.h"

#include <cmath>
#include <iostream>
#include <string>
#include <vector>

namespace
{
  constexpr double kPi = 3.14159265358979323846;

  // Trapezoidal integral of D(omega, theta) over theta in [-pi, pi].
  template <typename SPREADING>
  double IntegrateOverTheta(const SPREADING &D, double omega, int n = 4001)
  {
    double sum = 0.0;
    const double h = (2.0 * kPi) / (n - 1);
    for (int i = 0; i < n; ++i)
    {
      const double theta = -kPi + i * h;
      const double w = (i == 0 || i == n - 1) ? 0.5 : 1.0;
      sum += w * D(omega, theta, /*kMag=*/1.0, /*dTheta=*/h);
    }
    return sum * h;
  }
}  // namespace

int main()
{
  std::cout << "Directional spreading property tests\n\n";

  bool ok = true;
  auto demand = [&](bool cond, const std::string &msg) {
    std::cout << "  " << (cond ? "[ok]   " : "[FAIL] ") << msg << "\n";
    if (!cond) ok = false;
  };

  // The modal angular frequency the spreading models share with the JONSWAP
  // spectrum: omega_m = TAU 3.5 (g/U) (g F / U^2)^-0.33 with fetch in METERS,
  // while the function takes the Parameters convention of kilometers. This
  // pins the km-to-m conversion; the upstream code fed kilometers straight
  // into the dimensionless fetch, placing the spreading's modal frequency
  // ~9.8x above the spectrum's peak.
  {
    const double g = 9.81, U = 17.0, fetchKm = 300.0;
    const double chi = g * (fetchKm * 1000.0) / (U * U);
    const double expected =
        2.0 * kPi * 3.5 * (g / U) * std::pow(chi, -0.33);
    const double actual =
        ehukai::modalAngularFrequencyJONSWAP(g, U, fetchKm);
    demand(std::abs(actual - expected) < 1e-9 * expected,
           "modalAngularFrequencyJONSWAP treats fetch as km (SI internally)");
    // Sanity: ~0.6 rad/s is a ~10 s peak period, a realistic sea state for
    // 17 m/s wind and 300 km fetch. The unconverted version gave ~5.9 rad/s.
    demand(actual > 0.3 && actual < 1.2,
           "modal angular frequency is physically plausible");
  }
  std::cout << "\n";

  const std::vector<double> omegas = {0.5, 1.0, 2.0, 4.0, 6.0};
  const std::vector<double> swells = {-1.0, -0.6, -0.2, 0.0, 0.4, 1.0};
  constexpr int kThetaSamples = 721;
  const double isotropic = 1.0 / (2.0 * kPi);

  // Runs properties 1-3 for one spreading model across the sample grid.
  // `checkIntegral` decides per-swell whether the full-circle integral
  // must be ~1 for this model; `hasIsotropicBlend` says whether swell = -1
  // must collapse to the uniform distribution.
  auto testSpreading = [&](const std::string &name, auto makeSpreading,
                           auto checkIntegral, bool hasIsotropicBlend) {
    std::cout << name << ":\n";

    bool allNonNegative = true;
    bool allFinite = true;
    bool isotropicAtFullNegativeSwell = true;
    bool integralsUnit = true;
    bool pairMatchesSingle = true;

    for (double swellAmount : swells)
    {
      ehukai::Parameters<double> params;
      params.directionalSpreading.swell = swellAmount;
      const auto D = makeSpreading(params);

      for (double omega : omegas)
      {
        for (int i = 0; i < kThetaSamples; ++i)
        {
          const double theta = -kPi + (2.0 * kPi * i) / (kThetaSamples - 1);
          const double d = D(omega, theta, 1.0, 0.01);
          if (!std::isfinite(d)) allFinite = false;
          if (d < 0.0) allNonNegative = false;
          if (hasIsotropicBlend && swellAmount == -1.0 &&
              std::abs(d - isotropic) > 1e-9)
            isotropicAtFullNegativeSwell = false;

          // The two-direction overload must agree with two single
          // evaluations (guards against swapped or asymmetric outputs).
          if (i % 60 == 0)
          {
            double pa = 0.0, pb = 0.0;
            D(omega, theta, -theta, 1.0, 0.01, pa, pb);
            if (std::abs(pa - d) > 1e-12 ||
                std::abs(pb - D(omega, -theta, 1.0, 0.01)) > 1e-12)
              pairMatchesSingle = false;
          }
        }

        if (checkIntegral(swellAmount))
        {
          const double integral = IntegrateOverTheta(D, omega);
          if (std::abs(integral - 1.0) > 0.02)
          {
            std::cout << "    (integral = " << integral << " at omega = "
                      << omega << ", swell = " << swellAmount << ")\n";
            integralsUnit = false;
          }
        }
      }
    }

    demand(allFinite, name + ": finite for all (omega, theta, swell)");
    demand(allNonNegative, name + ": non-negative everywhere");
    if (hasIsotropicBlend)
      demand(isotropicAtFullNegativeSwell,
             name + ": swell = -1 gives the isotropic 1/(2pi)");
    demand(integralsUnit, name + ": normalized integrals are ~1");
    demand(pairMatchesSingle,
           name + ": pair overload agrees with single evaluations");
  };

  testSpreading(
      "DonelanBanner",
      [](const ehukai::Parameters<double> &p) {
        return ehukai::DonelanBannerDirectionalSpreading<double>(p);
      },
      // Full-circle normalization only holds on the negative-swell branch.
      [](double swell) { return swell < 0.0; },
      /*hasIsotropicBlend=*/true);

  testSpreading(
      "Mitsuyasu",
      [](const ehukai::Parameters<double> &p) {
        return ehukai::MitsuyasuDirectionalSpreading<double>(p);
      },
      [](double) { return true; },
      /*hasIsotropicBlend=*/true);

  testSpreading(
      "Hasselmann",
      [](const ehukai::Parameters<double> &p) {
        return ehukai::HasselmannDirectionalSpreading<double>(p);
      },
      [](double) { return true; },
      /*hasIsotropicBlend=*/true);

  testSpreading(
      "PosCosSquared",
      [](const ehukai::Parameters<double> &p) {
        return ehukai::PosCosSquaredDirectionalSpreading<double>(p);
      },
      // No isotropic blend branch; the kernel is zero outside
      // [-pi/2, pi/2], so the full-circle integral is ~1 for any swell.
      [](double) { return true; },
      /*hasIsotropicBlend=*/false);

  std::cout << "\nresult: " << (ok ? "PASS" : "FAIL") << "\n";
  return ok ? 0 : 1;
}

//-*****************************************************************************
// Copyright 2015 Christopher Jon Horvath
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//-*****************************************************************************

//-*****************************************************************************
// The basic architecture of these Waves is based on the TweakWaves application
// written by Chris Horvath for Tweak Films in 2001.  This, in turn, was based
// on the SIGGRAPH papers and courses by Jerry Tessendorf, and by the paper
// "A Simple Fluid Solver based on the FTT" by Jos Stam.
//
// The TMA, JONSWAP, and Pierson Moskowitz Wave Spectra, as well as the
// directional spreading functions are formulated based on the descriptions
// given in "Ocean Waves: The Stochastic Approach",
// by Michel K. Ochi, published by Cambridge Ocean Technology Series, 1998,2005.
//
// This library is written as a working implementation of the paper:
// Christopher J. Horvath. 2015.
// Empirical directional wave spectra for computer graphics.
// In Proceedings of the 2015 Symposium on Digital Production (DigiPro '15),
// Los Angeles, Aug. 8, 2015, pp. 29-39.
//-*****************************************************************************

#ifndef EHUKAI_DIRECTIONALSPREADING_H
#define EHUKAI_DIRECTIONALSPREADING_H

#include "Foundation.h"
#include "Basics.h"
#include "Parameters.h"

namespace ehukai {

//------------------------------------------------------------------------------
template <typename T, typename FUNC>
T numericallyIntegrate(FUNC f, T a, T b, int n) {
  T nf  = static_cast<T>(n);
  T sum = 0;
  for (int k = 1; k < n; ++k) {
    sum += f(a + (k * (b - a) / nf));
  }
  return ((b - a) / nf) * ((f(a) / 2) + (f(b) / 2) + sum);
}

//------------------------------------------------------------------------------
template <typename T>
T swellShape(T omega, T modal_omega, T swell_amount) {
  return 16.1 * std::tanh(modal_omega / omega) * sqr(swell_amount);
}

//------------------------------------------------------------------------------
template <typename T>
T swell(T theta, T omega, T modal_omega, T swell_amount) {
  T shape = swellShape(omega, modal_omega, swell_amount);
  return std::pow(std::abs(std::cos(theta / 2.0)), 2.0 * shape);
}

//------------------------------------------------------------------------------
template <typename T, typename FUNCA, typename FUNCB>
T normalizedSwellDirectionalProduct(T theta, FUNCA A, FUNCB B) {
  auto product = [A, B](T x) -> T { return A(x) * B(x); };

  T denom = numericallyIntegrate(product, -PI<T> / 2, PI<T> / 2, 36);
  return product(theta) / denom;
}

//------------------------------------------------------------------------------
// JONSWAP peak angular frequency: omega_m = TAU 3.5 (g/U) chi^-0.33 with the
// dimensionless fetch chi = g F / U^2, F in METERS. The fetch argument is in
// kilometers (the Parameters convention) and converted here, matching the
// km-to-m conversion JONSWAPSpectrum::init applies before computing the same
// quantity; without it the spreading's modal frequency sat ~9.8x above the
// spectrum's peak.
template <typename T>
T modalAngularFrequencyJONSWAP(T gravity, T meanWindSpeed, T fetchKm) {
  T fetchM             = fetchKm * T(1000.0);
  T dimensionlessFetch = gravity * fetchM / sqr(meanWindSpeed);
  return TAU<T> * 3.5 * (gravity / meanWindSpeed) *
         std::pow(dimensionlessFetch, -0.33);
}

//------------------------------------------------------------------------------
template <typename T>
class DonelanBannerDirectionalSpreading {
public:
  explicit DonelanBannerDirectionalSpreading(const Parameters<T>& params)
      : m_modalAngularFrequency(modalAngularFrequencyJONSWAP(
          params.gravity, params.windSpeed, params.fetch))
      , m_swell(params.directionalSpreading.swell) {}

  T operator()(T i_omega, T i_theta, T i_kMag, T i_dTheta) const {
    T dA, dB;
    (*this)(i_omega, i_theta, i_theta, i_kMag, i_dTheta, dA, dB);
    return dA;
  }

  // Evaluate at two directions sharing one omega. The normalization is
  // independent of theta, so computing it once here (instead of once per
  // direction) halves the dominant per-point cost: for this model a
  // 36-sample numerical integration when swell > 0.
  void operator()(T i_omega, T i_thetaA, T i_thetaB, T /*i_kMag*/,
                  T /*i_dTheta*/, T& o_dA, T& o_dB) const {
    T omega_over_modal_omega = i_omega / m_modalAngularFrequency;
    T beta_s;
    if (omega_over_modal_omega < 0.95) {
      beta_s = 2.61 * std::pow(omega_over_modal_omega, 1.3);
    } else if (omega_over_modal_omega < 1.6) {
      beta_s = 2.28 * std::pow(omega_over_modal_omega, -1.3);
    } else {
      T expo =
        -0.4 +
        0.8393 * std::exp(-0.567 * std::log(sqr(omega_over_modal_omega)));
      beta_s = std::pow(10, expo);
    }

    // Make a hyperbolic secant function
    auto sech = [](T x) { return 1.0 / std::cosh(x); };
    auto B    = [beta_s, sech](T x) -> T { return sqr(sech(beta_s * x)); };

    if (m_swell > 0.0) {
      // We need to do a numerical integration to determine the
      // normalization factor for the product of the original function (B)
      // with the swell elongation (A).
      auto A = [this, i_omega](T x) -> T {
        return swell(x, i_omega, m_modalAngularFrequency, m_swell);
      };
      auto product = [A, B](T x) -> T { return A(x) * B(x); };
      T denom = numericallyIntegrate(product, -PI<T> / 2, PI<T> / 2, 36);
      o_dA = product(i_thetaA) / denom;
      o_dB = product(i_thetaB) / denom;
    } else if (m_swell == 0.0) {
      // The swell elongation is identically 1 (cos^0), so the
      // normalization over [-pi/2, pi/2] has the closed form
      // int sech^2(beta x) dx = 2 tanh(beta pi / 2) / beta and the
      // numerical integration can be skipped entirely.
      T denom = T(2.0) * std::tanh(beta_s * PI<T> / 2) / beta_s;
      o_dA = B(i_thetaA) / denom;
      o_dB = B(i_thetaB) / denom;
    } else {
      T integral =
        (std::tanh(beta_s * PI<T>) - std::tanh(-beta_s * PI<T>)) / beta_s;
      // Negative swell blends toward the isotropic distribution 1/(2pi),
      // exactly as the Mitsuyasu and Hasselmann spreadings do below. The
      // upstream code lerped toward -1/(2pi), which drives the spreading
      // (and with it the spectral energy) negative; the sign error was
      // masked downstream by the abs() in the amplitude computation.
      const T blend = Imath::clamp(-m_swell, T(0), T(1));
      o_dA = Imath::lerp(B(i_thetaA) / integral, T(1) / TAU<T>, blend);
      o_dB = Imath::lerp(B(i_thetaB) / integral, T(1) / TAU<T>, blend);
    }
  }

protected:
  T m_modalAngularFrequency;
  T m_swell;
};

//------------------------------------------------------------------------------
// Shared machinery for the cos^(2*shape)(theta/2) spreadings: Mitsuyasu and
// Hasselmann differ only in their base shape formula, provided by the derived
// class; the swell bias and the normalized evaluation live here.
template <typename T>
class CosPowerDirectionalSpreadingBase {
protected:
  explicit CosPowerDirectionalSpreadingBase(const Parameters<T>& params)
      : m_modalAngularFrequency(modalAngularFrequencyJONSWAP(
          params.gravity, params.windSpeed, params.fetch))
      , m_modalShape(11.5 * std::pow(m_modalAngularFrequency *
                                       params.windSpeed / params.gravity,
                                     -2.5))
      , m_modalCelerity(params.gravity / m_modalAngularFrequency)
      , m_windSpeedOverCelerity(params.windSpeed / m_modalCelerity)
      , m_swell(params.directionalSpreading.swell) {}

  // Adds the swell bias to the model's base shape, then evaluates the
  // normalized distribution at two directions. The shape and the
  // tgamma-based normalization depend only on omega and are computed once.
  void evaluatePair(T i_omega, T i_baseShape, T i_thetaA, T i_thetaB,
                    T& o_dA, T& o_dB) const {
    T shape = i_baseShape;
    if (m_swell >= 0.0) {
      shape += swellShape(i_omega, m_modalAngularFrequency, m_swell);
    }

    // The double literals are load-bearing: they promote these expressions
    // to double even for T = float, and tgamma overflows single precision
    // (NaN normalization) once large swell pushes 2*shape past ~34. Do not
    // "clean up" the literals to T(...).
    T factor_A = std::pow(2.0, (2.0 * shape) - 1.0) / PI<T>;
    T factor_B =
      sqr(std::tgamma(shape + 1.0)) / std::tgamma((2.0 * shape) + 1.0);
    auto evalAt = [this, factor_A, factor_B, shape](T theta) -> T {
      T factor_C = std::pow(std::abs(std::cos(theta / 2.0)), 2.0 * shape);
      if (m_swell < 0) {
        return Imath::lerp(factor_A * factor_B * factor_C, T(1) / T(TAU<T>),
                           Imath::clamp(-m_swell, T(0), T(1)));
      } else {
        return factor_A * factor_B * factor_C;
      }
    };
    o_dA = evalAt(i_thetaA);
    o_dB = evalAt(i_thetaB);
  }

  T m_modalAngularFrequency;
  T m_modalShape;
  T m_modalCelerity;
  T m_windSpeedOverCelerity;
  T m_swell;
};

//------------------------------------------------------------------------------
template <typename T>
class MitsuyasuDirectionalSpreading
  : public CosPowerDirectionalSpreadingBase<T> {
public:
  typedef CosPowerDirectionalSpreadingBase<T> super_type;

  explicit MitsuyasuDirectionalSpreading(const Parameters<T>& params)
      : super_type(params) {}

  T operator()(T i_omega, T i_theta, T i_kMag, T i_dTheta) const {
    T dA, dB;
    (*this)(i_omega, i_theta, i_theta, i_kMag, i_dTheta, dA, dB);
    return dA;
  }

  // Evaluate at two directions sharing one omega.
  void operator()(T i_omega, T i_thetaA, T i_thetaB, T /*i_kMag*/,
                  T /*i_dTheta*/, T& o_dA, T& o_dB) const {
    T shape_exp = i_omega <= this->m_modalAngularFrequency ? 5.0 : -2.5;
    T baseShape =
      this->m_modalShape *
      std::pow(i_omega / this->m_modalAngularFrequency, shape_exp);
    this->evaluatePair(i_omega, baseShape, i_thetaA, i_thetaB, o_dA, o_dB);
  }
};

//------------------------------------------------------------------------------
template <typename T>
class HasselmannDirectionalSpreading
  : public CosPowerDirectionalSpreadingBase<T> {
public:
  typedef CosPowerDirectionalSpreadingBase<T> super_type;

  explicit HasselmannDirectionalSpreading(const Parameters<T>& params)
      : super_type(params) {}

  T operator()(T i_omega, T i_theta, T i_kMag, T i_dTheta) const {
    T dA, dB;
    (*this)(i_omega, i_theta, i_theta, i_kMag, i_dTheta, dA, dB);
    return dA;
  }

  // Evaluate at two directions sharing one omega.
  void operator()(T i_omega, T i_thetaA, T i_thetaB, T /*i_kMag*/,
                  T /*i_dTheta*/, T& o_dA, T& o_dB) const {
    T baseShape;
    if (i_omega > this->m_modalAngularFrequency) {
      baseShape =
        9.77 * std::pow(i_omega / this->m_modalAngularFrequency,
                        -2.33 - (1.45 * (this->m_windSpeedOverCelerity -
                                         1.17)));
    } else {
      baseShape =
        6.97 * std::pow(i_omega / this->m_modalAngularFrequency, 4.06);
    }
    this->evaluatePair(i_omega, baseShape, i_thetaA, i_thetaB, o_dA, o_dB);
  }
};

//------------------------------------------------------------------------------
template <typename T>
class PosCosSquaredDirectionalSpreading {
public:
  explicit PosCosSquaredDirectionalSpreading(const Parameters<T>& params)
      : m_modalAngularFrequency(modalAngularFrequencyJONSWAP(
          params.gravity, params.windSpeed, params.fetch))
      , m_swell(params.directionalSpreading.swell) {}

  T operator()(T i_omega, T i_theta, T i_kMag, T i_dTheta) const {
    T dA, dB;
    (*this)(i_omega, i_theta, i_theta, i_kMag, i_dTheta, dA, dB);
    return dA;
  }

  // Evaluate at two directions sharing one omega. The normalization is
  // independent of theta, so the 36-sample numerical integration (or its
  // closed form at swell == 0) runs once instead of once per direction.
  void operator()(T i_omega, T i_thetaA, T i_thetaB, T /*i_kMag*/,
                  T /*i_dTheta*/, T& o_dA, T& o_dB) const {
    auto B = [](T x) -> T {
      if (x < -PI_2<T> || x > PI_2<T>) {
        return T{0};
      } else {
        return sqr(std::cos(x));
      }
    };

    if (m_swell == 0.0) {
      // The swell elongation is identically 1 (cos^0) and
      // int cos^2 over [-pi/2, pi/2] is exactly pi/2, so the numerical
      // integration can be skipped entirely.
      o_dA = B(i_thetaA) / PI_2<T>;
      o_dB = B(i_thetaB) / PI_2<T>;
    } else {
      auto A = [this, i_omega](T x) -> T {
        return swell(x, i_omega, m_modalAngularFrequency, m_swell);
      };
      auto product = [A, B](T x) -> T { return A(x) * B(x); };
      T denom = numericallyIntegrate(product, -PI<T> / 2, PI<T> / 2, 36);
      o_dA = product(i_thetaA) / denom;
      o_dB = product(i_thetaB) / denom;
    }
  }

protected:
  T m_modalAngularFrequency;
  T m_swell;
};

}  // namespace ehukai

#endif

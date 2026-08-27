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

#ifndef EHUKAI_PARAMETERS_H
#define EHUKAI_PARAMETERS_H

#include "Foundation.h"
#include "Basics.h"

namespace ehukai {

//! Dispersion relationship linking wave number to angular frequency.
//! Capillary generalizes finite-depth, which generalizes deep water, so
//! kCapillaryDispersion is a safe default for all conditions.
enum DispersionType {
  kDeepDispersion,
  kFiniteDepthDispersion,
  kCapillaryDispersion
};

//! Non-directional wave energy spectrum. TMA extends JONSWAP with a
//! depth attenuation; JONSWAP extends Pierson-Moskowitz with fetch.
enum SpectrumType {
  kPiersonMoskowitzSpectrum,
  kJONSWAPSpectrum,
  kTMASpectrum,

  kNumSpectrumTypes
};

//! Directional spreading model distributing spectral energy over
//! direction relative to the wind.
enum DirectionalSpreadingType {
  kPosCosThetaSqrDirectionalSpreading,
  kMitsuyasuDirectionalSpreading,
  kHasselmannDirectionalSpreading,
  kDonelanBannerDirectionalSpreading,
};

//! Optional wavelength band-pass applied to the initial amplitudes.
enum FilterType {
  kNullFilter,
  kSmoothInvertibleBandPassFilter,
};

//! Distribution of the random amplitude draws.
enum RandomType {
  kNormalRandom,
  kLogNormalRandom,
};

//! All simulation inputs. Everything a wave field needs is gathered here;
//! pass one Parameters to InitialState, Propagation, and ComputeNormals.
//! Defaults describe a 100 m deep-ocean tile under a 17 m/s wind.
template <typename T>
struct Parameters {
  //! Grid resolution exponent: the simulation runs on a
  //! 2^resolutionPowerOfTwo square grid (clamped to [1, 2^30]).
  int resolutionPowerOfTwo;

  //! World-space size of the (square, tiling) simulated patch, in meters.
  T domain;

  //! Gravitational acceleration, in meters per second squared.
  T gravity;
  //! Surface tension, in Newtons per meter (capillary dispersion only).
  T surfaceTension;
  //! Water density, in kilograms per cubic meter (capillary dispersion).
  T density;
  //! Water depth, in meters (finite-depth/capillary dispersion and TMA).
  T depth;

  //! Wind speed, in meters per second. Wind is assumed to blow along the
  //! positive X axis; rotate the resulting fields externally if needed.
  T windSpeed;
  //! Fetch (distance over which the wind has blown), in KILOMETERS.
  T fetch;

  //! Lateral displacement (chop) gain applied to Dx/Dy.
  T pinch;
  //! Vertical displacement gain applied to the height field.
  T amplitudeGain;

  //! Strength of the trough attenuation pass in Propagation, in [0, 1];
  //! 0 disables it (and skips its extra FFTs).
  T troughDamping;
  //! Small-wavelength edge of the trough-damping band, in meters.
  T troughDampingSmallWavelength;
  //! Big-wavelength edge of the trough-damping band, in meters.
  T troughDampingBigWavelength;
  //! Soft transition width added around the damping band, in meters.
  T troughDampingSoftWidth;

  //! Dispersion relationship selection.
  struct Dispersion {
    DispersionType type;
    Dispersion()
      : type(kCapillaryDispersion) {}
  } dispersion;

  //! Non-directional spectrum selection.
  struct Spectrum {
    SpectrumType type;
    Spectrum()
      : type(kTMASpectrum) {}
  } spectrum;

  //! Directional spreading selection. swell in [0, 1] elongates crests
  //! toward the wind direction; negative values (down to -1) blend toward
  //! an isotropic, directionless sea.
  struct DirectionalSpreading {
    DirectionalSpreadingType type;
    T swell;
    DirectionalSpreading()
      : type(kHasselmannDirectionalSpreading)
      , swell(0.0) {}
  } directionalSpreading;

  //! Wavelength band-pass filter over the initial amplitudes. Wavelengths
  //! between smallWavelength and bigWavelength (in meters, with softWidth
  //! easing at the edges) are kept; the rest attenuate to min. invert
  //! flips the band.
  struct Filter {
    FilterType type;
    T softWidth;
    T smallWavelength;
    T bigWavelength;
    T min;
    bool invert;
    Filter()
      : type(kNullFilter)
      , softWidth(0.0)
      , smallWavelength(0.0)
      , bigWavelength(1000000.0)
      , min(0.0)
      , invert(false) {}
  } filter;

  //! Random distribution selection and seed. The same seed reproduces the
  //! same ocean on the same platform.
  struct Random {
    RandomType type;
    int seed;
    Random()
      : type(kNormalRandom)
      , seed(54321) {}
  } random;

  Parameters()
    : resolutionPowerOfTwo(9)
    , domain(100.0)
    , gravity(9.81)
    , surfaceTension(0.074)
    , density(1000.0)
    , depth(100.0)
    , windSpeed(17.0)
    , fetch(300.0)
    , pinch(0.75)
    , amplitudeGain(1.0)
    , troughDamping(0.0)
    , troughDampingSmallWavelength(1.0)
    , troughDampingBigWavelength(4.0)
    , troughDampingSoftWidth(2.0) {}

  //! Grid resolution (cells per side). Routed through PowerOfTwo so
  //! out-of-range exponents clamp to [1, 2^30] exactly like the field
  //! allocations do, instead of shifting into UB.
  int resolution() const { return PowerOfTwo(resolutionPowerOfTwo); }
};

//-*****************************************************************************
typedef Parameters<float> Parametersf;
typedef Parameters<double> Parametersd;

}  // namespace ehukai

#endif

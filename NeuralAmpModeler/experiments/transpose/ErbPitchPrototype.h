#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace dessmetal::experiments::transpose
{
// Evaluation implementation of the ERB-style analytic filter-bank approach used
// by the MIT-licensed Terrarium Poly Pitch project. The coefficient topology,
// phase-wrap strategy, and default 83-band spacing are derived from:
//   https://github.com/schult/terrarium-poly-octave/tree/arbitrary-shift
// Copyright (c) 2024 Steven Schulteis, used under the MIT License.
//
// This is deliberately isolated from the plug-in until the objective and musical
// quality gates pass. The desktop modes let the benchmark distinguish algorithm
// limitations from approximations intended for an embedded processor.
class ErbPitchPrototype
{
public:
  enum class MathMode
  {
    kEmbeddedApproximation,
    kBalancedApproximation,
    kAccurate
  };

  static constexpr int kMinSemitones = -12;
  static constexpr int kMaxSemitones = 12;
  static constexpr int kDefaultBandCount = 83;

  bool Reset(const double sampleRate, const int maxBlockSize, const MathMode mathMode,
             const int bandCount = kDefaultBandCount, const double density = 19.0,
             const double bandwidthScale = 1.0, const double lowBandwidthScale = 0.0,
             const double bandwidthTransitionHz = 300.0, const bool compensateFilterPhase = false,
             const int dominanceExponent = 0)
  {
    mConfigured = false;
    if (!std::isfinite(sampleRate) || sampleRate < 8000.0 || maxBlockSize <= 0 || bandCount <= 0
        || !std::isfinite(density) || density < 4.0 || !std::isfinite(bandwidthScale)
        || bandwidthScale <= 0.0 || !std::isfinite(lowBandwidthScale)
        || !std::isfinite(bandwidthTransitionHz) || bandwidthTransitionHz <= 0.0
        || dominanceExponent < 0 || dominanceExponent > 8)
      return false;

    mSampleRate = sampleRate;
    mMathMode = mathMode;
    mBands.clear();
    mBands.reserve(static_cast<std::size_t>(bandCount));
    for (int index = 0; index < bandCount; ++index)
    {
      const double center = CenterFrequency(index, density);
      if (center >= sampleRate * 0.475)
        break;
      const double lowScale = lowBandwidthScale > 0.0 ? lowBandwidthScale : bandwidthScale;
      const double normalizedCenter = center / bandwidthTransitionHz;
      const double lowWeight = 1.0 / (1.0 + normalizedCenter * normalizedCenter
                                           * normalizedCenter * normalizedCenter);
      const double shapedBandwidthScale = bandwidthScale + (lowScale - bandwidthScale) * lowWeight;
      mBands.emplace_back(center, sampleRate, Bandwidth(index, density) * shapedBandwidthScale, mathMode,
                          compensateFilterPhase);
    }
    if (mBands.empty())
      return false;

    mBandOutputs.assign(mBands.size(), 0.0f);
    mBandMagnitudePowers.assign(mBands.size(), 0.0f);
    mDominanceExponent = dominanceExponent;

    ApplySemitones(0);
    mConfigured = true;
    return true;
  }

  void Process(const double* input, double* output, const int numFrames, const int semitones)
  {
    if (output == nullptr || numFrames <= 0)
      return;
    if (!mConfigured || input == nullptr)
    {
      std::fill(output, output + numFrames, 0.0);
      return;
    }

    const int clampedSemitones = std::clamp(semitones, kMinSemitones, kMaxSemitones);
    if (clampedSemitones != mAppliedSemitones)
      ApplySemitones(clampedSemitones);

    for (int frame = 0; frame < numFrames; ++frame)
    {
      const float sample = static_cast<float>(input[frame]);
      float shifted = 0.0f;
      if (mDominanceExponent == 0)
      {
        for (auto& band : mBands)
          shifted += band.Process(sample);
      }
      else
      {
        for (std::size_t index = 0; index < mBands.size(); ++index)
        {
          mBandOutputs[index] = mBands[index].Process(sample);
          float power = mBands[index].GetMagnitude();
          for (int exponent = 1; exponent < mDominanceExponent; ++exponent)
            power *= mBands[index].GetMagnitude();
          mBandMagnitudePowers[index] = power;
        }
        for (std::size_t index = 0; index < mBands.size(); ++index)
        {
          double neighborhoodPower = mBandMagnitudePowers[index];
          if (index > 0)
            neighborhoodPower += mBandMagnitudePowers[index - 1];
          if (index + 1 < mBands.size())
            neighborhoodPower += mBandMagnitudePowers[index + 1];
          if (neighborhoodPower > std::numeric_limits<float>::min())
            shifted += mBandOutputs[index]
                       * static_cast<float>(mBandMagnitudePowers[index] / neighborhoodPower);
        }
      }
      output[frame] = static_cast<double>(shifted);
    }
  }

  bool IsConfigured() const { return mConfigured; }
  int GetLatencySamples() const { return 0; }
  int GetBandCount() const { return static_cast<int>(mBands.size()); }
  int GetAppliedSemitones() const { return mAppliedSemitones; }

  static double CenterFrequency(const int index)
  {
    return CenterFrequency(index, 19.0);
  }

  static double CenterFrequency(const int index, const double density)
  {
    return 480.0 * std::pow(2.0, static_cast<double>(index) / density) - 420.0;
  }

  static double Bandwidth(const int index)
  {
    return Bandwidth(index, 19.0);
  }

  static double Bandwidth(const int index, const double density)
  {
    const double lower = CenterFrequency(index - 1, density);
    const double center = CenterFrequency(index, density);
    const double upper = CenterFrequency(index + 1, density);
    const double upperDistance = upper - center;
    const double lowerDistance = center - lower;
    return 2.0 * upperDistance * lowerDistance / (upperDistance + lowerDistance);
  }

private:
  static constexpr float kPi = 3.14159265358979323846f;
  static constexpr float kTwoPi = 2.0f * kPi;

  static float EmbeddedInverseSqrt(const float value)
  {
    if (!(value > 0.0f))
      return 0.0f;
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    bits = 0x5F1FFFF9u - (bits >> 1u);
    float estimate = 0.0f;
    std::memcpy(&estimate, &bits, sizeof(estimate));
    return estimate * (0.703952253f * (2.38924456f - value * estimate * estimate));
  }

  static float EmbeddedAtan2Turns(const float y, const float x)
  {
    constexpr float halfTurn = 0.5f;
    constexpr float quarterTurn = 0.25f;
    const float absY = std::abs(y) + std::numeric_limits<float>::epsilon();
    const float absX = std::abs(x);
    const float ratio = (x - std::copysign(absY, x)) / (absY + absX);
    const float turns = halfTurn - std::copysign(quarterTurn, x) - quarterTurn * ratio;
    return std::copysign(turns, y);
  }

  static float EmbeddedSineTurns(const float turns)
  {
    const float z = 2.0f * (turns - std::floor(turns) - 0.5f);
    return 4.0f * z * (1.0f - std::abs(z));
  }

  // Low-cost atan approximation with substantially lower angular error than the
  // embedded linear approximation. Input and output are conventional radians.
  static float BalancedAtan2(const float y, const float x)
  {
    if (x == 0.0f)
      return y > 0.0f ? 0.5f * kPi : (y < 0.0f ? -0.5f * kPi : 0.0f);

    const float absX = std::abs(x);
    const float absY = std::abs(y);
    const bool invert = absY > absX;
    const float ratio = invert ? absX / std::max(absY, std::numeric_limits<float>::min())
                               : absY / std::max(absX, std::numeric_limits<float>::min());
    const float ratio2 = ratio * ratio;
    // Minimax-style odd polynomial on [0, 1].
    float angle = (((0.0208351f * ratio2 - 0.0851330f) * ratio2 + 0.1801410f) * ratio2
                   - 0.3302995f) * ratio2 * ratio + ratio;
    if (invert)
      angle = 0.5f * kPi - angle;
    if (x < 0.0f)
      angle = kPi - angle;
    return std::copysign(angle, y);
  }

  static float BalancedSine(const float radians)
  {
    float wrapped = radians - kTwoPi * std::floor((radians + kPi) / kTwoPi);
    constexpr float b = 4.0f / kPi;
    constexpr float c = -4.0f / (kPi * kPi);
    float estimate = b * wrapped + c * wrapped * std::abs(wrapped);
    constexpr float correction = 0.225f;
    estimate = correction * (estimate * std::abs(estimate) - estimate) + estimate;
    return estimate;
  }

  class Band
  {
  public:
    Band(const double center, const double sampleRate, const double bandwidth, const MathMode mathMode,
         const bool compensateFilterPhase)
      : mMathMode(mathMode), mCompensateFilterPhase(compensateFilterPhase),
        mCenterHz(static_cast<float>(center)), mBandwidthHz(static_cast<float>(bandwidth)),
        mSampleRate(static_cast<float>(sampleRate))
    {
      const std::complex<double> imaginary{0.0, 1.0};
      const double omega0 = static_cast<double>(kPi) * bandwidth / sampleRate;
      const double cosOmega0 = std::cos(omega0);
      const double sinOmega0 = std::sin(omega0);
      const double sqrtTwo = std::sqrt(2.0);
      const double a0 = 1.0 + sqrtTwo * sinOmega0 * 0.5;
      const double gain = (1.0 - cosOmega0) / (2.0 * a0);
      const double omega1 = static_cast<double>(kTwoPi) * center / sampleRate;
      const std::complex<double> rotation1 = std::exp(imaginary * omega1);
      const std::complex<double> rotation2 = std::exp(imaginary * omega1 * 2.0);

      mD0 = static_cast<float>(gain);
      mD1 = ToFloat(rotation1 * (2.0 * gain));
      mD2 = ToFloat(rotation2 * gain);
      mC1 = ToFloat(rotation1 * (-2.0 * cosOmega0 / a0));
      mC2 = ToFloat(rotation2 * ((1.0 - sqrtTwo * sinOmega0 * 0.5) / a0));
    }

    void SetScale(const float scale)
    {
      mScale = scale;
      mOffsetStepTurns = scale;
    }

    float Process(const float sample)
    {
      const bool previousImaginarySign = std::signbit(mY.imag());
      mY = mState2 + mD0 * sample;
      mState2 = mState1 + mD1 * sample - mC1 * mY;
      mState1 = mD2 * sample - mC2 * mY;

      if (mY.real() < 0.0f && std::signbit(mY.imag()) != previousImaginarySign)
      {
        mPhaseOffsetTurns += mOffsetStepTurns;
        mPhaseOffsetTurns -= std::floor(mPhaseOffsetTurns);
      }

      const float norm = std::norm(mY);
      if (!(norm > std::numeric_limits<float>::min()))
        return 0.0f;

      float magnitude = 0.0f;
      float phaseTurns = 0.0f;
      switch (mMathMode)
      {
        case MathMode::kEmbeddedApproximation:
          magnitude = norm * EmbeddedInverseSqrt(norm);
          phaseTurns = EmbeddedAtan2Turns(mY.imag(), mY.real());
          break;
        case MathMode::kBalancedApproximation:
          magnitude = std::sqrt(norm);
          phaseTurns = BalancedAtan2(mY.imag(), mY.real()) / kTwoPi;
          break;
        case MathMode::kAccurate:
          magnitude = std::sqrt(norm);
          phaseTurns = std::atan2(mY.imag(), mY.real()) / kTwoPi;
          break;
      }

      float responsePhaseTurns = 0.0f;
      if (mCompensateFilterPhase && mHasPreviousPhase)
      {
        float phaseDelta = phaseTurns - mPreviousPhaseTurns;
        phaseDelta -= std::round(phaseDelta);
        const float instantaneousHz = phaseDelta * mSampleRate;
        const float normalizedOffset = std::clamp(
          2.0f * (instantaneousHz - mCenterHz) / std::max(mBandwidthHz, 1.0e-6f), -4.0f, 4.0f);
        const float numerator = 1.41421356237f * normalizedOffset;
        const float denominator = 1.0f - normalizedOffset * normalizedOffset;
        responsePhaseTurns = -BalancedAtan2(numerator, denominator) / kTwoPi;
      }
      mPreviousPhaseTurns = phaseTurns;
      mHasPreviousPhase = true;
      mLastMagnitude = magnitude;

      const float outputTurns = mScale * (phaseTurns - responsePhaseTurns) + mPhaseOffsetTurns;
      if (mMathMode == MathMode::kEmbeddedApproximation)
        return magnitude * EmbeddedSineTurns(outputTurns);
      if (mMathMode == MathMode::kAccurate)
        return magnitude * std::sin(kTwoPi * outputTurns);
      return magnitude * BalancedSine(kTwoPi * outputTurns);
    }

    float GetMagnitude() const { return mLastMagnitude; }

  private:
    static std::complex<float> ToFloat(const std::complex<double> value)
    {
      return {static_cast<float>(value.real()), static_cast<float>(value.imag())};
    }

    MathMode mMathMode = MathMode::kBalancedApproximation;
    bool mCompensateFilterPhase = false;
    bool mHasPreviousPhase = false;
    float mCenterHz = 0.0f;
    float mBandwidthHz = 1.0f;
    float mSampleRate = 48000.0f;
    float mPreviousPhaseTurns = 0.0f;
    float mLastMagnitude = 0.0f;
    float mScale = 1.0f;
    float mOffsetStepTurns = 1.0f;
    float mPhaseOffsetTurns = 0.25f;
    float mD0 = 0.0f;
    std::complex<float> mD1{};
    std::complex<float> mD2{};
    std::complex<float> mC1{};
    std::complex<float> mC2{};
    std::complex<float> mState1{};
    std::complex<float> mState2{};
    std::complex<float> mY{};
  };

  void ApplySemitones(const int semitones)
  {
    mAppliedSemitones = std::clamp(semitones, kMinSemitones, kMaxSemitones);
    const float scale = static_cast<float>(std::pow(2.0, static_cast<double>(mAppliedSemitones) / 12.0));
    for (auto& band : mBands)
      band.SetScale(scale);
  }

  std::vector<Band> mBands;
  std::vector<float> mBandOutputs;
  std::vector<float> mBandMagnitudePowers;
  double mSampleRate = 48000.0;
  MathMode mMathMode = MathMode::kBalancedApproximation;
  int mAppliedSemitones = 0;
  int mDominanceExponent = 0;
  bool mConfigured = false;
};
} // namespace dessmetal::experiments::transpose

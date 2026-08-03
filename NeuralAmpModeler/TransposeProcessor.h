#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>
#include <vector>

namespace dessmetal::transpose
{
// Low-latency polyphonic pitch shifting for live guitar. The analytic
// filter-bank topology and ERB-like spacing are derived from Steven Schulteis'
// MIT-licensed Terrarium Poly Pitch implementation (Copyright 2024) and the
// ERB-PS research it cites. DessMetal adds a two-resolution transient/body
// path, phase-continuous ratio automation, and a click-free dry transition.
// See licenses/terrarium-poly-octave-MIT.txt.
class Processor
{
public:
  static constexpr int kMinSemitones = -12;
  static constexpr int kMaxSemitones = 12;
  static constexpr int kBandCount = 83;

  bool Reset(const double sampleRate, const int initialSemitones = 0)
  {
    mConfigured = false;
    if (!std::isfinite(sampleRate) || sampleRate < 8000.0)
      return false;

    const int clampedSemitones = ClampSemitones(initialSemitones);
    const float initialScale = SemitonesToScale(clampedSemitones);
    if (!mQuality.Configure(sampleRate, 0.5, initialScale)
        || !mFast.Configure(sampleRate, 2.0, initialScale))
      return false;

    mSampleRate = sampleRate;
    mTransitionSamples = std::max(1, static_cast<int>(std::lround(0.008 * sampleRate)));
    mFastAttackCoefficient = Coefficient(0.0005);
    mFastReleaseCoefficient = Coefficient(0.010);
    mSlowAttackCoefficient = Coefficient(0.020);
    mSlowReleaseCoefficient = Coefficient(0.100);
    mBlendReleaseCoefficient = Coefficient(0.015);
    mTransientHighpassCoefficient =
      static_cast<float>(std::exp(-2.0 * kPi * 1200.0 / sampleRate));
    mHoldDurationSamples = std::max(1, static_cast<int>(std::lround(0.008 * sampleRate)));

    mTargetSemitones = clampedSemitones;
    mCurrentSemitones = static_cast<float>(clampedSemitones);
    mTargetScale = initialScale;
    mCurrentScale = initialScale;
    mScaleStep = 0.0f;
    mSemitoneStep = 0.0f;
    mScaleRampRemaining = 0;
    mTargetWetMix = clampedSemitones == 0 ? 0.0f : 1.0f;
    mWetMix = mTargetWetMix;
    mWetMixStep = 0.0f;
    mWetRampRemaining = 0;
    ClearTransientState();
    mConfigured = true;
    return true;
  }

  void Process(const double* input, double* output, const int numFrames, const int semitones) noexcept
  {
    if (output == nullptr || numFrames <= 0)
      return;
    if (input == nullptr)
    {
      std::fill(output, output + numFrames, 0.0);
      return;
    }
    if (!mConfigured)
    {
      for (int frame = 0; frame < numFrames; ++frame)
        output[frame] = std::isfinite(input[frame]) ? input[frame] : 0.0;
      return;
    }

    SetSemitones(semitones);
    for (int frame = 0; frame < numFrames; ++frame)
    {
      const double drySample = std::isfinite(input[frame]) ? input[frame] : 0.0;
      const float sample = static_cast<float>(drySample);
      AdvancePitchRamp();

      const float quality = kQualityGain * mQuality.Process(sample, mCurrentScale);
      const float positiveShift = std::max(mCurrentSemitones, 0.0f) / 12.0f;
      const float positiveShiftSquared = positiveShift * positiveShift;
      const float fastGain = 1.0f + 5.0f * positiveShiftSquared * positiveShiftSquared;
      const float fast = fastGain * mFast.Process(sample, mCurrentScale);

      // A slew envelope reacts quickly to pick edges without continuously
      // retriggering on ordinary amplitude beating inside a chord.
      const float onsetMagnitude = std::abs(sample - mPreviousInput);
      mPreviousInput = sample;
      mFastEnvelope = Follow(onsetMagnitude, mFastEnvelope,
                             onsetMagnitude > mFastEnvelope ? mFastAttackCoefficient
                                                            : mFastReleaseCoefficient);
      mSlowEnvelope = Follow(onsetMagnitude, mSlowEnvelope,
                             onsetMagnitude > mSlowEnvelope ? mSlowAttackCoefficient
                                                            : mSlowReleaseCoefficient);

      if (mDetectorArmed && mFastEnvelope > 1.0e-5f
          && mFastEnvelope > std::max(3.0f * mSlowEnvelope, 2.0e-5f))
      {
        mTransientBlend = 1.0f;
        mHoldSamplesRemaining = mHoldDurationSamples;
        mDetectorArmed = false;
      }
      if (!mDetectorArmed
          && (mFastEnvelope < 1.25f * mSlowEnvelope || mSlowEnvelope < 1.0e-6f))
        mDetectorArmed = true;

      if (mHoldSamplesRemaining > 0)
      {
        --mHoldSamplesRemaining;
        mTransientBlend = 1.0f;
      }
      else
      {
        mTransientBlend *= mBlendReleaseCoefficient;
        if (mTransientBlend < 1.0e-6f)
          mTransientBlend = 0.0f;
      }

      mTransientHighpass = mTransientHighpassCoefficient
                           * (mTransientHighpass + sample - mHighpassPreviousInput);
      mHighpassPreviousInput = sample;

      const float shifted = mTransientBlend * (fast + kTransientHighpassGain * mTransientHighpass)
                            + (1.0f - mTransientBlend) * quality;
      AdvanceWetRamp();
      if (mWetMix == 0.0f)
      {
        output[frame] = drySample;
        continue;
      }
      const double result = (1.0 - static_cast<double>(mWetMix)) * drySample
                            + static_cast<double>(mWetMix) * shifted;
      if (std::isfinite(result))
      {
        output[frame] = static_cast<double>(result);
      }
      else
      {
        output[frame] = 0.0;
        ClearSignalState();
      }
    }
  }

  void ProcessInPlace(double* samples, const int numFrames, const int semitones) noexcept
  {
    Process(samples, samples, numFrames, semitones);
  }

  bool IsConfigured() const noexcept { return mConfigured; }
  int GetLatencySamples() const noexcept { return 0; }
  int GetBandCount() const noexcept { return mQuality.GetBandCount(); }
  int GetTargetSemitones() const noexcept { return mTargetSemitones; }

private:
  static constexpr float kPi = 3.14159265358979323846f;
  static constexpr float kTwoPi = 2.0f * kPi;
  static constexpr float kQualityGain = 2.8f;
  static constexpr float kTransientHighpassGain = 0.65f;

  static int ClampSemitones(const int semitones) noexcept
  {
    return std::clamp(semitones, kMinSemitones, kMaxSemitones);
  }

  static float SemitonesToScale(const int semitones)
  {
    return static_cast<float>(std::pow(2.0, static_cast<double>(semitones) / 12.0));
  }

  static float CenterFrequency(const int index)
  {
    return 480.0f * std::pow(2.0f, static_cast<float>(index) / 19.0f) - 420.0f;
  }

  static float Bandwidth(const int index)
  {
    const float lower = CenterFrequency(index - 1);
    const float center = CenterFrequency(index);
    const float upper = CenterFrequency(index + 1);
    const float upperDistance = upper - center;
    const float lowerDistance = center - lower;
    return 2.0f * upperDistance * lowerDistance / (upperDistance + lowerDistance);
  }

  static float BalancedAtan2(const float y, const float x) noexcept
  {
    if (x == 0.0f)
      return y > 0.0f ? 0.5f * kPi : (y < 0.0f ? -0.5f * kPi : 0.0f);

    const float absX = std::abs(x);
    const float absY = std::abs(y);
    const bool invert = absY > absX;
    const float ratio = invert ? absX / std::max(absY, std::numeric_limits<float>::min())
                               : absY / std::max(absX, std::numeric_limits<float>::min());
    const float ratio2 = ratio * ratio;
    float angle = (((0.0208351f * ratio2 - 0.0851330f) * ratio2 + 0.1801410f) * ratio2
                   - 0.3302995f)
                    * ratio2 * ratio
                  + ratio;
    if (invert)
      angle = 0.5f * kPi - angle;
    if (x < 0.0f)
      angle = kPi - angle;
    return std::copysign(angle, y);
  }

  static float BalancedSine(const float radians) noexcept
  {
    const float wrapped = radians - kTwoPi * std::floor((radians + kPi) / kTwoPi);
    constexpr float b = 4.0f / kPi;
    constexpr float c = -4.0f / (kPi * kPi);
    float estimate = b * wrapped + c * wrapped * std::abs(wrapped);
    constexpr float correction = 0.225f;
    return correction * (estimate * std::abs(estimate) - estimate) + estimate;
  }

  class Band
  {
  public:
    Band(const double center, const double sampleRate, const double bandwidth, const float initialScale)
      : mScale(initialScale)
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

    float Process(const float sample, const float scale) noexcept
    {
      const bool previousImaginarySign = std::signbit(mY.imag());
      mY = mState2 + mD0 * sample;
      mState2 = mState1 + mD1 * sample - mC1 * mY;
      mState1 = mD2 * sample - mC2 * mY;

      if (!std::isfinite(mY.real()) || !std::isfinite(mY.imag()))
      {
        ClearState();
        mScale = scale;
        return 0.0f;
      }

      const bool wrapped = mY.real() < 0.0f && std::signbit(mY.imag()) != previousImaginarySign;
      const float norm = std::norm(mY);
      if (!(norm > std::numeric_limits<float>::min()))
      {
        mScale = scale;
        return 0.0f;
      }

      const float phaseTurns = BalancedAtan2(mY.imag(), mY.real()) / kTwoPi;
      if (wrapped)
        mPhaseOffsetTurns += mScale;

      // Retune without a phase jump. Changing the scale normally moves the
      // current point on the waveform; the compensating offset keeps that
      // point continuous while the following samples adopt the new ratio.
      mPhaseOffsetTurns += (mScale - scale) * phaseTurns;
      mPhaseOffsetTurns -= std::floor(mPhaseOffsetTurns);
      mScale = scale;

      const float magnitude = std::sqrt(norm);
      return magnitude * BalancedSine(kTwoPi * (scale * phaseTurns + mPhaseOffsetTurns));
    }

    void ClearState() noexcept
    {
      mState1 = {};
      mState2 = {};
      mY = {};
      mPhaseOffsetTurns = 0.25f;
    }

  private:
    static std::complex<float> ToFloat(const std::complex<double>& value)
    {
      return {static_cast<float>(value.real()), static_cast<float>(value.imag())};
    }

    float mScale = 1.0f;
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

  class FilterBank
  {
  public:
    bool Configure(const double sampleRate, const double bandwidthScale, const float initialScale)
    {
      mBands.clear();
      mBands.reserve(kBandCount);
      for (int index = 0; index < kBandCount; ++index)
      {
        const double center = CenterFrequency(index);
        if (center >= sampleRate * 0.475)
          break;
        mBands.emplace_back(center, sampleRate, Bandwidth(index) * bandwidthScale, initialScale);
      }
      return !mBands.empty();
    }

    float Process(const float sample, const float scale) noexcept
    {
      float output = 0.0f;
      for (auto& band : mBands)
        output += band.Process(sample, scale);
      return output;
    }

    void ClearState() noexcept
    {
      for (auto& band : mBands)
        band.ClearState();
    }

    int GetBandCount() const noexcept { return static_cast<int>(mBands.size()); }

  private:
    std::vector<Band> mBands;
  };

  float Coefficient(const double seconds) const
  {
    return static_cast<float>(std::exp(-1.0 / std::max(seconds * mSampleRate, 1.0)));
  }

  static float Follow(const float input, const float state, const float coefficient) noexcept
  {
    return input + coefficient * (state - input);
  }

  void SetSemitones(const int semitones) noexcept
  {
    const int clamped = ClampSemitones(semitones);
    if (clamped == mTargetSemitones)
      return;

    mTargetSemitones = clamped;
    mTargetScale = SemitonesToScale(clamped);
    mScaleStep = (mTargetScale - mCurrentScale) / static_cast<float>(mTransitionSamples);
    mSemitoneStep = (static_cast<float>(clamped) - mCurrentSemitones)
                    / static_cast<float>(mTransitionSamples);
    mScaleRampRemaining = mTransitionSamples;

    mTargetWetMix = clamped == 0 ? 0.0f : 1.0f;
    mWetMixStep = (mTargetWetMix - mWetMix) / static_cast<float>(mTransitionSamples);
    mWetRampRemaining = mTransitionSamples;
  }

  void AdvancePitchRamp() noexcept
  {
    if (mScaleRampRemaining <= 0)
      return;
    mCurrentScale += mScaleStep;
    mCurrentSemitones += mSemitoneStep;
    if (--mScaleRampRemaining == 0)
    {
      mCurrentScale = mTargetScale;
      mCurrentSemitones = static_cast<float>(mTargetSemitones);
    }
  }

  void AdvanceWetRamp() noexcept
  {
    if (mWetRampRemaining <= 0)
      return;
    mWetMix += mWetMixStep;
    if (--mWetRampRemaining == 0)
      mWetMix = mTargetWetMix;
  }

  void ClearTransientState() noexcept
  {
    mFastEnvelope = 0.0f;
    mSlowEnvelope = 0.0f;
    mTransientBlend = 0.0f;
    mPreviousInput = 0.0f;
    mHighpassPreviousInput = 0.0f;
    mTransientHighpass = 0.0f;
    mHoldSamplesRemaining = 0;
    mDetectorArmed = true;
  }

  void ClearSignalState() noexcept
  {
    mQuality.ClearState();
    mFast.ClearState();
    ClearTransientState();
  }

  FilterBank mQuality;
  FilterBank mFast;
  double mSampleRate = 48000.0;
  int mTransitionSamples = 384;
  int mTargetSemitones = 0;
  float mCurrentSemitones = 0.0f;
  float mTargetScale = 1.0f;
  float mCurrentScale = 1.0f;
  float mScaleStep = 0.0f;
  float mSemitoneStep = 0.0f;
  int mScaleRampRemaining = 0;
  float mTargetWetMix = 0.0f;
  float mWetMix = 0.0f;
  float mWetMixStep = 0.0f;
  int mWetRampRemaining = 0;
  float mFastAttackCoefficient = 0.0f;
  float mFastReleaseCoefficient = 0.0f;
  float mSlowAttackCoefficient = 0.0f;
  float mSlowReleaseCoefficient = 0.0f;
  float mBlendReleaseCoefficient = 0.0f;
  float mTransientHighpassCoefficient = 0.0f;
  float mFastEnvelope = 0.0f;
  float mSlowEnvelope = 0.0f;
  float mTransientBlend = 0.0f;
  float mPreviousInput = 0.0f;
  float mHighpassPreviousInput = 0.0f;
  float mTransientHighpass = 0.0f;
  int mHoldDurationSamples = 0;
  int mHoldSamplesRemaining = 0;
  bool mDetectorArmed = true;
  bool mConfigured = false;
};
} // namespace dessmetal::transpose

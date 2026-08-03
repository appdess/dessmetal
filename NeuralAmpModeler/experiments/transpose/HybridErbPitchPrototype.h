#pragma once

#include "ErbPitchPrototype.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace dessmetal::experiments::transpose
{
// Evaluation-only two-resolution extension. A broad filter bank supplies the
// first part of detected attacks while the normal bank continuously builds its
// cleaner steady-state response. This mirrors the transient/body split suggested
// by measured commercial octave-pedal responses without copying proprietary DSP.
class HybridErbPitchPrototype
{
public:
  using MathMode = ErbPitchPrototype::MathMode;
  static constexpr int kMinSemitones = ErbPitchPrototype::kMinSemitones;
  static constexpr int kMaxSemitones = ErbPitchPrototype::kMaxSemitones;
  static constexpr int kDefaultBandCount = ErbPitchPrototype::kDefaultBandCount;

  bool Reset(const double sampleRate, const int maxBlockSize, const MathMode mathMode)
  {
    mConfigured = false;
    if (!std::isfinite(sampleRate) || sampleRate < 8000.0 || maxBlockSize <= 0)
      return false;
    if (!mQuality.Reset(sampleRate, maxBlockSize, mathMode, kDefaultBandCount, 19.0, 0.5)
        || !mFast.Reset(sampleRate, maxBlockSize, mathMode, kDefaultBandCount, 19.0, 2.0))
      return false;

    mQualityOutput.assign(static_cast<std::size_t>(maxBlockSize), 0.0);
    mFastOutput.assign(static_cast<std::size_t>(maxBlockSize), 0.0);
    mSampleRate = sampleRate;
    mFastAttackCoefficient = Coefficient(0.0005);
    mFastReleaseCoefficient = Coefficient(0.010);
    mSlowAttackCoefficient = Coefficient(0.020);
    mSlowReleaseCoefficient = Coefficient(0.100);
    mBlendReleaseCoefficient = Coefficient(0.015);
    mTransientHighpassCoefficient = std::exp(-2.0 * 3.14159265358979323846 * 1200.0 / sampleRate);
    mHoldDurationSamples = std::max(1, static_cast<int>(std::lround(0.008 * sampleRate)));
    mFastEnvelope = 0.0;
    mSlowEnvelope = 0.0;
    mTransientBlend = 0.0;
    mPreviousInput = 0.0;
    mHighpassPreviousInput = 0.0;
    mTransientHighpass = 0.0;
    mHoldSamplesRemaining = 0;
    mDetectorArmed = true;
    mConfigured = true;
    return true;
  }

  void Process(const double* input, double* output, const int numFrames, const int semitones)
  {
    if (output == nullptr || numFrames <= 0)
      return;
    if (!mConfigured || input == nullptr || numFrames > static_cast<int>(mQualityOutput.size()))
    {
      std::fill(output, output + numFrames, 0.0);
      return;
    }

    const int clampedSemitones = std::clamp(semitones, kMinSemitones, kMaxSemitones);
    mQuality.Process(input, mQualityOutput.data(), numFrames, clampedSemitones);
    mFast.Process(input, mFastOutput.data(), numFrames, clampedSemitones);

    const double positiveShift = std::max(clampedSemitones, 0) / 12.0;
    const double positiveShiftSquared = positiveShift * positiveShift;
    const double fastGain = 1.0 + 5.0 * positiveShiftSquared * positiveShiftSquared;
    constexpr double qualityGain = 2.8;
    for (int frame = 0; frame < numFrames; ++frame)
    {
      // A slew envelope reacts to pick edges but is much less likely than a raw
      // amplitude envelope to retrigger on the normal beating inside a chord.
      const double onsetMagnitude = std::abs(input[frame] - mPreviousInput);
      mPreviousInput = input[frame];
      mFastEnvelope = Follow(onsetMagnitude, mFastEnvelope,
                             onsetMagnitude > mFastEnvelope ? mFastAttackCoefficient
                                                            : mFastReleaseCoefficient);
      mSlowEnvelope = Follow(onsetMagnitude, mSlowEnvelope,
                             onsetMagnitude > mSlowEnvelope ? mSlowAttackCoefficient
                                                            : mSlowReleaseCoefficient);

      if (mDetectorArmed && mFastEnvelope > 1.0e-5
          && mFastEnvelope > std::max(3.0 * mSlowEnvelope, 2.0e-5))
      {
        mTransientBlend = 1.0;
        mHoldSamplesRemaining = mHoldDurationSamples;
        mDetectorArmed = false;
      }
      if (!mDetectorArmed && (mFastEnvelope < 1.25 * mSlowEnvelope || mSlowEnvelope < 1.0e-6))
        mDetectorArmed = true;

      if (mHoldSamplesRemaining > 0)
      {
        --mHoldSamplesRemaining;
        mTransientBlend = 1.0;
      }
      else
      {
        mTransientBlend *= mBlendReleaseCoefficient;
        if (mTransientBlend < 1.0e-6)
          mTransientBlend = 0.0;
      }

      mTransientHighpass = mTransientHighpassCoefficient
                           * (mTransientHighpass + input[frame] - mHighpassPreviousInput);
      mHighpassPreviousInput = input[frame];

      if (clampedSemitones == 0)
      {
        output[frame] = input[frame];
        continue;
      }
      const double quality = qualityGain * mQualityOutput[static_cast<std::size_t>(frame)];
      const double fast = fastGain * mFastOutput[static_cast<std::size_t>(frame)];
      constexpr double transientHighpassGain = 0.65;
      output[frame] = mTransientBlend * (fast + transientHighpassGain * mTransientHighpass)
                      + (1.0 - mTransientBlend) * quality;
    }
  }

  bool IsConfigured() const { return mConfigured; }
  int GetLatencySamples() const { return 0; }
  int GetBandCount() const { return mQuality.GetBandCount(); }

private:
  double Coefficient(const double seconds) const
  {
    return std::exp(-1.0 / std::max(seconds * mSampleRate, 1.0));
  }

  static double Follow(const double input, const double state, const double coefficient)
  {
    return input + coefficient * (state - input);
  }

  ErbPitchPrototype mQuality;
  ErbPitchPrototype mFast;
  std::vector<double> mQualityOutput;
  std::vector<double> mFastOutput;
  double mSampleRate = 48000.0;
  double mFastAttackCoefficient = 0.0;
  double mFastReleaseCoefficient = 0.0;
  double mSlowAttackCoefficient = 0.0;
  double mSlowReleaseCoefficient = 0.0;
  double mBlendReleaseCoefficient = 0.0;
  double mTransientHighpassCoefficient = 0.0;
  double mFastEnvelope = 0.0;
  double mSlowEnvelope = 0.0;
  double mTransientBlend = 0.0;
  double mPreviousInput = 0.0;
  double mHighpassPreviousInput = 0.0;
  double mTransientHighpass = 0.0;
  int mHoldDurationSamples = 0;
  int mHoldSamplesRemaining = 0;
  bool mDetectorArmed = true;
  bool mConfigured = false;
};
} // namespace dessmetal::experiments::transpose

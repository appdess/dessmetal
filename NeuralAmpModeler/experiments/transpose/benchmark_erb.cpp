#include "ErbPitchPrototype.h"
#include "HybridErbPitchPrototype.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <new>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace
{
std::atomic<bool> gTrackAllocations{false};
std::atomic<std::size_t> gAllocationCount{0};

constexpr double kSampleRate = 48000.0;
constexpr int kMaxBlockSize = 512;
constexpr int kRenderFrames = 72000;
constexpr int kAnalysisStart = 36000;
constexpr int kAnalysisLength = 24000;
constexpr double kPi = 3.14159265358979323846;

using ErbPrototype = dessmetal::experiments::transpose::ErbPitchPrototype;
using HybridPrototype = dessmetal::experiments::transpose::HybridErbPitchPrototype;

bool gUseHybrid = false;

class BenchmarkPrototype
{
public:
  using MathMode = ErbPrototype::MathMode;
  static constexpr int kDefaultBandCount = ErbPrototype::kDefaultBandCount;

  bool Reset(const double sampleRate, const int maxBlockSize, const MathMode mathMode,
             const int bandCount = kDefaultBandCount, const double density = 19.0,
             const double bandwidthScale = 1.0, const double lowBandwidthScale = 0.0,
             const double transitionHz = 300.0, const bool phaseCompensation = false,
             const int dominanceExponent = 0)
  {
    mHybrid = gUseHybrid;
    if (mHybrid)
      return mHybridProcessor.Reset(sampleRate, maxBlockSize, mathMode);
    return mErbProcessor.Reset(sampleRate, maxBlockSize, mathMode, bandCount, density,
                               bandwidthScale, lowBandwidthScale, transitionHz,
                               phaseCompensation, dominanceExponent);
  }

  void Process(const double* input, double* output, const int frames, const int semitones)
  {
    if (mHybrid)
      mHybridProcessor.Process(input, output, frames, semitones);
    else
      mErbProcessor.Process(input, output, frames, semitones);
  }

  bool IsConfigured() const
  {
    return mHybrid ? mHybridProcessor.IsConfigured() : mErbProcessor.IsConfigured();
  }
  int GetLatencySamples() const
  {
    return mHybrid ? mHybridProcessor.GetLatencySamples() : mErbProcessor.GetLatencySamples();
  }
  int GetBandCount() const
  {
    return mHybrid ? mHybridProcessor.GetBandCount() : mErbProcessor.GetBandCount();
  }

private:
  ErbPrototype mErbProcessor;
  HybridPrototype mHybridProcessor;
  bool mHybrid = false;
};

using Prototype = BenchmarkPrototype;

int gBandCount = Prototype::kDefaultBandCount;
double gDensity = 19.0;
double gBandwidthScale = 1.0;
double gLowBandwidthScale = 0.0;
double gBandwidthTransitionHz = 300.0;
bool gCompensateFilterPhase = false;
int gDominanceExponent = 0;

bool ResetProcessor(Prototype& processor, const Prototype::MathMode mathMode)
{
  return processor.Reset(kSampleRate, kMaxBlockSize, mathMode, gBandCount, gDensity, gBandwidthScale,
                         gLowBandwidthScale, gBandwidthTransitionHz, gCompensateFilterPhase,
                         gDominanceExponent);
}

struct Measurement
{
  double inputHz = 0.0;
  int semitones = 0;
  double expectedHz = 0.0;
  double dominantHz = 0.0;
  double errorCents = 0.0;
  double carrierDeficitDb = 0.0;
  double outputRms = 0.0;
};

struct ChordMeasurement
{
  int semitones = 0;
  double targetSnrDb = 0.0;
  double outputRms = 0.0;
};

struct OnsetMeasurement
{
  double inputHz = 0.0;
  int semitones = 0;
  int tenPercentSample = 0;
  int fiftyPercentSample = 0;
};

void Require(const bool condition, const char* message)
{
  if (!condition)
    throw std::runtime_error(message);
}

Prototype::MathMode ParseMathMode(const std::string& name)
{
  if (name == "embedded")
    return Prototype::MathMode::kEmbeddedApproximation;
  if (name == "balanced")
    return Prototype::MathMode::kBalancedApproximation;
  if (name == "accurate")
    return Prototype::MathMode::kAccurate;
  throw std::runtime_error("unknown math mode: " + name);
}

const char* MathModeName(const Prototype::MathMode mode)
{
  switch (mode)
  {
    case Prototype::MathMode::kEmbeddedApproximation: return "embedded";
    case Prototype::MathMode::kBalancedApproximation: return "balanced";
    case Prototype::MathMode::kAccurate: return "accurate";
  }
  return "unknown";
}

std::vector<double> RenderSine(const double inputFrequency, const int semitones,
                               const std::vector<int>& blockPattern, const Prototype::MathMode mathMode,
                               const int renderFrames = kRenderFrames)
{
  Prototype processor;
  Require(ResetProcessor(processor, mathMode), "processor reset failed");
  std::vector<double> rendered(static_cast<std::size_t>(renderFrames));
  std::vector<double> input(kMaxBlockSize);
  double phase = 0.0;
  int frame = 0;
  std::size_t patternIndex = 0;
  while (frame < renderFrames)
  {
    const int frames = std::min(blockPattern[patternIndex++ % blockPattern.size()], renderFrames - frame);
    for (int index = 0; index < frames; ++index)
    {
      input[static_cast<std::size_t>(index)] = 0.2 * std::sin(phase);
      phase += 2.0 * kPi * inputFrequency / kSampleRate;
      if (phase >= 2.0 * kPi)
        phase -= 2.0 * kPi;
    }
    processor.Process(input.data(), rendered.data() + frame, frames, semitones);
    frame += frames;
  }
  return rendered;
}

std::vector<double> WindowAnalysis(const std::vector<double>& rendered)
{
  std::vector<double> result(kAnalysisLength);
  for (int index = 0; index < kAnalysisLength; ++index)
  {
    const double hann = 0.5 - 0.5 * std::cos(2.0 * kPi * index / (kAnalysisLength - 1));
    result[static_cast<std::size_t>(index)] =
      rendered[static_cast<std::size_t>(kAnalysisStart + index)] * hann;
  }
  return result;
}

double MagnitudeAt(const std::vector<double>& samples, const double frequency)
{
  std::complex<double> sum{0.0, 0.0};
  const std::complex<double> rotation = std::polar(1.0, -2.0 * kPi * frequency / kSampleRate);
  std::complex<double> oscillator{1.0, 0.0};
  for (const double sample : samples)
  {
    sum += sample * oscillator;
    oscillator *= rotation;
  }
  return std::abs(sum) / samples.size();
}

void FourierTransform(std::vector<std::complex<double>>& values)
{
  const std::size_t size = values.size();
  Require(size > 0 && (size & (size - 1)) == 0, "FFT size must be a power of two");
  for (std::size_t index = 1, reversed = 0; index < size; ++index)
  {
    std::size_t bit = size >> 1;
    while (reversed & bit)
    {
      reversed ^= bit;
      bit >>= 1;
    }
    reversed ^= bit;
    if (index < reversed)
      std::swap(values[index], values[reversed]);
  }

  for (std::size_t length = 2; length <= size; length <<= 1)
  {
    const std::complex<double> step = std::polar(1.0, -2.0 * kPi / static_cast<double>(length));
    for (std::size_t offset = 0; offset < size; offset += length)
    {
      std::complex<double> rotation{1.0, 0.0};
      for (std::size_t index = 0; index < length / 2; ++index)
      {
        const std::complex<double> even = values[offset + index];
        const std::complex<double> odd = values[offset + index + length / 2] * rotation;
        values[offset + index] = even + odd;
        values[offset + index + length / 2] = even - odd;
        rotation *= step;
      }
    }
  }
}

std::vector<std::pair<double, double>> GuitarChordPartials()
{
  const std::vector<double> fundamentals{82.4069, 123.471, 164.814, 195.998, 246.942, 329.628};
  std::vector<std::pair<double, double>> partials;
  for (std::size_t stringIndex = 0; stringIndex < fundamentals.size(); ++stringIndex)
  {
    for (int harmonic = 1; harmonic <= 16; ++harmonic)
    {
      const double frequency = fundamentals[stringIndex] * harmonic;
      if (frequency >= 9000.0)
        break;
      const double stringBalance = 1.0 - 0.06 * static_cast<double>(stringIndex);
      const double amplitude = stringBalance / std::pow(static_cast<double>(harmonic), 1.15);
      partials.emplace_back(frequency, amplitude);
    }
  }
  return partials;
}

std::vector<double> RenderChord(const int semitones, const Prototype::MathMode mathMode,
                                const int renderFrames = 96000)
{
  const auto partials = GuitarChordPartials();
  std::vector<double> phases(partials.size(), 0.0);
  for (std::size_t index = 0; index < phases.size(); ++index)
    phases[index] = std::fmod(0.754877666 * static_cast<double>(index * index + 1), 1.0) * 2.0 * kPi;

  Prototype processor;
  Require(ResetProcessor(processor, mathMode), "processor reset failed");
  std::vector<double> rendered(static_cast<std::size_t>(renderFrames));
  std::vector<double> input(kMaxBlockSize);
  int frame = 0;
  while (frame < renderFrames)
  {
    const int frames = std::min(kMaxBlockSize, renderFrames - frame);
    for (int index = 0; index < frames; ++index)
    {
      double sample = 0.0;
      for (std::size_t partial = 0; partial < partials.size(); ++partial)
      {
        sample += partials[partial].second * std::sin(phases[partial]);
        phases[partial] += 2.0 * kPi * partials[partial].first / kSampleRate;
        if (phases[partial] >= 2.0 * kPi)
          phases[partial] -= 2.0 * kPi;
      }
      input[static_cast<std::size_t>(index)] = sample * 0.0125;
    }
    processor.Process(input.data(), rendered.data() + frame, frames, semitones);
    frame += frames;
  }
  return rendered;
}

ChordMeasurement MeasureChord(const int semitones, const Prototype::MathMode mathMode)
{
  constexpr int fftSize = 32768;
  const auto rendered = RenderChord(semitones, mathMode);
  std::vector<std::complex<double>> spectrum(fftSize);
  const int start = static_cast<int>(rendered.size()) - fftSize;
  double outputEnergy = 0.0;
  for (int index = 0; index < fftSize; ++index)
  {
    const double sample = rendered[static_cast<std::size_t>(start + index)];
    const double hann = 0.5 - 0.5 * std::cos(2.0 * kPi * index / (fftSize - 1));
    spectrum[static_cast<std::size_t>(index)] = sample * hann;
    outputEnergy += sample * sample;
  }
  FourierTransform(spectrum);

  std::vector<bool> expectedBins(static_cast<std::size_t>(fftSize / 2 + 1), false);
  const double scale = std::pow(2.0, semitones / 12.0);
  for (const auto& [frequency, amplitude] : GuitarChordPartials())
  {
    (void) amplitude;
    const double shiftedFrequency = frequency * scale;
    if (shiftedFrequency < 20.0 || shiftedFrequency >= kSampleRate * 0.495)
      continue;
    const int centerBin = static_cast<int>(std::lround(shiftedFrequency * fftSize / kSampleRate));
    for (int bin = std::max(centerBin - 2, 1); bin <= std::min(centerBin + 2, fftSize / 2); ++bin)
      expectedBins[static_cast<std::size_t>(bin)] = true;
  }

  double targetPower = 0.0;
  double residualPower = 0.0;
  for (int bin = 1; bin <= fftSize / 2; ++bin)
  {
    const double frequency = static_cast<double>(bin) * kSampleRate / fftSize;
    if (frequency < 20.0)
      continue;
    const double power = std::norm(spectrum[static_cast<std::size_t>(bin)]);
    if (expectedBins[static_cast<std::size_t>(bin)])
      targetPower += power;
    else
      residualPower += power;
  }

  ChordMeasurement result;
  result.semitones = semitones;
  result.targetSnrDb = 10.0 * std::log10(std::max(targetPower, 1.0e-30)
                                         / std::max(residualPower, 1.0e-30));
  result.outputRms = std::sqrt(outputEnergy / fftSize);
  return result;
}

OnsetMeasurement MeasureOnset(const double inputFrequency, const int semitones,
                              const Prototype::MathMode mathMode)
{
  constexpr int onsetSample = 1024;
  constexpr int frameCount = 24000;
  constexpr int windowSize = 48;
  Prototype processor;
  Require(ResetProcessor(processor, mathMode), "processor reset failed");
  std::vector<double> input(frameCount, 0.0);
  std::vector<double> output(frameCount, 0.0);
  double phase = 0.0;
  for (int frame = onsetSample; frame < frameCount; ++frame)
  {
    const double ramp = std::min(1.0, static_cast<double>(frame - onsetSample) / 24.0);
    input[static_cast<std::size_t>(frame)] = 0.2 * ramp * std::sin(phase);
    phase += 2.0 * kPi * inputFrequency / kSampleRate;
    if (phase >= 2.0 * kPi)
      phase -= 2.0 * kPi;
  }
  for (int offset = 0; offset < frameCount; offset += kMaxBlockSize)
    processor.Process(input.data() + offset, output.data() + offset,
                      std::min(kMaxBlockSize, frameCount - offset), semitones);

  double steadyEnergy = 0.0;
  for (int frame = frameCount - 4096; frame < frameCount; ++frame)
    steadyEnergy += output[static_cast<std::size_t>(frame)] * output[static_cast<std::size_t>(frame)];
  const double steadyRms = std::sqrt(steadyEnergy / 4096.0);

  int tenPercentSample = frameCount - onsetSample;
  int fiftyPercentSample = frameCount - onsetSample;
  for (int frame = onsetSample; frame <= frameCount - windowSize; ++frame)
  {
    double windowEnergy = 0.0;
    for (int index = 0; index < windowSize; ++index)
    {
      const double sample = output[static_cast<std::size_t>(frame + index)];
      windowEnergy += sample * sample;
    }
    const double rms = std::sqrt(windowEnergy / windowSize);
    if (tenPercentSample == frameCount - onsetSample && rms >= steadyRms * 0.1)
      tenPercentSample = frame - onsetSample;
    if (fiftyPercentSample == frameCount - onsetSample && rms >= steadyRms * 0.5)
    {
      fiftyPercentSample = frame - onsetSample;
      break;
    }
  }

  return {inputFrequency, semitones, tenPercentSample, fiftyPercentSample};
}

Measurement Measure(const double inputFrequency, const int semitones, const Prototype::MathMode mathMode)
{
  const auto rendered = RenderSine(inputFrequency, semitones, {31, 64, 127, 256, 511}, mathMode);
  Require(std::all_of(rendered.begin(), rendered.end(), [](const double x) { return std::isfinite(x); }),
          "processor produced a non-finite sample");
  const auto analysis = WindowAnalysis(rendered);
  const double expected = inputFrequency * std::pow(2.0, semitones / 12.0);
  const double carrierMagnitude = MagnitudeAt(analysis, expected);
  double dominantFrequency = expected;
  double dominantMagnitude = 0.0;
  for (double cents = -300.0; cents <= 300.0; cents += 1.0)
  {
    const double frequency = expected * std::pow(2.0, cents / 1200.0);
    const double magnitude = MagnitudeAt(analysis, frequency);
    if (magnitude > dominantMagnitude)
    {
      dominantMagnitude = magnitude;
      dominantFrequency = frequency;
    }
  }

  double energy = 0.0;
  for (const double sample : analysis)
    energy += sample * sample;

  Measurement result;
  result.inputHz = inputFrequency;
  result.semitones = semitones;
  result.expectedHz = expected;
  result.dominantHz = dominantFrequency;
  result.errorCents = 1200.0 * std::log2(dominantFrequency / expected);
  result.carrierDeficitDb = 20.0 * std::log10(std::max(carrierMagnitude, 1.0e-15)
                                              / std::max(dominantMagnitude, 1.0e-15));
  result.outputRms = std::sqrt(energy / analysis.size()) / std::sqrt(0.375);
  return result;
}

double MeasureRealtimeCpuPercent(const Prototype::MathMode mathMode)
{
  Prototype processor;
  Require(ResetProcessor(processor, mathMode), "processor reset failed");
  std::vector<double> input(kMaxBlockSize, 0.0);
  std::vector<double> output(kMaxBlockSize, 0.0);
  for (int frame = 0; frame < kMaxBlockSize; ++frame)
  {
    const double time = frame / kSampleRate;
    input[static_cast<std::size_t>(frame)] = 0.08 * std::sin(2.0 * kPi * 82.4069 * time)
                                                   + 0.05 * std::sin(2.0 * kPi * 123.471 * time)
                                                   + 0.03 * std::sin(2.0 * kPi * 329.628 * time);
  }
  constexpr int frames = static_cast<int>(kSampleRate * 5.0);
  const auto started = std::chrono::steady_clock::now();
  for (int offset = 0; offset < frames; offset += kMaxBlockSize)
    processor.Process(input.data(), output.data(), std::min(kMaxBlockSize, frames - offset), -7);
  const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  return elapsed / 5.0 * 100.0;
}

void RunFunctionalChecks(const Prototype::MathMode mathMode)
{
  Prototype processor;
  Require(!processor.Reset(0.0, kMaxBlockSize, mathMode), "invalid sample rate was accepted");
  Require(!processor.Reset(kSampleRate, 0, mathMode), "invalid block size was accepted");
  Require(ResetProcessor(processor, mathMode), "valid reset failed");
  Require(processor.IsConfigured(), "processor did not become configured");
  Require(processor.GetLatencySamples() == 0, "unexpected buffered latency");
  Require(processor.GetBandCount() == gBandCount, "unexpected band count");

  std::vector<double> input(kMaxBlockSize, 0.1);
  std::vector<double> output(kMaxBlockSize, 0.0);
  processor.Process(input.data(), output.data(), kMaxBlockSize, 0);
  gAllocationCount.store(0, std::memory_order_relaxed);
  gTrackAllocations.store(true, std::memory_order_release);
  for (int block = 0; block < 128; ++block)
    processor.Process(input.data(), output.data(), kMaxBlockSize, (block % 25) - 12);
  gTrackAllocations.store(false, std::memory_order_release);
  Require(gAllocationCount.load(std::memory_order_relaxed) == 0, "render path allocated memory");

  constexpr int blockCheckFrames = 8192;
  const auto fixedBlocks = RenderSine(110.0, -5, {512}, mathMode, blockCheckFrames);
  const auto varyingBlocks = RenderSine(110.0, -5, {31, 64, 127, 256, 511}, mathMode, blockCheckFrames);
  double maximumDifference = 0.0;
  for (std::size_t index = 0; index < fixedBlocks.size(); ++index)
    maximumDifference = std::max(maximumDifference, std::abs(fixedBlocks[index] - varyingBlocks[index]));
  Require(maximumDifference == 0.0, "output depends on host block boundaries");
}
} // namespace

void* operator new(const std::size_t size)
{
  if (gTrackAllocations.load(std::memory_order_relaxed))
    gAllocationCount.fetch_add(1, std::memory_order_relaxed);
  if (void* memory = std::malloc(size))
    return memory;
  throw std::bad_alloc();
}

void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }

int main(int argc, char** argv)
{
  try
  {
    Prototype::MathMode mathMode = Prototype::MathMode::kBalancedApproximation;
    bool functionalOnly = false;
    for (int index = 1; index < argc; ++index)
    {
      const std::string argument(argv[index]);
      if (argument == "--functional-only")
        functionalOnly = true;
      else if (argument == "--hybrid")
        gUseHybrid = true;
      else if (argument.rfind("--math=", 0) == 0)
        mathMode = ParseMathMode(argument.substr(7));
      else if (argument.rfind("--bands=", 0) == 0)
        gBandCount = std::stoi(argument.substr(8));
      else if (argument.rfind("--density=", 0) == 0)
        gDensity = std::stod(argument.substr(10));
      else if (argument.rfind("--bandwidth-scale=", 0) == 0)
        gBandwidthScale = std::stod(argument.substr(18));
      else if (argument.rfind("--low-bandwidth-scale=", 0) == 0)
        gLowBandwidthScale = std::stod(argument.substr(22));
      else if (argument.rfind("--bandwidth-transition=", 0) == 0)
        gBandwidthTransitionHz = std::stod(argument.substr(23));
      else if (argument == "--compensate-filter-phase")
        gCompensateFilterPhase = true;
      else if (argument.rfind("--dominance-exponent=", 0) == 0)
        gDominanceExponent = std::stoi(argument.substr(21));
      else
        throw std::runtime_error("unknown argument: " + argument);
    }

    RunFunctionalChecks(mathMode);
    if (functionalOnly)
    {
      std::cout << "engine=erb math=" << MathModeName(mathMode) << " functional_gate=PASS\n";
      return 0;
    }

    const std::vector<double> openStringHz{82.4069, 110.0, 146.832, 195.998, 246.942, 329.628};
    const std::vector<int> shifts{-12, 12};
    bool qualityPassed = true;
    std::cout << std::fixed << std::setprecision(3);
    std::cout << "engine=" << (gUseHybrid ? "hybrid-erb" : "erb")
              << " math=" << MathModeName(mathMode) << " bands=" << gBandCount
              << " density=" << gDensity << " bandwidth_scale=" << gBandwidthScale
              << " low_bandwidth_scale=" << gLowBandwidthScale
              << " bandwidth_transition_hz=" << gBandwidthTransitionHz
              << " phase_compensation=" << (gCompensateFilterPhase ? "on" : "off")
              << " dominance_exponent=" << gDominanceExponent << '\n';
    std::cout << "input_hz,semitones,expected_hz,dominant_hz,error_cents,carrier_deficit_db,output_rms\n";
    for (const double inputFrequency : openStringHz)
    {
      for (const int semitones : shifts)
      {
        const auto result = Measure(inputFrequency, semitones, mathMode);
        std::cout << result.inputHz << ',' << result.semitones << ',' << result.expectedHz << ','
                  << result.dominantHz << ',' << result.errorCents << ',' << result.carrierDeficitDb << ','
                  << result.outputRms << '\n';
        qualityPassed = qualityPassed && std::abs(result.errorCents) <= 5.0 && result.carrierDeficitDb >= -3.0;
      }
    }

    double maximumIntermediatePitchError = 0.0;
    double worstIntermediateCarrierDeficit = 0.0;
    int worstPitchShift = 0;
    int worstCarrierShift = 0;
    for (int semitones = -12; semitones <= 12; ++semitones)
    {
      if (semitones == -12 || semitones == 12)
        continue;
      const auto result = Measure(110.0, semitones, mathMode);
      if (std::abs(result.errorCents) > maximumIntermediatePitchError)
      {
        maximumIntermediatePitchError = std::abs(result.errorCents);
        worstPitchShift = semitones;
      }
      if (result.carrierDeficitDb < worstIntermediateCarrierDeficit)
      {
        worstIntermediateCarrierDeficit = result.carrierDeficitDb;
        worstCarrierShift = semitones;
      }
    }
    std::cout << "intermediate_a2_max_abs_error_cents=" << maximumIntermediatePitchError
              << " shift=" << worstPitchShift
              << " worst_carrier_deficit_db=" << worstIntermediateCarrierDeficit
              << " shift=" << worstCarrierShift << '\n';

    std::cout << "chord_shift,target_snr_db,output_rms\n";
    for (const int semitones : std::vector<int>{-12, -7, 7, 12})
    {
      const auto result = MeasureChord(semitones, mathMode);
      std::cout << result.semitones << ',' << result.targetSnrDb << ',' << result.outputRms << '\n';
    }

    std::cout << "onset_input_hz,semitones,ten_percent_samples,fifty_percent_samples,"
                 "ten_percent_ms,fifty_percent_ms\n";
    for (const auto [inputFrequency, semitones] :
         std::vector<std::pair<double, int>>{{82.4069, -12}, {82.4069, 12}, {329.628, -12}, {329.628, 12}})
    {
      const auto result = MeasureOnset(inputFrequency, semitones, mathMode);
      std::cout << result.inputHz << ',' << result.semitones << ',' << result.tenPercentSample << ','
                << result.fiftyPercentSample << ',' << result.tenPercentSample * 1000.0 / kSampleRate << ','
                << result.fiftyPercentSample * 1000.0 / kSampleRate << '\n';
    }

    std::cout << "buffered_latency_samples=0 band_count=" << gBandCount
              << " realtime_cpu_percent=" << MeasureRealtimeCpuPercent(mathMode) << '\n';
    std::cout << "functional_gate=PASS sine_quality_gate=" << (qualityPassed ? "PASS" : "FAIL") << '\n';
    return qualityPassed ? 0 : 2;
  }
  catch (const std::exception& error)
  {
    std::cerr << "benchmark error: " << error.what() << '\n';
    return 1;
  }
}

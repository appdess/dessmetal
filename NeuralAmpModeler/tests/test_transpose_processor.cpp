#include "../TransposeProcessor.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>
#include <vector>

namespace
{
std::atomic<bool> gTrackAllocations{false};
std::atomic<std::size_t> gAllocationCount{0};

constexpr double kPi = 3.14159265358979323846;

void Require(const bool condition, const char* message)
{
  if (!condition)
    throw std::runtime_error(message);
}

double ToneMagnitude(const std::vector<double>& signal, const int start, const int length,
                     const double frequency, const double sampleRate)
{
  double real = 0.0;
  double imaginary = 0.0;
  double windowSum = 0.0;
  for (int index = 0; index < length; ++index)
  {
    const double window = 0.5 - 0.5 * std::cos(2.0 * kPi * index / (length - 1));
    const double angle = 2.0 * kPi * frequency * index / sampleRate;
    const double sample = signal[static_cast<std::size_t>(start + index)] * window;
    real += sample * std::cos(angle);
    imaginary -= sample * std::sin(angle);
    windowSum += window;
  }
  return 2.0 * std::hypot(real, imaginary) / windowSum;
}

std::vector<double> RenderSine(const double sampleRate, const double frequency, const int semitones,
                               const std::vector<int>& blockPattern)
{
  constexpr int seconds = 2;
  const int frames = static_cast<int>(sampleRate) * seconds;
  dessmetal::transpose::Processor processor;
  Require(processor.Reset(sampleRate, semitones), "valid reset failed");
  std::vector<double> input(static_cast<std::size_t>(frames));
  std::vector<double> output(static_cast<std::size_t>(frames));
  for (int frame = 0; frame < frames; ++frame)
    input[static_cast<std::size_t>(frame)] = 0.2 * std::sin(2.0 * kPi * frequency * frame / sampleRate);

  int offset = 0;
  std::size_t patternIndex = 0;
  while (offset < frames)
  {
    const int block = std::min(blockPattern[patternIndex++ % blockPattern.size()], frames - offset);
    processor.Process(input.data() + offset, output.data() + offset, block, semitones);
    offset += block;
  }
  return output;
}

void TestConfigurationAndBypass()
{
  dessmetal::transpose::Processor processor;
  Require(!processor.Reset(std::numeric_limits<double>::quiet_NaN()), "NaN sample rate accepted");
  Require(!processor.Reset(7999.0), "too-low sample rate accepted");
  Require(processor.Reset(48000.0), "48 kHz reset failed");
  Require(processor.IsConfigured(), "processor not configured");
  Require(processor.GetLatencySamples() == 0, "unexpected buffered latency");
  Require(processor.GetBandCount() == dessmetal::transpose::Processor::kBandCount,
          "unexpected band count");

  std::vector<double> input(4096);
  std::vector<double> output(input.size());
  for (std::size_t index = 0; index < input.size(); ++index)
    input[index] = 0.3 * std::sin(0.013 * static_cast<double>(index))
                   + 0.1 * std::cos(0.031 * static_cast<double>(index));
  processor.Process(input.data(), output.data(), static_cast<int>(input.size()), 0);
  Require(input == output, "zero-semitone bypass is not bit exact");

  processor.ProcessInPlace(input.data(), static_cast<int>(input.size()), 0);
  Require(input == output, "in-place zero-semitone bypass changed samples");
}

void TestPitchAccuracyAndBlockInvariance()
{
  constexpr double sampleRate = 48000.0;
  constexpr double frequencies[]{82.407, 110.0, 146.832, 195.998, 246.942, 329.628};
  constexpr int shifts[]{-12, 12};
  for (const double frequency : frequencies)
  {
    for (const int semitones : shifts)
    {
      const auto fixed = RenderSine(sampleRate, frequency, semitones, {512});
      const auto irregular = RenderSine(sampleRate, frequency, semitones, {1, 17, 64, 3, 255, 31, 128});
      Require(fixed == irregular, "render changed with host block partitioning");

      const int analysisLength = 24000;
      const int analysisStart = static_cast<int>(fixed.size()) - analysisLength;
      const double expected = frequency * std::pow(2.0, semitones / 12.0);
      const double target = ToneMagnitude(fixed, analysisStart, analysisLength, expected, sampleRate);
      double strongest = 0.0;
      double dominant = expected;
      for (double candidate = expected * 0.85; candidate <= expected * 1.15; candidate += 0.25)
      {
        const double magnitude = ToneMagnitude(fixed, analysisStart, analysisLength, candidate, sampleRate);
        if (magnitude > strongest)
        {
          strongest = magnitude;
          dominant = candidate;
        }
      }
      const double cents = 1200.0 * std::log2(dominant / expected);
      const double deficitDb = 20.0 * std::log10(std::max(strongest, 1.0e-15)
                                                  / std::max(target, 1.0e-15));
      Require(std::abs(cents) <= 6.0, "dominant pitch missed the target by more than 6 cents");
      Require(deficitDb <= 3.0, "target carrier is more than 3 dB below a nearby component");
    }
  }
}

void TestEveryIntegerShift()
{
  constexpr double sampleRate = 48000.0;
  constexpr double inputFrequency = 146.832;
  constexpr int frames = 48000;
  constexpr int analysisLength = 24000;

  for (int semitones = -12; semitones <= 12; ++semitones)
  {
    dessmetal::transpose::Processor processor;
    Require(processor.Reset(sampleRate, semitones), "integer-shift reset failed");
    std::vector<double> buffer(frames);
    for (int frame = 0; frame < frames; ++frame)
      buffer[static_cast<std::size_t>(frame)] =
        0.2 * std::sin(2.0 * kPi * inputFrequency * frame / sampleRate);

    for (int offset = 0; offset < frames; offset += 257)
      processor.ProcessInPlace(buffer.data() + offset, std::min(257, frames - offset), semitones);

    const double expected = inputFrequency * std::pow(2.0, semitones / 12.0);
    const double target = ToneMagnitude(buffer, frames - analysisLength, analysisLength, expected, sampleRate);
    double strongest = 0.0;
    for (double candidate = expected * 0.95; candidate <= expected * 1.05; candidate += 0.25)
      strongest = std::max(strongest,
                           ToneMagnitude(buffer, frames - analysisLength, analysisLength, candidate, sampleRate));
    const double deficitDb = 20.0 * std::log10(std::max(strongest, 1.0e-15)
                                                / std::max(target, 1.0e-15));
    Require(target > 1.0e-5, "integer shift produced no target carrier");
    Require(deficitDb <= 3.0, "integer shift target is more than 3 dB below a nearby component");
  }
}

int MeasureHalfLevelOnset(const double frequency, const int semitones)
{
  constexpr double sampleRate = 48000.0;
  constexpr int onsetSample = 1024;
  constexpr int frameCount = 24000;
  constexpr int windowSize = 48;
  dessmetal::transpose::Processor processor;
  Require(processor.Reset(sampleRate, semitones), "onset reset failed");
  std::vector<double> input(frameCount, 0.0);
  std::vector<double> output(frameCount, 0.0);
  double phase = 0.0;
  for (int frame = onsetSample; frame < frameCount; ++frame)
  {
    const double ramp = std::min(1.0, static_cast<double>(frame - onsetSample) / 24.0);
    input[static_cast<std::size_t>(frame)] = 0.2 * ramp * std::sin(phase);
    phase += 2.0 * kPi * frequency / sampleRate;
    if (phase >= 2.0 * kPi)
      phase -= 2.0 * kPi;
  }
  for (int offset = 0; offset < frameCount; offset += 512)
    processor.Process(input.data() + offset, output.data() + offset,
                      std::min(512, frameCount - offset), semitones);

  double steadyEnergy = 0.0;
  for (int frame = frameCount - 4096; frame < frameCount; ++frame)
    steadyEnergy += output[static_cast<std::size_t>(frame)] * output[static_cast<std::size_t>(frame)];
  const double steadyRms = std::sqrt(steadyEnergy / 4096.0);
  for (int frame = onsetSample; frame <= frameCount - windowSize; ++frame)
  {
    double windowEnergy = 0.0;
    for (int index = 0; index < windowSize; ++index)
    {
      const double sample = output[static_cast<std::size_t>(frame + index)];
      windowEnergy += sample * sample;
    }
    if (std::sqrt(windowEnergy / windowSize) >= steadyRms * 0.5)
      return frame - onsetSample;
  }
  return frameCount - onsetSample;
}

void TestOnsetResponse()
{
  for (const auto& test : std::vector<std::pair<double, int>>{
         {82.4069, -12}, {82.4069, 12}, {329.628, -12}, {329.628, 12}})
  {
    const int onsetSamples = MeasureHalfLevelOnset(test.first, test.second);
    Require(onsetSamples < 480, "half-level onset response exceeded 10 ms at 48 kHz");
  }
}

std::vector<double> RenderAutomation(const std::vector<int>& blockPattern)
{
  constexpr double sampleRate = 48000.0;
  constexpr int frames = 48000;
  constexpr int change1 = 12000;
  constexpr int change2 = 24000;
  constexpr int change3 = 36000;
  dessmetal::transpose::Processor processor;
  Require(processor.Reset(sampleRate), "automation reset failed");
  std::vector<double> input(frames);
  std::vector<double> output(frames);
  for (int frame = 0; frame < frames; ++frame)
    input[static_cast<std::size_t>(frame)] = 0.18 * std::sin(2.0 * kPi * 146.832 * frame / sampleRate);

  int offset = 0;
  std::size_t patternIndex = 0;
  while (offset < frames)
  {
    const int nextChange = offset < change1 ? change1 : (offset < change2 ? change2 : (offset < change3 ? change3 : frames));
    const int requested = blockPattern[patternIndex++ % blockPattern.size()];
    const int block = std::min({requested, nextChange - offset, frames - offset});
    const int semitones = offset < change1 ? 0 : (offset < change2 ? 12 : (offset < change3 ? -12 : 0));
    processor.Process(input.data() + offset, output.data() + offset, block, semitones);
    offset += block;
  }
  return output;
}

void TestAutomation()
{
  const auto fixed = RenderAutomation({256});
  const auto irregular = RenderAutomation({1, 63, 17, 511, 5, 128});
  Require(fixed == irregular, "automation result changed with host block partitioning");

  double peak = 0.0;
  double maxStep = 0.0;
  for (std::size_t index = 1; index < fixed.size(); ++index)
  {
    Require(std::isfinite(fixed[index]), "automation produced a non-finite sample");
    peak = std::max(peak, std::abs(fixed[index]));
    maxStep = std::max(maxStep, std::abs(fixed[index] - fixed[index - 1]));
  }
  Require(peak < 2.0, "automation produced an unstable peak");
  Require(maxStep < 0.25, "automation produced a click-sized sample discontinuity");

  // The final return to zero must finish at the exact dry signal.
  constexpr int tailStart = 36000 + 512;
  for (int frame = tailStart; frame < static_cast<int>(fixed.size()); ++frame)
  {
    const double dry = 0.18 * std::sin(2.0 * kPi * 146.832 * frame / 48000.0);
    Require(fixed[static_cast<std::size_t>(frame)] == dry, "zero transition did not settle to exact dry");
  }
}

void TestRatesShiftsSilenceAndFiniteHandling()
{
  constexpr double rates[]{44100.0, 48000.0, 88200.0, 96000.0};
  for (const double sampleRate : rates)
  {
    for (int semitones = -12; semitones <= 12; ++semitones)
    {
      dessmetal::transpose::Processor processor;
      Require(processor.Reset(sampleRate, semitones), "supported sample rate reset failed");
      std::vector<double> input(4096);
      std::vector<double> output(input.size());
      for (std::size_t index = 0; index < input.size(); ++index)
      {
        const double t = static_cast<double>(index) / sampleRate;
        input[index] = 0.08 * std::sin(2.0 * kPi * 82.407 * t)
                       + 0.06 * std::sin(2.0 * kPi * 110.0 * t)
                       + 0.04 * std::sin(2.0 * kPi * 146.832 * t);
      }
      processor.Process(input.data(), output.data(), static_cast<int>(input.size()), semitones);
      double peak = 0.0;
      double energy = 0.0;
      for (const double sample : output)
      {
        Require(std::isfinite(sample), "supported rate/shift produced non-finite output");
        peak = std::max(peak, std::abs(sample));
        energy += sample * sample;
      }
      Require(peak < 4.0, "supported rate/shift produced an unstable peak");
      Require(energy > 1.0e-12, "supported rate/shift produced silent output");
    }
  }

  dessmetal::transpose::Processor silenceProcessor;
  Require(silenceProcessor.Reset(48000.0, 12), "silence reset failed");
  std::vector<double> silence(48000, 0.0);
  silenceProcessor.ProcessInPlace(silence.data(), static_cast<int>(silence.size()), 12);
  for (const double sample : silence)
    Require(sample == 0.0, "silence generated output");

  dessmetal::transpose::Processor finiteProcessor;
  Require(finiteProcessor.Reset(48000.0, 7), "finite reset failed");
  std::vector<double> invalid{0.0, std::numeric_limits<double>::quiet_NaN(),
                              std::numeric_limits<double>::infinity(),
                              -std::numeric_limits<double>::infinity(), 0.1};
  finiteProcessor.ProcessInPlace(invalid.data(), static_cast<int>(invalid.size()), 7);
  for (const double sample : invalid)
    Require(std::isfinite(sample), "invalid input escaped as non-finite output");
}

void TestRenderPathAllocations()
{
  dessmetal::transpose::Processor processor;
  Require(processor.Reset(48000.0), "allocation-test reset failed");
  std::vector<double> buffer(512, 0.1);
  gAllocationCount.store(0, std::memory_order_relaxed);
  gTrackAllocations.store(true, std::memory_order_release);
  for (int block = 0; block < 100; ++block)
    processor.ProcessInPlace(buffer.data(), static_cast<int>(buffer.size()), (block % 25) - 12);
  gTrackAllocations.store(false, std::memory_order_release);
  Require(gAllocationCount.load(std::memory_order_relaxed) == 0, "render path allocated memory");
}
} // namespace

void* operator new(const std::size_t size)
{
  if (gTrackAllocations.load(std::memory_order_acquire))
    gAllocationCount.fetch_add(1, std::memory_order_relaxed);
  if (void* memory = std::malloc(size))
    return memory;
  throw std::bad_alloc();
}

void* operator new[](const std::size_t size)
{
  return ::operator new(size);
}

void operator delete(void* memory) noexcept
{
  std::free(memory);
}

void operator delete[](void* memory) noexcept
{
  std::free(memory);
}

void operator delete(void* memory, std::size_t) noexcept
{
  std::free(memory);
}

void operator delete[](void* memory, std::size_t) noexcept
{
  std::free(memory);
}

int main()
{
  try
  {
    TestConfigurationAndBypass();
    TestPitchAccuracyAndBlockInvariance();
    TestEveryIntegerShift();
    TestOnsetResponse();
    TestAutomation();
    TestRatesShiftsSilenceAndFiniteHandling();
    TestRenderPathAllocations();
  }
  catch (const std::exception& error)
  {
    std::cerr << "DessMetal transpose test failed: " << error.what() << '\n';
    return 1;
  }

  std::cout << "DessMetal transpose processor tests passed\n";
  return 0;
}

#include "ErbPitchPrototype.h"
#include "HybridErbPitchPrototype.h"
#include "../../TransposeProcessor.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
using Prototype = dessmetal::experiments::transpose::ErbPitchPrototype;
using HybridPrototype = dessmetal::experiments::transpose::HybridErbPitchPrototype;
using ProductionProcessor = dessmetal::transpose::Processor;

void Require(const bool condition, const char* message)
{
  if (!condition)
    throw std::runtime_error(message);
}

std::vector<double> ReadRawDoubles(const std::string& path)
{
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  Require(input.good(), "could not open raw input");
  const std::streamsize byteCount = input.tellg();
  Require(byteCount >= 0 && byteCount % static_cast<std::streamsize>(sizeof(double)) == 0,
          "raw input size is invalid");
  input.seekg(0);
  std::vector<double> samples(static_cast<std::size_t>(byteCount / sizeof(double)));
  input.read(reinterpret_cast<char*>(samples.data()), byteCount);
  Require(input.good(), "could not read raw input");
  return samples;
}

void WriteRawDoubles(const std::string& path, const std::vector<double>& samples)
{
  std::ofstream output(path, std::ios::binary);
  Require(output.good(), "could not open raw output");
  output.write(reinterpret_cast<const char*>(samples.data()),
               static_cast<std::streamsize>(samples.size() * sizeof(double)));
  Require(output.good(), "could not write raw output");
}
} // namespace

int main(int argc, char** argv)
{
  try
  {
    if (argc < 5 || argc > 10)
      throw std::runtime_error(
        "usage: render_erb_file input.f64le output.f64le sample-rate semitones "
        "[bandwidth-scale [low-bandwidth-scale [transition-hz [dominance-exponent [engine]]]]]");

    const std::string inputPath = argv[1];
    const std::string outputPath = argv[2];
    const double sampleRate = std::stod(argv[3]);
    const int semitones = std::stoi(argv[4]);
    const double bandwidthScale = argc >= 6 ? std::stod(argv[5]) : 1.0;
    const double lowBandwidthScale = argc >= 7 ? std::stod(argv[6]) : 0.0;
    const double transitionHz = argc >= 8 ? std::stod(argv[7]) : 300.0;
    const int dominanceExponent = argc >= 9 ? std::stoi(argv[8]) : 0;
    const std::string engine = argc >= 10 ? argv[9] : "erb";
    Require(engine == "erb" || engine == "hybrid" || engine == "production",
            "engine must be erb, hybrid, or production");

    auto input = ReadRawDoubles(inputPath);
    std::vector<double> output(input.size(), 0.0);
    Prototype processor;
    HybridPrototype hybridProcessor;
    ProductionProcessor productionProcessor;
    constexpr int blockSize = 512;
    const bool reset = engine == "production"
                         ? productionProcessor.Reset(sampleRate, semitones)
                         : (engine == "hybrid"
                              ? hybridProcessor.Reset(sampleRate, blockSize,
                                                      Prototype::MathMode::kBalancedApproximation)
                              : processor.Reset(sampleRate, blockSize,
                                                Prototype::MathMode::kBalancedApproximation,
                                                Prototype::kDefaultBandCount, 19.0, bandwidthScale,
                                                lowBandwidthScale, transitionHz, false,
                                                dominanceExponent));
    Require(reset, "processor reset failed");
    for (std::size_t offset = 0; offset < input.size(); offset += blockSize)
    {
      const int frames = static_cast<int>(std::min<std::size_t>(blockSize, input.size() - offset));
      if (engine == "production")
        productionProcessor.Process(input.data() + offset, output.data() + offset, frames, semitones);
      else if (engine == "hybrid")
        hybridProcessor.Process(input.data() + offset, output.data() + offset, frames, semitones);
      else
        processor.Process(input.data() + offset, output.data() + offset, frames, semitones);
    }
    Require(std::all_of(output.begin(), output.end(), [](const double sample) { return std::isfinite(sample); }),
            "processor produced a non-finite output");
    WriteRawDoubles(outputPath, output);
    std::cout << "rendered_samples=" << output.size() << " sample_rate=" << sampleRate
              << " semitones=" << semitones << " bandwidth_scale=" << bandwidthScale
              << " low_bandwidth_scale=" << lowBandwidthScale << " transition_hz=" << transitionHz
              << " dominance_exponent=" << dominanceExponent << " engine=" << engine << '\n';
    return 0;
  }
  catch (const std::exception& error)
  {
    std::cerr << "render error: " << error.what() << '\n';
    return 1;
  }
}

#include <AudioToolbox/AudioToolbox.h>

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
void Require(const bool condition, const char* message)
{
  if (!condition)
    throw std::runtime_error(message);
}

void Check(const OSStatus status, const char* operation)
{
  if (status != noErr)
    throw std::runtime_error(std::string(operation) + " failed with OSStatus " + std::to_string(status));
}

std::vector<float> ReadRawFloats(const std::string& path)
{
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  Require(input.good(), "could not open raw input");
  const std::streamsize byteCount = input.tellg();
  Require(byteCount >= 0 && byteCount % static_cast<std::streamsize>(sizeof(float)) == 0,
          "raw input size is invalid");
  input.seekg(0);
  std::vector<float> samples(static_cast<std::size_t>(byteCount / sizeof(float)));
  input.read(reinterpret_cast<char*>(samples.data()), byteCount);
  Require(input.good(), "could not read raw input");
  return samples;
}

void WriteRawFloats(const std::string& path, const std::vector<float>& samples)
{
  std::ofstream output(path, std::ios::binary);
  Require(output.good(), "could not open raw output");
  output.write(reinterpret_cast<const char*>(samples.data()),
               static_cast<std::streamsize>(samples.size() * sizeof(float)));
  Require(output.good(), "could not write raw output");
}

struct InputContext
{
  const std::vector<float>* samples = nullptr;
  std::size_t callbackCount = 0;
  float peak = 0.0f;
};

OSStatus InputCallback(void* userData, AudioUnitRenderActionFlags* flags,
                       const AudioTimeStamp* timestamp, UInt32, UInt32 frameCount,
                       AudioBufferList* buffers)
{
  auto& context = *static_cast<InputContext*>(userData);
  ++context.callbackCount;
  const std::size_t start = timestamp->mSampleTime >= 0.0
                              ? static_cast<std::size_t>(timestamp->mSampleTime)
                              : 0;
  bool silent = true;
  for (UInt32 bufferIndex = 0; bufferIndex < buffers->mNumberBuffers; ++bufferIndex)
  {
    auto* destination = static_cast<float*>(buffers->mBuffers[bufferIndex].mData);
    for (UInt32 frame = 0; frame < frameCount; ++frame)
    {
      const std::size_t sourceIndex = start + frame;
      const float sample = sourceIndex < context.samples->size() ? (*context.samples)[sourceIndex] : 0.0f;
      destination[frame] = sample;
      context.peak = std::max(context.peak, std::abs(sample));
      silent = silent && sample == 0.0f;
    }
    buffers->mBuffers[bufferIndex].mDataByteSize = frameCount * sizeof(float);
  }
  if (silent)
    *flags |= kAudioUnitRenderAction_OutputIsSilence;
  return noErr;
}

struct StereoBufferList
{
  UInt32 numberBuffers = 2;
  AudioBuffer buffers[2]{};
};
} // namespace

int main(int argc, char** argv)
{
  AudioUnit unit = nullptr;
  try
  {
    if (argc != 5 && argc != 6)
      throw std::runtime_error(
        "usage: render_reference_au input.f32le output.f32le sample-rate semitones [section-mask]");
    const std::string inputPath = argv[1];
    const std::string outputPath = argv[2];
    const double sampleRate = std::stod(argv[3]);
    const int semitones = std::stoi(argv[4]);
    const int sectionMask = argc == 6 ? std::stoi(argv[5]) : 31;
    Require(semitones >= -12 && semitones <= 12, "semitones must be between -12 and +12");
    auto input = ReadRawFloats(inputPath);
    std::vector<float> output(input.size(), 0.0f);

    AudioComponentDescription description{};
    description.componentType = 'aumf';
    description.componentSubType = 'FNMX';
    description.componentManufacturer = 'NDSP';
    AudioComponent component = AudioComponentFindNext(nullptr, &description);
    Require(component != nullptr, "Fortin Nameless Suite X Audio Unit was not found");
    Check(AudioComponentInstanceNew(component, &unit), "AudioComponentInstanceNew");

    constexpr UInt32 blockSize = 512;
    UInt32 maximumFrames = blockSize;
    Check(AudioUnitSetProperty(unit, kAudioUnitProperty_MaximumFramesPerSlice,
                               kAudioUnitScope_Global, 0, &maximumFrames, sizeof(maximumFrames)),
          "set maximum frames");

    AudioStreamBasicDescription format{};
    format.mSampleRate = sampleRate;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked
                          | kAudioFormatFlagIsNonInterleaved | kAudioFormatFlagsNativeEndian;
    format.mBytesPerPacket = sizeof(float);
    format.mFramesPerPacket = 1;
    format.mBytesPerFrame = sizeof(float);
    format.mChannelsPerFrame = 2;
    format.mBitsPerChannel = 8 * sizeof(float);
    Check(AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat,
                               kAudioUnitScope_Input, 0, &format, sizeof(format)),
          "set input format");
    Check(AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat,
                               kAudioUnitScope_Output, 0, &format, sizeof(format)),
          "set output format");

    InputContext inputContext{&input};
    AURenderCallbackStruct callback{InputCallback, &inputContext};
    Check(AudioUnitSetProperty(unit, kAudioUnitProperty_SetRenderCallback,
                               kAudioUnitScope_Input, 0, &callback, sizeof(callback)),
          "set input callback");

    // Leave only the input transpose module active. These are public Audio Unit
    // parameters observed from the installed reference plug-in; no proprietary
    // code or state is read or copied.
    const std::vector<std::pair<AudioUnitParameterID, AudioUnitParameterValue>> settings{
      {0, 0.5f},  // Input Gain (unity)
      {1, 0.5f},  // Output Gain (unity)
      {2, 0.0f},  // Gate
      {4, static_cast<float>(semitones + 12) / 24.0f},
      {5, 0.0f},  // Doubler
      {7, (sectionMask & 1) != 0 ? 1.0f : 0.0f},
      {16, (sectionMask & 2) != 0 ? 1.0f : 0.0f},
      {26, (sectionMask & 4) != 0 ? 1.0f : 0.0f},
      {47, (sectionMask & 8) != 0 ? 1.0f : 0.0f},
      {59, (sectionMask & 16) != 0 ? 1.0f : 0.0f}
    };
    Check(AudioUnitInitialize(unit), "AudioUnitInitialize");
    for (const auto [identifier, value] : settings)
      Check(AudioUnitSetParameter(unit, identifier, kAudioUnitScope_Global, 0, value, 0), "set parameter");
    std::vector<float> left(blockSize, 0.0f);
    std::vector<float> right(blockSize, 0.0f);
    for (std::size_t offset = 0; offset < output.size(); offset += blockSize)
    {
      const UInt32 frames = static_cast<UInt32>(std::min<std::size_t>(blockSize, output.size() - offset));
      StereoBufferList storage;
      storage.buffers[0] = {1, frames * static_cast<UInt32>(sizeof(float)), left.data()};
      storage.buffers[1] = {1, frames * static_cast<UInt32>(sizeof(float)), right.data()};
      AudioTimeStamp timestamp{};
      timestamp.mFlags = kAudioTimeStampSampleTimeValid;
      timestamp.mSampleTime = static_cast<Float64>(offset);
      AudioUnitRenderActionFlags flags = 0;
      Check(AudioUnitRender(unit, &flags, &timestamp, 0, frames,
                            reinterpret_cast<AudioBufferList*>(&storage)),
            "AudioUnitRender");
      std::copy(left.begin(), left.begin() + frames, output.begin() + static_cast<std::ptrdiff_t>(offset));
    }

    Float64 reportedLatencySeconds = 0.0;
    UInt32 latencySize = sizeof(reportedLatencySeconds);
    Check(AudioUnitGetProperty(unit, kAudioUnitProperty_Latency, kAudioUnitScope_Global, 0,
                               &reportedLatencySeconds, &latencySize),
          "get latency");
    float outputPeak = 0.0f;
    for (const float sample : output)
      outputPeak = std::max(outputPeak, std::abs(sample));
    AudioUnitParameterValue transposeReadback = 0.0f;
    Check(AudioUnitGetParameter(unit, 4, kAudioUnitScope_Global, 0, &transposeReadback),
          "get transpose parameter");
    WriteRawFloats(outputPath, output);
    std::cout << "rendered_samples=" << output.size() << " sample_rate=" << sampleRate
              << " semitones=" << semitones
              << " section_mask=" << sectionMask
              << " reported_latency_samples=" << reportedLatencySeconds * sampleRate
              << " callbacks=" << inputContext.callbackCount << " input_peak=" << inputContext.peak
              << " output_peak=" << outputPeak << " transpose_readback=" << transposeReadback << '\n';
    AudioUnitUninitialize(unit);
    AudioComponentInstanceDispose(unit);
    return 0;
  }
  catch (const std::exception& error)
  {
    if (unit != nullptr)
      AudioComponentInstanceDispose(unit);
    std::cerr << "reference render error: " << error.what() << '\n';
    return 1;
  }
}

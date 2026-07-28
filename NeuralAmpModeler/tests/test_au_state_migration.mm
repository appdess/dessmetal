#include <AudioToolbox/AudioToolbox.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace
{
constexpr UInt32 kDessMetalSubtype = '1YEo';
constexpr UInt32 kDessMetalManufacturer = 'AdMs';
constexpr std::size_t kCurrentParameterCount = 20;

template <typename T>
void AppendNative(std::vector<std::uint8_t>& bytes, const T& value)
{
  const auto offset = bytes.size();
  bytes.resize(offset + sizeof(T));
  std::memcpy(bytes.data() + offset, &value, sizeof(T));
}

void AppendString(std::vector<std::uint8_t>& bytes, const std::string& value)
{
  const auto length = static_cast<std::int32_t>(value.size());
  AppendNative(bytes, length);
  bytes.insert(bytes.end(), value.begin(), value.end());
}

std::vector<std::uint8_t> MakeStateChunk(const std::string& version, const std::vector<double>& parameters)
{
  std::vector<std::uint8_t> bytes;
  AppendString(bytes, "###NeuralAmpModeler###");
  AppendString(bytes, version);
  AppendString(bytes, ""); // Historical NAMPath, intentionally no user data.
  AppendString(bytes, ""); // Historical IRPath, intentionally no user data.
  for (const double parameter : parameters)
    AppendNative(bytes, parameter);
  return bytes;
}

CFStringRef MakeString(const char* value)
{
  return CFStringCreateWithCString(kCFAllocatorDefault, value, kCFStringEncodingUTF8);
}

void SetDictionaryNumber(CFMutableDictionaryRef dictionary, const char* key, const SInt32 value)
{
  CFStringRef keyString = MakeString(key);
  CFNumberRef number = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &value);
  CFDictionarySetValue(dictionary, keyString, number);
  CFRelease(number);
  CFRelease(keyString);
}

void SetDictionaryString(CFMutableDictionaryRef dictionary, const char* key, const char* value)
{
  CFStringRef keyString = MakeString(key);
  CFStringRef valueString = MakeString(value);
  CFDictionarySetValue(dictionary, keyString, valueString);
  CFRelease(valueString);
  CFRelease(keyString);
}

void SetDictionaryData(CFMutableDictionaryRef dictionary, const char* key, const std::vector<std::uint8_t>& bytes)
{
  CFStringRef keyString = MakeString(key);
  CFDataRef data = CFDataCreate(kCFAllocatorDefault, bytes.data(), static_cast<CFIndex>(bytes.size()));
  CFDictionarySetValue(dictionary, keyString, data);
  CFRelease(data);
  CFRelease(keyString);
}

OSStatus ApplyState(AudioUnit unit, const std::string& version, const std::vector<double>& parameters)
{
  const auto bytes = MakeStateChunk(version, parameters);
  CFMutableDictionaryRef dictionary = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
                                                                 &kCFTypeDictionaryKeyCallBacks,
                                                                 &kCFTypeDictionaryValueCallBacks);
  SetDictionaryNumber(dictionary, kAUPresetVersionKey, 0x000101);
  SetDictionaryNumber(dictionary, kAUPresetTypeKey, static_cast<SInt32>(kAudioUnitType_Effect));
  SetDictionaryNumber(dictionary, kAUPresetSubtypeKey, static_cast<SInt32>(kDessMetalSubtype));
  SetDictionaryNumber(dictionary, kAUPresetManufacturerKey, static_cast<SInt32>(kDessMetalManufacturer));
  // Exercise the real factory-preset name. The AU wrapper restores this preset
  // before parsing and must roll it back if the state is rejected.
  SetDictionaryString(dictionary, kAUPresetNameKey, "Empty");
  SetDictionaryData(dictionary, kAUPresetDataKey, bytes);

  CFPropertyListRef propertyList = dictionary;
  const OSStatus status = AudioUnitSetProperty(unit, kAudioUnitProperty_ClassInfo, kAudioUnitScope_Global, 0,
                                               &propertyList, sizeof(propertyList));
  CFRelease(dictionary);
  return status;
}

bool ExpectParameters(AudioUnit unit, const std::array<double, kCurrentParameterCount>& expected,
                      const char* fixtureName)
{
  for (AudioUnitParameterID parameter = 0; parameter < expected.size(); ++parameter)
  {
    AudioUnitParameterValue actual = 0.0f;
    const OSStatus status =
      AudioUnitGetParameter(unit, parameter, kAudioUnitScope_Global, 0, &actual);
    if (status != noErr || std::abs(static_cast<double>(actual) - expected[parameter]) > 1.0e-4)
    {
      std::cerr << fixtureName << " parameter " << parameter << " mismatch: expected " << expected[parameter]
                << ", got " << actual << ", OSStatus " << status << '\n';
      return false;
    }
  }
  return true;
}

bool ExpectAppliedState(AudioUnit unit, const std::string& version, const std::vector<double>& parameters,
                        const std::array<double, kCurrentParameterCount>& expected, const char* fixtureName)
{
  const OSStatus status = ApplyState(unit, version, parameters);
  if (status != noErr)
  {
    std::cerr << fixtureName << " was rejected with OSStatus " << status << '\n';
    return false;
  }
  return ExpectParameters(unit, expected, fixtureName);
}

bool ExpectRejectedState(AudioUnit unit, const std::string& version, const std::vector<double>& parameters,
                         const std::array<double, kCurrentParameterCount>& unchanged,
                         const char* fixtureName)
{
  const OSStatus status = ApplyState(unit, version, parameters);
  if (status != kAudioUnitErr_InvalidPropertyValue)
  {
    std::cerr << fixtureName << " returned OSStatus " << status << ", expected "
              << kAudioUnitErr_InvalidPropertyValue << '\n';
    return false;
  }
  return ExpectParameters(unit, unchanged, fixtureName);
}
} // namespace

int main()
{
  AudioComponentDescription description{};
  description.componentType = kAudioUnitType_Effect;
  description.componentSubType = kDessMetalSubtype;
  description.componentManufacturer = kDessMetalManufacturer;

  const AudioComponent component = AudioComponentFindNext(nullptr, &description);
  if (component == nullptr)
  {
    std::cerr << "DessMetal AU is not registered\n";
    return 1;
  }

  AudioUnit unit = nullptr;
  const OSStatus createStatus = AudioComponentInstanceNew(component, &unit);
  if (createStatus != noErr || unit == nullptr)
  {
    std::cerr << "AudioComponentInstanceNew failed with OSStatus " << createStatus << '\n';
    return 1;
  }

  // Exact f55d020 positional shape. Gain 10 deliberately verifies clamping to
  // the current trained maximum 8. Legacy DessBlock index 1 migrates to Green
  // at stable current host index 2. The absent drive stage remains disabled.
  const std::vector<double> legacy010{
    -3.0, 10.0, -45.0, 2.0, 3.0, 4.0, -2.0, 0.0, 1.0, 0.0, 0.0, 10.0, 0.0, 1.0,
  };
  const std::array<double, kCurrentParameterCount> expectedLegacy{
    -3.0, 8.0, -45.0, 2.0, 3.0, 4.0, -2.0, 0.0, 5.0, 0.0,
    1.0, 0.0, 5.0, 0.0, 0.0, 0.0, 10.0, 0.0, 2.0, 1.0,
  };

  // Existing 20-value states were also mislabeled 0.1.0 and must remain exact.
  const std::vector<double> currentTagged010{
    -1.0, 7.0, -55.0, 6.0, 5.0, 4.0, 1.0, -4.0, 8.0, 1.0,
    0.0, 1.0, 7.0, 1.0, 3.0, 1.0, 11.0, 0.0, 4.0, 0.0,
  };
  const std::array<double, kCurrentParameterCount> expectedCurrent010{
    -1.0, 7.0, -55.0, 6.0, 5.0, 4.0, 1.0, -4.0, 8.0, 1.0,
    0.0, 1.0, 7.0, 1.0, 3.0, 1.0, 11.0, 0.0, 4.0, 0.0,
  };

  // 0.1.1 establishes the unambiguous current layout.
  const std::vector<double> current011{
    2.0, 3.0, -65.0, 4.0, 5.0, 6.0, -3.0, 2.0, 1.0, 0.0,
    1.0, 0.0, 4.0, 0.0, 2.0, 0.0, 12.0, 0.0, 0.0, 1.0,
  };
  const std::array<double, kCurrentParameterCount> expectedCurrent011{
    2.0, 3.0, -65.0, 4.0, 5.0, 6.0, -3.0, 2.0, 1.0, 0.0,
    1.0, 0.0, 4.0, 0.0, 2.0, 0.0, 12.0, 0.0, 0.0, 1.0,
  };

  // Unsupported historical custom slots depended on path-based loading, which
  // no longer exists. The recognized 18-value WIP layout used incompatible
  // drive semantics. Both must be rejected without changing any parameter.
  auto unsupportedLegacyCustom = legacy010;
  unsupportedLegacyCustom.back() = 2.0;
  const std::vector<double> unsupportedWip18(currentTagged010.begin(), currentTagged010.begin() + 18);

  const bool passed = ExpectAppliedState(unit, "0.1.0", legacy010, expectedLegacy, "legacy 14-value 0.1.0")
                      && ExpectAppliedState(unit, "0.1.0", currentTagged010, expectedCurrent010,
                                            "current 20-value 0.1.0")
                      && ExpectAppliedState(unit, "0.1.1", current011, expectedCurrent011,
                                            "current 20-value 0.1.1")
                      && ExpectRejectedState(unit, "0.1.0", unsupportedLegacyCustom, expectedCurrent011,
                                             "unsupported legacy custom amp")
                      && ExpectRejectedState(unit, "0.1.0", unsupportedWip18, expectedCurrent011,
                                             "unsupported 18-value WIP layout");

  AudioComponentInstanceDispose(unit);
  if (!passed)
    return 1;

  std::cout << "DessMetal AU state migration passed: legacy14, current20@0.1.0, current20@0.1.1, "
               "atomic rejection for legacy custom and WIP18\n";
  return 0;
}

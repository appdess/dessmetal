#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace dessmetal::state
{
// Commit f55d0201 serialized these values positionally as DessMetal 0.1.0.
// The last destination name is the current name of the historical "Amp Switch"
// parameter; parameter names themselves are not present in an IByteChunk.
inline constexpr std::array<std::string_view, 14> kLegacy010ParameterNames{
  "Input",
  "Gain",
  "Threshold",
  "Bass",
  "Middle",
  "Treble",
  "Output",
  "NoiseGateActive",
  "ToneStack",
  "IRToggle",
  "CalibrateInput",
  "InputCalibrationLevel",
  "OutputMode",
  "Amp Model",
};

// The current order must stay identical to EParams/SerializeParams.
inline constexpr std::array<std::string_view, 20> kCurrent01ParameterNames{
  "Input",
  "Gain",
  "Threshold",
  "Bass",
  "Middle",
  "Treble",
  "Output",
  "Boost Level",
  "Boost Tone",
  "NoiseGateActive",
  "ToneStack",
  "Boost",
  "BoostOutput",
  "IRToggle",
  "Boost Model",
  "CalibrateInput",
  "InputCalibrationLevel",
  "OutputMode",
  "Amp Model",
  "Amp Enabled",
};

// DessMetal 0.2 appends Transpose without changing any established host
// parameter identifier. New parameters must continue to be appended here and
// in EParams so positional state remains deterministic.
inline constexpr std::array<std::string_view, 21> kCurrent02ParameterNames{
  "Input",
  "Gain",
  "Threshold",
  "Bass",
  "Middle",
  "Treble",
  "Output",
  "Boost Level",
  "Boost Tone",
  "NoiseGateActive",
  "ToneStack",
  "Boost",
  "BoostOutput",
  "IRToggle",
  "Boost Model",
  "CalibrateInput",
  "InputCalibrationLevel",
  "OutputMode",
  "Amp Model",
  "Amp Enabled",
  "Transpose",
};

struct NamedDefault
{
  std::string_view name;
  double value;
};

// A legacy project had no drive stage or amp-bypass parameter. Keep the newly
// introduced drive out of its signal path, keep the amp active, and initialize
// the remaining host-visible compatibility parameters deterministically.
inline constexpr std::array<NamedDefault, 6> kLegacy010AddedParameterDefaults{
  NamedDefault{"Boost Level", 0.0},
  NamedDefault{"Boost Tone", 5.0},
  NamedDefault{"Boost", 0.0},
  NamedDefault{"BoostOutput", 5.0},
  NamedDefault{"Boost Model", 0.0},
  NamedDefault{"Amp Enabled", 1.0},
};

enum class ParameterLayout01
{
  Invalid,
  Legacy010_14,
  UnsupportedWip18,
  UnsupportedWip19,
  Current20,
};

enum class ParameterLayout02
{
  Invalid,
  Current21,
};

inline constexpr std::size_t ParameterBytes(const std::size_t parameterCount)
{
  return parameterCount * sizeof(double);
}

// iPlug2's VST3 wrapper appends its own int32 bypass value after the plug-in
// state and expects UnserializeState() to return the position just before it.
inline constexpr std::size_t kVST3BypassSuffixBytes = sizeof(std::int32_t);

inline constexpr bool MatchesParameterPayload(const std::size_t remainingBytes, const std::size_t parameterCount,
                                              const bool allowVST3BypassSuffix)
{
  const auto bytes = ParameterBytes(parameterCount);
  return remainingBytes == bytes
         || (allowVST3BypassSuffix && remainingBytes == bytes + kVST3BypassSuffixBytes);
}

inline constexpr ParameterLayout01 DetectParameterLayout01(const std::size_t remainingBytes,
                                                            const bool allowLegacy010,
                                                            const bool allowVST3BypassSuffix)
{
  if (MatchesParameterPayload(remainingBytes, kCurrent01ParameterNames.size(), allowVST3BypassSuffix))
    return ParameterLayout01::Current20;
  if (allowLegacy010
      && MatchesParameterPayload(remainingBytes, kLegacy010ParameterNames.size(), allowVST3BypassSuffix))
    return ParameterLayout01::Legacy010_14;
  if (allowLegacy010 && MatchesParameterPayload(remainingBytes, 18, allowVST3BypassSuffix))
    return ParameterLayout01::UnsupportedWip18;
  if (allowLegacy010 && MatchesParameterPayload(remainingBytes, 19, allowVST3BypassSuffix))
    return ParameterLayout01::UnsupportedWip19;
  return ParameterLayout01::Invalid;
}

inline constexpr ParameterLayout02 DetectParameterLayout02(const std::size_t remainingBytes,
                                                            const bool allowVST3BypassSuffix)
{
  return MatchesParameterPayload(remainingBytes, kCurrent02ParameterNames.size(), allowVST3BypassSuffix)
           ? ParameterLayout02::Current21
           : ParameterLayout02::Invalid;
}

// The original DessMetal enum was DessTortion=0, DessBlock=1. The current
// stable host enum keeps DessTortion-blue at 0 and DessBlock-green at 2.
inline constexpr bool IsSupportedLegacy010AmpModel(const double legacyValue)
{
  return legacyValue == 0.0 || legacyValue == 1.0;
}

inline constexpr double MigrateLegacy010AmpModel(const double legacyValue)
{
  return legacyValue == 1.0 ? 2.0 : legacyValue;
}
} // namespace dessmetal::state

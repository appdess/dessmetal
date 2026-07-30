#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

#include "dsp.h"
#include "registry.h"
#include "json.hpp"
#include "lstm.h"
#include "model_validation.h"
#include "convnet.h"
#include "wavenet.h"
#include "get_dsp.h"

namespace nam
{
namespace
{
constexpr int kMaxNestedModelDepth = 4;
thread_local int gNestedModelDepth = 0;

class ModelDepthGuard
{
public:
  ModelDepthGuard()
  {
    if (gNestedModelDepth >= kMaxNestedModelDepth)
      throw std::invalid_argument("Nested condition DSP depth exceeds the supported limit");
    ++gNestedModelDepth;
  }

  ~ModelDepthGuard() { --gNestedModelDepth; }
};

} // namespace

struct Version
{
  int major;
  int minor;
  int patch;
};

Version ParseVersion(const std::string& versionStr)
{
  Version version;

  // Split the version string into major, minor, and patch components
  std::stringstream ss(versionStr);
  std::string majorStr, minorStr, patchStr;
  std::getline(ss, majorStr, '.');
  std::getline(ss, minorStr, '.');
  std::getline(ss, patchStr);

  // Parse the components as integers and assign them to the version struct
  try
  {
    version.major = std::stoi(majorStr);
    version.minor = std::stoi(minorStr);
    version.patch = std::stoi(patchStr);
  }
  catch (const std::invalid_argument&)
  {
    throw std::invalid_argument("Invalid version string: " + versionStr);
  }
  catch (const std::out_of_range&)
  {
    throw std::out_of_range("Version string out of range: " + versionStr);
  }

  // Validate the semver components
  if (version.major < 0 || version.minor < 0 || version.patch < 0)
  {
    throw std::invalid_argument("Negative version component: " + versionStr);
  }
  return version;
}

void verify_config_version(const std::string versionStr)
{
  Version version = ParseVersion(versionStr);
  if (version.major != 0 || version.minor != 5)
  {
    std::stringstream ss;
    ss << "Model config is an unsupported version " << versionStr
       << ". Try either converting the model to a more recent version, or "
          "update your version of the NAM plugin.";
    throw std::runtime_error(ss.str());
  }
}

std::vector<float> GetWeights(nlohmann::json const& j)
{
  auto it = j.find("weights");
  if (it == j.end())
    throw std::runtime_error("Corrupted model file is missing weights.");
  if (!it->is_array())
    throw std::invalid_argument("Model weights must be an array");

  std::vector<float> weights;
  weights.reserve(it->size());
  for (const auto& weight : *it)
    weights.push_back(model_validation::json_float(weight, "Model weight"));
  return weights;
}

std::unique_ptr<DSP> get_dsp(const std::filesystem::path config_filename)
{
  dspData temp;
  return get_dsp(config_filename, temp);
}

std::unique_ptr<DSP> get_dsp(const nlohmann::json& config)
{
  dspData temp;
  return get_dsp(config, temp);
}

std::unique_ptr<DSP> get_dsp(const std::filesystem::path config_filename, dspData& returnedConfig)
{
  if (!std::filesystem::exists(config_filename))
    throw std::runtime_error("Config file doesn't exist!\n");
  std::ifstream i(config_filename);
  nlohmann::json j;
  i >> j;
  get_dsp(j, returnedConfig);

  /*Copy to a new dsp_config object for get_dsp below,
   since not sure if weights actually get modified as being non-const references on some
   model constructors inside get_dsp(dsp_config& conf).
   We need to return unmodified version of dsp_config via returnedConfig.*/
  dspData conf = returnedConfig;

  return get_dsp(conf);
}

std::unique_ptr<DSP> get_dsp(const nlohmann::json& config, dspData& returnedConfig)
{
  const std::string version = config.at("version").get<std::string>();
  const std::string architecture = config.at("architecture").get<std::string>();
  nlohmann::json config_json = config.at("config");
  verify_config_version(version);

  std::vector<float> weights = GetWeights(config);

  // Assign values to returnedConfig
  returnedConfig.version = version;
  returnedConfig.architecture = architecture;
  returnedConfig.config = config_json;
  const auto metadata = config.find("metadata");
  returnedConfig.metadata = metadata == config.end() ? nlohmann::json(nullptr) : *metadata;
  if (!returnedConfig.metadata.is_null() && !returnedConfig.metadata.is_object())
    throw std::invalid_argument("Model metadata must be an object or null");
  returnedConfig.weights = weights;
  returnedConfig.expected_sample_rate = nam::get_sample_rate_from_nam_file(config);

  /*Copy to a new dsp_config object for get_dsp below,
   since not sure if weights actually get modified as being non-const references on some
   model constructors inside get_dsp(dsp_config& conf).
   We need to return unmodified version of dsp_config via returnedConfig.*/
  dspData conf = returnedConfig;

  return get_dsp(conf);
}

struct OptionalValue
{
  bool have = false;
  double value = 0.0;
};

std::unique_ptr<DSP> get_dsp(dspData& conf)
{
  ModelDepthGuard model_depth_guard;
  verify_config_version(conf.version);

  auto& architecture = conf.architecture;
  nlohmann::json& config = conf.config;
  std::vector<float>& weights = conf.weights;
  OptionalValue loudness, inputLevel, outputLevel;

  auto AssignOptional = [&conf](const std::string key, OptionalValue& v) {
    if (conf.metadata.find(key) != conf.metadata.end())
    {
      if (!conf.metadata[key].is_null())
      {
        v.value = conf.metadata[key];
        v.have = true;
      }
    }
  };

  if (!conf.metadata.is_null())
  {
    AssignOptional("loudness", loudness);
    AssignOptional("input_level_dbu", inputLevel);
    AssignOptional("output_level_dbu", outputLevel);
  }
  const double expectedSampleRate = conf.expected_sample_rate;

  // Initialize using registry-based factory
  std::unique_ptr<DSP> out =
    nam::factory::FactoryRegistry::instance().create(architecture, config, weights, expectedSampleRate);

  if (loudness.have)
  {
    out->SetLoudness(loudness.value);
  }
  if (inputLevel.have)
  {
    out->SetInputLevel(inputLevel.value);
  }
  if (outputLevel.have)
  {
    out->SetOutputLevel(outputLevel.value);
  }

  // "pre-warm" the model to settle initial conditions
  // Can this be removed now that it's part of Reset()?
  out->prewarm();

  return out;
}

double get_sample_rate_from_nam_file(const nlohmann::json& j)
{
  const auto sample_rate = j.find("sample_rate");
  if (sample_rate == j.end() || sample_rate->is_null())
    return NAM_UNKNOWN_EXPECTED_SAMPLE_RATE;
  if (!sample_rate->is_number())
    throw std::invalid_argument("Model sample_rate must be numeric or null");
  return sample_rate->get<double>();
}

}; // namespace nam

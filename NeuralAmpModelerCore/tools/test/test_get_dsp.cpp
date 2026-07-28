#include <algorithm>
#include <cassert>
#include <cmath>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "json.hpp"

#include "NAM/get_dsp.h"

namespace test_get_dsp
{
// Config
const std::string basicConfigStr =
  R"({"version": "0.5.4", "metadata": {"date": {"year": 2024, "month": 10, "day": 9, "hour": 18, "minute": 44, "second": 41}, "loudness": -37.8406867980957, "gain": 0.13508800804658277, "name": "Test LSTM", "modeled_by": "Steve", "gear_type": "amp", "gear_make": "Darkglass Electronics", "gear_model": "Microtubes 900 v2", "tone_type": "clean", "input_level_dbu": 18.3, "output_level_dbu": 12.3, "training": {"settings": {"ignore_checks": false}, "data": {"latency": {"manual": null, "calibration": {"algorithm_version": 1, "delays": [-16], "safety_factor": 1, "recommended": -17, "warnings": {"matches_lookahead": false, "disagreement_too_high": false}}}, "checks": {"version": 3, "passed": true}}, "validation_esr": null}}, "architecture": "LSTM", "config": {"input_size": 1, "hidden_size": 3, "num_layers": 1}, "weights": [-0.21677088737487793, -0.6683622002601624, -0.2560940980911255, -0.3588429093360901, 0.17952610552310944, 0.19445613026618958, -0.01662646047770977, 0.5353694558143616, -0.2536540627479553, -0.5132213234901428, -0.020476307719945908, 0.08592455089092255, -0.6891753673553467, 0.3627359867095947, 0.008421811275184155, 0.3113192617893219, 0.14251480996608734, 0.07989779114723206, -0.18211324512958527, 0.7118963003158569, 0.41084015369415283, -0.6571938395500183, -0.13214066624641418, -0.2698603868484497, 0.49387243390083313, -0.3491725027561188, 0.6353667974472046, -0.5005152225494385, 0.2052856683731079, -0.4301638901233673, -0.15770092606544495, -0.7181791067123413, 0.056290093809366226, -0.49049463868141174, 0.6623441576957703, 0.09029324352741241, 0.34005245566368103, 0.16416560113430023, 0.15520110726356506, -0.4155678153038025, -0.36928507685661316, 0.3211132884025574, -0.6769840121269226, -0.1575538069009781, 0.05268515646457672, -0.4191459119319916, 0.599330484867096, 0.21518059074878693, -4.246325492858887, -3.315647840499878, -4.328850746154785, 4.496089458465576, 5.015639305114746, 3.6492037773132324, 0.14431169629096985, -0.6633821725845337, 0.11673200130462646, -0.1418764889240265, -0.4897872805595398, -0.8689419031143188, -0.06714004278182983, -0.4450395107269287, -0.02142983116209507, -0.15136894583702087, -2.775207996368408, -0.08681213855743408, 0.05702732503414154, 0.670292317867279, 0.31442636251449585, 0.30793967843055725], "sample_rate": 48000})";

// Copied over but shouldn't be publicly-exposed.
std::vector<float> GetWeights(nlohmann::json const& j)
{
  auto it = j.find("weights");
  if (it != j.end())
  {
    return *it;
  }
  else
  {
    throw std::runtime_error("Corrupted model file is missing weights.");
  }
}

nam::dspData _GetConfig(const std::string& configStr = basicConfigStr)
{
  nlohmann::json j = nlohmann::json::parse(configStr);

  std::vector<float> weights = GetWeights(j);
  nam::dspData returnedConfig;
  returnedConfig.version = j["version"];
  returnedConfig.architecture = j["architecture"];
  returnedConfig.config = j["config"];
  returnedConfig.metadata = j["metadata"];
  returnedConfig.weights = weights;

  return returnedConfig;
}

void test_gets_input_level()
{
  nam::dspData config = _GetConfig();
  std::unique_ptr<nam::DSP> dsp = get_dsp(config);
  assert(dsp->HasInputLevel());
}
void test_gets_output_level()
{
  nam::dspData config = _GetConfig();
  std::unique_ptr<nam::DSP> dsp = get_dsp(config);
  assert(dsp->HasOutputLevel());
}

void test_null_input_level()
{
  // Issue 129
  const std::string configStr =
    R"({"version": "0.5.4", "metadata": {"date": {"year": 2024, "month": 10, "day": 9, "hour": 18, "minute": 44, "second": 41}, "loudness": -37.8406867980957, "gain": 0.13508800804658277, "name": "Test LSTM", "modeled_by": "Steve", "gear_type": "amp", "gear_make": "Darkglass Electronics", "gear_model": "Microtubes 900 v2", "tone_type": "clean", "input_level_dbu": null, "output_level_dbu": 12.3, "training": {"settings": {"ignore_checks": false}, "data": {"latency": {"manual": null, "calibration": {"algorithm_version": 1, "delays": [-16], "safety_factor": 1, "recommended": -17, "warnings": {"matches_lookahead": false, "disagreement_too_high": false}}}, "checks": {"version": 3, "passed": true}}, "validation_esr": null}}, "architecture": "LSTM", "config": {"input_size": 1, "hidden_size": 3, "num_layers": 1}, "weights": [-0.21677088737487793, -0.6683622002601624, -0.2560940980911255, -0.3588429093360901, 0.17952610552310944, 0.19445613026618958, -0.01662646047770977, 0.5353694558143616, -0.2536540627479553, -0.5132213234901428, -0.020476307719945908, 0.08592455089092255, -0.6891753673553467, 0.3627359867095947, 0.008421811275184155, 0.3113192617893219, 0.14251480996608734, 0.07989779114723206, -0.18211324512958527, 0.7118963003158569, 0.41084015369415283, -0.6571938395500183, -0.13214066624641418, -0.2698603868484497, 0.49387243390083313, -0.3491725027561188, 0.6353667974472046, -0.5005152225494385, 0.2052856683731079, -0.4301638901233673, -0.15770092606544495, -0.7181791067123413, 0.056290093809366226, -0.49049463868141174, 0.6623441576957703, 0.09029324352741241, 0.34005245566368103, 0.16416560113430023, 0.15520110726356506, -0.4155678153038025, -0.36928507685661316, 0.3211132884025574, -0.6769840121269226, -0.1575538069009781, 0.05268515646457672, -0.4191459119319916, 0.599330484867096, 0.21518059074878693, -4.246325492858887, -3.315647840499878, -4.328850746154785, 4.496089458465576, 5.015639305114746, 3.6492037773132324, 0.14431169629096985, -0.6633821725845337, 0.11673200130462646, -0.1418764889240265, -0.4897872805595398, -0.8689419031143188, -0.06714004278182983, -0.4450395107269287, -0.02142983116209507, -0.15136894583702087, -2.775207996368408, -0.08681213855743408, 0.05702732503414154, 0.670292317867279, 0.31442636251449585, 0.30793967843055725], "sample_rate": 48000})";
  nam::dspData config = _GetConfig(configStr);
  // The first part of this is that the following line doesn't fail:
  std::unique_ptr<nam::DSP> dsp = get_dsp(config);

  assert(!dsp->HasInputLevel());
  assert(dsp->HasOutputLevel());
}

void test_null_output_level()
{
  // Issue 129
  const std::string configStr =
    R"({"version": "0.5.4", "metadata": {"date": {"year": 2024, "month": 10, "day": 9, "hour": 18, "minute": 44, "second": 41}, "loudness": -37.8406867980957, "gain": 0.13508800804658277, "name": "Test LSTM", "modeled_by": "Steve", "gear_type": "amp", "gear_make": "Darkglass Electronics", "gear_model": "Microtubes 900 v2", "tone_type": "clean", "input_level_dbu": 19.0, "output_level_dbu": null, "training": {"settings": {"ignore_checks": false}, "data": {"latency": {"manual": null, "calibration": {"algorithm_version": 1, "delays": [-16], "safety_factor": 1, "recommended": -17, "warnings": {"matches_lookahead": false, "disagreement_too_high": false}}}, "checks": {"version": 3, "passed": true}}, "validation_esr": null}}, "architecture": "LSTM", "config": {"input_size": 1, "hidden_size": 3, "num_layers": 1}, "weights": [-0.21677088737487793, -0.6683622002601624, -0.2560940980911255, -0.3588429093360901, 0.17952610552310944, 0.19445613026618958, -0.01662646047770977, 0.5353694558143616, -0.2536540627479553, -0.5132213234901428, -0.020476307719945908, 0.08592455089092255, -0.6891753673553467, 0.3627359867095947, 0.008421811275184155, 0.3113192617893219, 0.14251480996608734, 0.07989779114723206, -0.18211324512958527, 0.7118963003158569, 0.41084015369415283, -0.6571938395500183, -0.13214066624641418, -0.2698603868484497, 0.49387243390083313, -0.3491725027561188, 0.6353667974472046, -0.5005152225494385, 0.2052856683731079, -0.4301638901233673, -0.15770092606544495, -0.7181791067123413, 0.056290093809366226, -0.49049463868141174, 0.6623441576957703, 0.09029324352741241, 0.34005245566368103, 0.16416560113430023, 0.15520110726356506, -0.4155678153038025, -0.36928507685661316, 0.3211132884025574, -0.6769840121269226, -0.1575538069009781, 0.05268515646457672, -0.4191459119319916, 0.599330484867096, 0.21518059074878693, -4.246325492858887, -3.315647840499878, -4.328850746154785, 4.496089458465576, 5.015639305114746, 3.6492037773132324, 0.14431169629096985, -0.6633821725845337, 0.11673200130462646, -0.1418764889240265, -0.4897872805595398, -0.8689419031143188, -0.06714004278182983, -0.4450395107269287, -0.02142983116209507, -0.15136894583702087, -2.775207996368408, -0.08681213855743408, 0.05702732503414154, 0.670292317867279, 0.31442636251449585, 0.30793967843055725], "sample_rate": 48000})";
  nam::dspData config = _GetConfig(configStr);
  // The first part of this is that the following line doesn't fail:
  std::unique_ptr<nam::DSP> dsp = get_dsp(config);
  assert(dsp->HasInputLevel());
  assert(!dsp->HasOutputLevel());
}

// Helper function to process buffers through a DSP model
void process_buffers(nam::DSP* dsp, int num_buffers, int buffer_size)
{
  const int in_channels = dsp->NumInputChannels();
  const int out_channels = dsp->NumOutputChannels();
  const double sample_rate = dsp->GetExpectedSampleRate() > 0 ? dsp->GetExpectedSampleRate() : 48000.0;

  // Reset the model
  dsp->Reset(sample_rate, buffer_size);

  // Allocate buffers for all channels
  std::vector<std::vector<NAM_SAMPLE>> inputBuffers(in_channels);
  std::vector<std::vector<NAM_SAMPLE>> outputBuffers(out_channels);
  std::vector<NAM_SAMPLE*> inputPtrs(in_channels);
  std::vector<NAM_SAMPLE*> outputPtrs(out_channels);

  for (int ch = 0; ch < in_channels; ch++)
  {
    inputBuffers[ch].resize(buffer_size, (NAM_SAMPLE)0.0);
    inputPtrs[ch] = inputBuffers[ch].data();
  }
  for (int ch = 0; ch < out_channels; ch++)
  {
    outputBuffers[ch].resize(buffer_size, (NAM_SAMPLE)0.0);
    outputPtrs[ch] = outputBuffers[ch].data();
  }

  double output_peak = 0.0;
  // Process num_buffers buffers
  for (int buf = 0; buf < num_buffers; buf++)
  {
    // Fill input with some test data (simple sine-like pattern)
    for (int ch = 0; ch < in_channels; ch++)
    {
      for (int i = 0; i < buffer_size; i++)
      {
        inputBuffers[ch][i] = (NAM_SAMPLE)(0.1 * (ch + 1) * ((buf * buffer_size + i) % 100) / 100.0);
      }
    }

    // Process the buffer
    dsp->process(inputPtrs.data(), outputPtrs.data(), buffer_size);

    // Verify output is finite
    for (int ch = 0; ch < out_channels; ch++)
    {
      for (int i = 0; i < buffer_size; i++)
      {
        assert(std::isfinite(outputBuffers[ch][i]));
        output_peak = std::max(output_peak, std::abs(static_cast<double>(outputBuffers[ch][i])));
      }
    }
  }
  assert(output_peak > 1.0e-8);
}

void test_load_and_process_nam_files()
{
  // Test loading and processing three different .nam files
  // Paths are relative to root directory where tests run (./build/tools/run_tests)
  const std::vector<std::string> nam_files = {"example_models/wavenet.nam", "example_models/lstm.nam",
                                              "example_models/wavenet_condition_dsp.nam",
                                              "example_models/wavenet_a2_max.nam"};

  const int num_buffers = 3;
  const int buffer_size = 64;

  for (const auto& nam_file : nam_files)
  {
    std::filesystem::path model_path(nam_file);

    // Load the model
    std::unique_ptr<nam::DSP> dsp = nam::get_dsp(model_path);
    assert(dsp != nullptr);

    // Process buffers through the model
    process_buffers(dsp.get(), num_buffers, buffer_size);
  }
}

std::vector<NAM_SAMPLE> process_parametric_model(const std::filesystem::path& model_path, const double gain)
{
  constexpr int buffer_size = 2048;
  std::unique_ptr<nam::DSP> dsp = nam::get_dsp(model_path);
  assert(dsp != nullptr);
  const double sample_rate = dsp->GetExpectedSampleRate() > 0.0 ? dsp->GetExpectedSampleRate() : 48000.0;
  dsp->ResetAndPrewarm(sample_rate, buffer_size);

  std::vector<NAM_SAMPLE> input(buffer_size);
  std::vector<NAM_SAMPLE> output(buffer_size, 0.0);
  for (int i = 0; i < buffer_size; i++)
    input[i] = static_cast<NAM_SAMPLE>(0.15 * std::sin(2.0 * M_PI * 110.0 * i / sample_rate));

  NAM_SAMPLE* input_ptr = input.data();
  NAM_SAMPLE* output_ptr = output.data();
  dsp->process(&input_ptr, &output_ptr, buffer_size, &gain, 1);
  for (const NAM_SAMPLE sample : output)
    assert(std::isfinite(sample));
  return output;
}

void test_bundled_parametric_models_load_and_respond_to_gain()
{
  const std::vector<std::filesystem::path> model_paths = {
    "../NeuralAmpModeler/resources/models/DessTortion-blue/DessTortion-blue.nam",
    "../NeuralAmpModeler/resources/models/DessTortion-red/DessTortion-red.nam",
    "../NeuralAmpModeler/resources/models/DessBlock-green/model.nam",
    "../NeuralAmpModeler/resources/models/SickDess/SickDess.nam"};

  for (const auto& model_path : model_paths)
  {
    assert(std::filesystem::is_regular_file(model_path));
    const auto low = process_parametric_model(model_path, 0.2);
    const auto high = process_parametric_model(model_path, 0.8);
    double absolute_difference = 0.0;
    for (size_t i = 0; i < low.size(); i++)
      absolute_difference += std::abs(static_cast<double>(low[i] - high[i]));
    assert(absolute_difference > 1.0e-4);
  }
}

void test_bundled_drive_models_load_and_render()
{
  const std::vector<std::filesystem::path> model_paths = {
    "../NeuralAmpModeler/resources/models/DessDrive/OD808.nam",
    "../NeuralAmpModeler/resources/models/DessDrive/SD1.nam",
    "../NeuralAmpModeler/resources/models/DessDrive/TS9.nam",
    "../NeuralAmpModeler/resources/models/DessDrive/aesahaettr.nam"};

  for (const auto& model_path : model_paths)
  {
    assert(std::filesystem::is_regular_file(model_path));
    std::unique_ptr<nam::DSP> dsp = nam::get_dsp(model_path);
    assert(dsp != nullptr);
    process_buffers(dsp.get(), 3, 64);
  }
}
}; // namespace test_get_dsp

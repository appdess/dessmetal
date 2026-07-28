#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <sstream>

#include <Eigen/Dense>

#include "get_dsp.h"
#include "registry.h"
#include "wavenet.h"

namespace
{
constexpr int kMaxGlobalConditionSize = 64;
}

// Layer ======================================================================

void nam::wavenet::_Layer::SetMaxBufferSize(const int maxBufferSize)
{
  _conv.SetMaxBufferSize(maxBufferSize);
  _input_mixin.SetMaxBufferSize(maxBufferSize);
  const long z_channels = this->_conv.get_out_channels(); // This is 2*bottleneck when gated, bottleneck when not
  _z.resize(z_channels, maxBufferSize);
  _1x1.SetMaxBufferSize(maxBufferSize);
  // Pre-allocate output buffers
  const long channels = this->get_channels();
  this->_output_next_layer.resize(channels, maxBufferSize);
  // _output_head stores the activated portion: bottleneck rows when no head1x1, or head1x1 out_channels when head1x1 is
  // active
  if (_head1x1)
  {
    this->_output_head.resize(_head1x1->get_out_channels(), maxBufferSize);
    this->_output_head.setZero(); // Ensure consistent initialization across platforms
    _head1x1->SetMaxBufferSize(maxBufferSize);
  }
  else
  {
    this->_output_head.resize(this->_bottleneck, maxBufferSize);
    this->_output_head.setZero(); // Ensure consistent initialization across platforms
  }
  // Set max buffer size for FiLM objects
  if (this->_conv_pre_film)
    this->_conv_pre_film->SetMaxBufferSize(maxBufferSize);
  if (this->_conv_post_film)
    this->_conv_post_film->SetMaxBufferSize(maxBufferSize);
  if (this->_input_mixin_pre_film)
    this->_input_mixin_pre_film->SetMaxBufferSize(maxBufferSize);
  if (this->_input_mixin_post_film)
    this->_input_mixin_post_film->SetMaxBufferSize(maxBufferSize);
  if (this->_activation_pre_film)
    this->_activation_pre_film->SetMaxBufferSize(maxBufferSize);
  if (this->_activation_post_film)
    this->_activation_post_film->SetMaxBufferSize(maxBufferSize);
  if (this->_1x1_post_film)
    this->_1x1_post_film->SetMaxBufferSize(maxBufferSize);
  if (this->_head1x1_post_film)
    this->_head1x1_post_film->SetMaxBufferSize(maxBufferSize);
}

void nam::wavenet::_Layer::set_weights_(std::vector<float>::iterator& weights)
{
  this->_conv.set_weights_(weights);
  this->_input_mixin.set_weights_(weights);
  this->_1x1.set_weights_(weights);
  if (this->_head1x1)
  {
    this->_head1x1->set_weights_(weights);
  }
  // Set weights for FiLM objects
  if (this->_conv_pre_film)
    this->_conv_pre_film->set_weights_(weights);
  if (this->_conv_post_film)
    this->_conv_post_film->set_weights_(weights);
  if (this->_input_mixin_pre_film)
    this->_input_mixin_pre_film->set_weights_(weights);
  if (this->_input_mixin_post_film)
    this->_input_mixin_post_film->set_weights_(weights);
  if (this->_activation_pre_film)
    this->_activation_pre_film->set_weights_(weights);
  if (this->_activation_post_film)
    this->_activation_post_film->set_weights_(weights);
  if (this->_1x1_post_film)
    this->_1x1_post_film->set_weights_(weights);
  if (this->_head1x1_post_film)
    this->_head1x1_post_film->set_weights_(weights);
}

void nam::wavenet::_Layer::Process(const Eigen::MatrixXf& input, const Eigen::MatrixXf& condition, const int num_frames)
{
  const long bottleneck = this->_bottleneck; // Use the actual bottleneck value, not the doubled output channels

  // Step 1: input convolutions
  if (this->_conv_pre_film)
  {
    // Use Process() instead of Process_() since input is const
    this->_conv_pre_film->Process(input, condition, num_frames);
    this->_conv.Process(this->_conv_pre_film->GetOutput(), num_frames);
  }
  else
  {
    this->_conv.Process(input, num_frames);
  }
  if (this->_conv_post_film)
  {
    Eigen::MatrixXf& conv_output = this->_conv.GetOutput();
    this->_conv_post_film->Process_(conv_output, condition, num_frames);
  }

  if (this->_input_mixin_pre_film)
  {
    // Use Process() instead of Process_() since condition is const
    this->_input_mixin_pre_film->Process(condition, condition, num_frames);
    this->_input_mixin.process_(this->_input_mixin_pre_film->GetOutput(), num_frames);
  }
  else
  {
    this->_input_mixin.process_(condition, num_frames);
  }
  if (this->_input_mixin_post_film)
  {
    Eigen::MatrixXf& input_mixin_output = this->_input_mixin.GetOutput();
    this->_input_mixin_post_film->Process_(input_mixin_output, condition, num_frames);
  }
  this->_z.leftCols(num_frames).noalias() =
    _conv.GetOutput().leftCols(num_frames) + _input_mixin.GetOutput().leftCols(num_frames);
  if (this->_activation_pre_film)
  {
    this->_activation_pre_film->Process_(this->_z, condition, num_frames);
  }

  // Step 2 & 3: activation and 1x1
  //
  // A note about the gating/blending activations:
  // They take 2x dimension as input.
  // The top channels are for the "primary" activation and will be in-place modified for the final result.
  // The bottom channels are for the "secondary" activation and should not be used post-activation.
  if (this->_gating_mode == GatingMode::NONE)
  {
    this->_activation->apply(this->_z.leftCols(num_frames));
    if (this->_activation_post_film)
    {
      this->_activation_post_film->Process_(this->_z, condition, num_frames);
    }
    _1x1.process_(_z, num_frames);
  }
  else if (this->_gating_mode == GatingMode::GATED)
  {
    // Use the GatingActivation class
    // Extract the blocks first to avoid temporary reference issues
    auto input_block = this->_z.leftCols(num_frames);
    auto output_block = this->_z.topRows(bottleneck).leftCols(num_frames);
    this->_gating_activation->apply(input_block, output_block);
    if (this->_activation_post_film)
    {
      // Use Process() for blocks and copy result back
      this->_activation_post_film->Process(this->_z.topRows(bottleneck), condition, num_frames);
      this->_z.topRows(bottleneck).leftCols(num_frames).noalias() =
        this->_activation_post_film->GetOutput().leftCols(num_frames);
    }
    _1x1.process_(this->_z.topRows(bottleneck), num_frames);
  }
  else if (this->_gating_mode == GatingMode::BLENDED)
  {
    // Use the BlendingActivation class
    // Extract the blocks first to avoid temporary reference issues
    auto input_block = this->_z.leftCols(num_frames);
    auto output_block = this->_z.topRows(bottleneck).leftCols(num_frames);
    this->_blending_activation->apply(input_block, output_block);
    if (this->_activation_post_film)
    {
      // Use Process() for blocks and copy result back
      this->_activation_post_film->Process(this->_z.topRows(bottleneck), condition, num_frames);
      this->_z.topRows(bottleneck).leftCols(num_frames).noalias() =
        this->_activation_post_film->GetOutput().leftCols(num_frames);
    }
    _1x1.process_(this->_z.topRows(bottleneck), num_frames);
    if (this->_1x1_post_film)
    {
      Eigen::MatrixXf& _1x1_output = this->_1x1.GetOutput();
      this->_1x1_post_film->Process_(_1x1_output, condition, num_frames);
    }
  }

  if (this->_head1x1)
  {
    if (this->_gating_mode == GatingMode::NONE)
    {
      this->_head1x1->process_(this->_z.leftCols(num_frames), num_frames);
    }
    else
    {
      this->_head1x1->process_(this->_z.topRows(bottleneck).leftCols(num_frames), num_frames);
    }
    this->_head1x1->process(this->_z.topRows(bottleneck).leftCols(num_frames), num_frames);
    if (this->_head1x1_post_film)
    {
      Eigen::MatrixXf& head1x1_output = this->_head1x1->GetOutput();
      this->_head1x1_post_film->Process_(head1x1_output, condition, num_frames);
    }
    this->_output_head.leftCols(num_frames).noalias() = this->_head1x1->GetOutput().leftCols(num_frames);
  }
  else // No head 1x1
  {
    // (No FiLM)
    // Store output to head (skip connection: activated conv output)
    if (this->_gating_mode == GatingMode::NONE)
      this->_output_head.leftCols(num_frames).noalias() = this->_z.leftCols(num_frames);
    else
      this->_output_head.leftCols(num_frames).noalias() = this->_z.topRows(bottleneck).leftCols(num_frames);
  }

  // Store output to next layer (residual connection: input + _1x1 output)
  this->_output_next_layer.leftCols(num_frames).noalias() =
    input.leftCols(num_frames) + _1x1.GetOutput().leftCols(num_frames);
}

// LayerArray =================================================================

nam::wavenet::_LayerArray::_LayerArray(
  const int input_size, const int condition_size, const int head_size, const int channels, const int bottleneck,
  const int kernel_size, const std::vector<int>& dilations, const activations::ActivationConfig& activation_config,
  const GatingMode gating_mode, const bool head_bias, const int groups_input, const int groups_input_mixin,
  const int groups_1x1, const Head1x1Params& head1x1_params,
  const activations::ActivationConfig& secondary_activation_config, const _FiLMParams& conv_pre_film_params,
  const _FiLMParams& conv_post_film_params, const _FiLMParams& input_mixin_pre_film_params,
  const _FiLMParams& input_mixin_post_film_params, const _FiLMParams& activation_pre_film_params,
  const _FiLMParams& activation_post_film_params, const _FiLMParams& _1x1_post_film_params,
  const _FiLMParams& head1x1_post_film_params)
: _rechannel(input_size, channels, false)
, _head_rechannel(head1x1_params.active ? head1x1_params.out_channels : bottleneck, head_size, head_bias)
, _head_output_size(head1x1_params.active ? head1x1_params.out_channels : bottleneck)
{
  for (size_t i = 0; i < dilations.size(); i++)
    this->_layers.push_back(
      _Layer(condition_size, channels, bottleneck, kernel_size, dilations[i], activation_config, gating_mode,
             groups_input, groups_input_mixin, groups_1x1, head1x1_params, secondary_activation_config,
             conv_pre_film_params, conv_post_film_params, input_mixin_pre_film_params, input_mixin_post_film_params,
             activation_pre_film_params, activation_post_film_params, _1x1_post_film_params, head1x1_post_film_params));
}

void nam::wavenet::_LayerArray::SetMaxBufferSize(const int maxBufferSize)
{
  _rechannel.SetMaxBufferSize(maxBufferSize);
  _head_rechannel.SetMaxBufferSize(maxBufferSize);
  for (auto it = _layers.begin(); it != _layers.end(); ++it)
  {
    it->SetMaxBufferSize(maxBufferSize);
  }
  // Pre-allocate output buffers
  const long channels = this->_get_channels();
  this->_layer_outputs.resize(channels, maxBufferSize);
  // _head_inputs size matches actual head output: head1x1.out_channels if active, else bottleneck
  this->_head_inputs.resize(this->_head_output_size, maxBufferSize);
}


long nam::wavenet::_LayerArray::get_receptive_field() const
{
  long result = 0;
  for (size_t i = 0; i < this->_layers.size(); i++)
    result += this->_layers[i].get_dilation() * (this->_layers[i].get_kernel_size() - 1);
  return result;
}


void nam::wavenet::_LayerArray::Process(const Eigen::MatrixXf& layer_inputs, const Eigen::MatrixXf& condition,
                                        const int num_frames)
{
  // Zero head inputs accumulator (first layer array)
  this->_head_inputs.setZero();
  ProcessInner(layer_inputs, condition, num_frames);
}

void nam::wavenet::_LayerArray::Process(const Eigen::MatrixXf& layer_inputs, const Eigen::MatrixXf& condition,
                                        const Eigen::MatrixXf& head_inputs, const int num_frames)
{
  // Copy head inputs from previous layer array
  this->_head_inputs.leftCols(num_frames).noalias() = head_inputs.leftCols(num_frames);
  ProcessInner(layer_inputs, condition, num_frames);
}

void nam::wavenet::_LayerArray::ProcessInner(const Eigen::MatrixXf& layer_inputs, const Eigen::MatrixXf& condition,
                                             const int num_frames)
{
  // Process rechannel and get output
  this->_rechannel.process_(layer_inputs, num_frames);
  Eigen::MatrixXf& rechannel_output = _rechannel.GetOutput();

  // Process layers
  for (size_t i = 0; i < this->_layers.size(); i++)
  {
    // Process first layer with rechannel output, subsequent layers with previous layer output
    // Use separate branches to avoid ternary operator creating temporaries
    if (i == 0)
    {
      // First layer consumes the rechannel output buffer
      this->_layers[i].Process(rechannel_output, condition, num_frames);
    }
    else
    {
      // Subsequent layers consume the full output buffer of the previous layer
      Eigen::MatrixXf& prev_output = this->_layers[i - 1].GetOutputNextLayer();
      this->_layers[i].Process(prev_output, condition, num_frames);
    }

    // Accumulate head output from this layer
    this->_head_inputs.leftCols(num_frames).noalias() += this->_layers[i].GetOutputHead().leftCols(num_frames);
  }

  // Store output from last layer
  const size_t last_layer = this->_layers.size() - 1;
  this->_layer_outputs.leftCols(num_frames).noalias() =
    this->_layers[last_layer].GetOutputNextLayer().leftCols(num_frames);

  // Process head rechannel
  _head_rechannel.process_(this->_head_inputs, num_frames);
}


Eigen::MatrixXf& nam::wavenet::_LayerArray::GetHeadOutputs()
{
  return this->_head_rechannel.GetOutput();
}

const Eigen::MatrixXf& nam::wavenet::_LayerArray::GetHeadOutputs() const
{
  return this->_head_rechannel.GetOutput();
}


void nam::wavenet::_LayerArray::set_weights_(std::vector<float>::iterator& weights)
{
  this->_rechannel.set_weights_(weights);
  for (size_t i = 0; i < this->_layers.size(); i++)
    this->_layers[i].set_weights_(weights);
  this->_head_rechannel.set_weights_(weights);
}

long nam::wavenet::_LayerArray::_get_channels() const
{
  return this->_layers.size() > 0 ? this->_layers[0].get_channels() : 0;
}

// WaveNet ====================================================================

nam::wavenet::WaveNet::WaveNet(const int in_channels,
                               const std::vector<nam::wavenet::LayerArrayParams>& layer_array_params,
                               const float head_scale, const bool with_head, std::vector<float> weights,
                               std::unique_ptr<DSP> condition_dsp, const double expected_sample_rate,
                               const int global_condition_size)
: DSP(in_channels,
      layer_array_params.empty() ? throw std::runtime_error("WaveNet requires at least one layer array")
                                 : layer_array_params.back().head_size,
      expected_sample_rate)
, _condition_dsp(std::move(condition_dsp))
, _head_scale(head_scale)
, _global_condition_size(global_condition_size)
, _default_global_condition(static_cast<size_t>(std::max(0, global_condition_size)), 0.5)
{
  if (_global_condition_size < 0 || _global_condition_size > kMaxGlobalConditionSize)
    throw std::invalid_argument("global_condition_size is outside the supported range");
  // Assert that if there's a condition DSP, its input is compatible with what it'll get from this WaveNet:
  if (this->_condition_dsp != nullptr)
  {
    if (this->_get_condition_dim() != this->_condition_dsp->NumInputChannels())
    {
      std::stringstream ss;
      ss << "input channels of WaveNet (" << in_channels << ") don't match input channels of condition DSP ("
         << this->_condition_dsp->NumInputChannels() << "!\n";
      throw std::runtime_error(ss.str().c_str());
    }
  }
  if (layer_array_params.empty())
    throw std::runtime_error("WaveNet requires at least one layer array");
  if (with_head)
    throw std::runtime_error("Head not implemented!");
  for (size_t i = 0; i < layer_array_params.size(); i++)
  {
    // Quick assert that the condition_dsp will output compatibly with this layer array
    if (this->_condition_dsp != nullptr)
    {
      const int conditionDSPOutputs = this->_condition_dsp->NumOutputChannels();
      if (conditionDSPOutputs < 0
          || conditionDSPOutputs > std::numeric_limits<int>::max() - this->_global_condition_size)
        throw std::invalid_argument("Condition DSP output size is outside the supported range");
      const int expectedConditionSize = conditionDSPOutputs + this->_global_condition_size;
      if (layer_array_params[i].condition_size != expectedConditionSize)
      {
        std::stringstream ss;
        ss << "condition_size of layer " << i << " (" << layer_array_params[i].condition_size
           << ") doesn't match output channels of condition DSP plus global conditioning (" << expectedConditionSize
           << "!\n";
        throw std::runtime_error(ss.str().c_str());
      }
    }
    this->_layer_arrays.push_back(nam::wavenet::_LayerArray(
      layer_array_params[i].input_size, layer_array_params[i].condition_size, layer_array_params[i].head_size,
      layer_array_params[i].channels, layer_array_params[i].bottleneck, layer_array_params[i].kernel_size,
      layer_array_params[i].dilations, layer_array_params[i].activation_config, layer_array_params[i].gating_mode,
      layer_array_params[i].head_bias, layer_array_params[i].groups_input, layer_array_params[i].groups_input_mixin,
      layer_array_params[i].groups_1x1, layer_array_params[i].head1x1_params,
      layer_array_params[i].secondary_activation_config, layer_array_params[i].conv_pre_film_params,
      layer_array_params[i].conv_post_film_params, layer_array_params[i].input_mixin_pre_film_params,
      layer_array_params[i].input_mixin_post_film_params, layer_array_params[i].activation_pre_film_params,
      layer_array_params[i].activation_post_film_params, layer_array_params[i]._1x1_post_film_params,
      layer_array_params[i].head1x1_post_film_params));
    if (i > 0)
      if (layer_array_params[i].channels != layer_array_params[i - 1].head_size)
      {
        std::stringstream ss;
        ss << "channels of layer " << i << " (" << layer_array_params[i].channels
           << ") doesn't match head_size of preceding layer (" << layer_array_params[i - 1].head_size << "!\n";
        throw std::runtime_error(ss.str().c_str());
      }
  }
  this->set_weights_(weights);

  // Finally, figure out how much pre-warming is needed for this model.
  mPrewarmSamples = this->_condition_dsp != nullptr ? this->_condition_dsp->PrewarmSamples() : 1;
  for (size_t i = 0; i < this->_layer_arrays.size(); i++)
    mPrewarmSamples += this->_layer_arrays[i].get_receptive_field();
}

void nam::wavenet::WaveNet::set_weights_(std::vector<float>& weights)
{
  std::vector<float>::iterator it = weights.begin();
  // Note: condition_dsp already has its own weights from construction,
  // so we don't need to set its weights here.
  for (size_t i = 0; i < this->_layer_arrays.size(); i++)
    this->_layer_arrays[i].set_weights_(it);
  this->_head_scale = *(it++); // TODO `LayerArray.absorb_head_scale()`
  if (it != weights.end())
  {
    std::stringstream ss;
    for (size_t i = 0; i < weights.size(); i++)
      if (weights[i] == *it)
      {
        ss << "Weight mismatch: assigned " << i + 1 << " weights, but " << weights.size() << " were provided.";
        throw std::runtime_error(ss.str().c_str());
      }
    ss << "Weight mismatch: provided " << weights.size() << " weights, but the model expects more.";
    throw std::runtime_error(ss.str().c_str());
  }
}

void nam::wavenet::WaveNet::SetMaxBufferSize(const int maxBufferSize)
{
  DSP::SetMaxBufferSize(maxBufferSize);
  this->_condition_input.resize(this->_get_condition_dim(), maxBufferSize);
  const int condition_output_channels = this->_condition_dsp == nullptr
                                          ? this->_get_condition_dim()
                                          : this->_condition_dsp->NumOutputChannels();
  // Resize condition output
  if (this->_condition_dsp == nullptr)
  {
    this->_condition_output.resize(condition_output_channels + this->_global_condition_size, maxBufferSize);
  }
  else
  {
    this->_condition_dsp->SetMaxBufferSize(maxBufferSize);
    this->_condition_output.resize(condition_output_channels + this->_global_condition_size, maxBufferSize);

    // Resize temporary buffers for condition DSP processing
    const int condition_dim = this->_get_condition_dim();
    this->_condition_dsp_input_buffers.resize(condition_dim);
    this->_condition_dsp_output_buffers.resize(condition_output_channels);
    this->_condition_dsp_input_ptrs.resize(condition_dim);
    this->_condition_dsp_output_ptrs.resize(condition_output_channels);

    for (int ch = 0; ch < condition_dim; ch++)
    {
      this->_condition_dsp_input_buffers[ch].resize(maxBufferSize);
      this->_condition_dsp_input_ptrs[ch] = this->_condition_dsp_input_buffers[ch].data();
    }

    for (int ch = 0; ch < condition_output_channels; ch++)
    {
      this->_condition_dsp_output_buffers[ch].resize(maxBufferSize);
      this->_condition_dsp_output_ptrs[ch] = this->_condition_dsp_output_buffers[ch].data();
    }
  }

  for (size_t i = 0; i < this->_layer_arrays.size(); i++)
    this->_layer_arrays[i].SetMaxBufferSize(maxBufferSize);
}

void nam::wavenet::WaveNet::_process_condition(const int num_frames)
{
  if (this->_condition_dsp == nullptr)
  {
    this->_condition_output.topRows(this->_get_condition_dim()).leftCols(num_frames) =
      this->_condition_input.leftCols(num_frames);
  }
  else
  {
    // Copy input data from Eigen matrix to pre-allocated contiguous buffers
    // Since Eigen matrices are column-major, rows are not contiguous
    // TODO maybe use row-major here?
    const int condition_dim = this->_get_condition_dim();
    for (int ch = 0; ch < condition_dim; ch++)
    {
      for (int j = 0; j < num_frames; j++)
        this->_condition_dsp_input_buffers[ch][j] = (NAM_SAMPLE)this->_condition_input(ch, j);
    }

    // Process through condition DSP using pre-allocated buffers
    this->_condition_dsp->process(
      this->_condition_dsp_input_ptrs.data(), this->_condition_dsp_output_ptrs.data(), num_frames);

    // Copy output data back to Eigen matrix
    const int condition_output_channels = this->_condition_dsp->NumOutputChannels();
    for (int ch = 0; ch < condition_output_channels; ch++)
    {
      for (int j = 0; j < num_frames; j++)
        this->_condition_output(ch, j) = (float)this->_condition_dsp_output_buffers[ch][j];
    }
  }
}

void nam::wavenet::WaveNet::_set_condition_array(NAM_SAMPLE** input, const int num_frames)
{
  const int in_channels = NumInputChannels();
  // Fill condition array with input channels
  for (int ch = 0; ch < in_channels; ch++)
  {
    for (int j = 0; j < num_frames; j++)
    {
      this->_condition_input(ch, j) = input[ch][j];
    }
  }
}

void nam::wavenet::WaveNet::process(NAM_SAMPLE** input, NAM_SAMPLE** output, const int num_frames)
{
  if (this->_global_condition_size == 0)
    return this->_process(input, output, num_frames, nullptr, 0);

  // A stable midpoint is safer than feeding an uninitialised condition when a
  // generic NAM host invokes the legacy, non-parametric interface.
  this->_process(input, output, num_frames, this->_default_global_condition.data(), this->_global_condition_size);
}

void nam::wavenet::WaveNet::process(NAM_SAMPLE** input, NAM_SAMPLE** output, const int num_frames,
                                    const double* params, const int num_params)
{
  this->_process(input, output, num_frames, params, num_params);
}

void nam::wavenet::WaveNet::SetDefaultParams(const double* params, const int num_params)
{
  if (num_params != this->_global_condition_size || (num_params > 0 && params == nullptr))
    throw std::invalid_argument("Default global conditioning vector has the wrong size");
  for (int parameter = 0; parameter < num_params; parameter++)
  {
    if (!std::isfinite(params[parameter]))
      throw std::invalid_argument("Default global conditioning values must be finite");
    this->_default_global_condition[static_cast<size_t>(parameter)] = params[parameter];
  }
}

void nam::wavenet::WaveNet::_process(NAM_SAMPLE** input, NAM_SAMPLE** output, const int num_frames,
                                     const double* params, const int num_params)
{
  assert(num_frames <= mMaxBufferSize);
  if (num_frames < 0 || num_frames > mMaxBufferSize)
    throw std::invalid_argument("Invalid WaveNet frame count");
  if (num_params != this->_global_condition_size || (num_params > 0 && params == nullptr))
    throw std::invalid_argument("Global conditioning vector has the wrong size");
  const int out_channels = NumOutputChannels();

  this->_set_condition_array(input, num_frames);
  this->_process_condition(num_frames);
  const int base_condition_rows = static_cast<int>(this->_condition_output.rows()) - this->_global_condition_size;
  for (int parameter = 0; parameter < this->_global_condition_size; parameter++)
  {
    if (!std::isfinite(params[parameter]))
      throw std::invalid_argument("Global conditioning values must be finite");
    const float value = static_cast<float>(params[parameter]);
    this->_condition_output.row(base_condition_rows + parameter).head(num_frames).setConstant(value);
  }

  // Main layer arrays:
  // Layer-to-layer
  for (size_t i = 0; i < this->_layer_arrays.size(); i++)
  {
    if (i == 0)
    {
      // First layer array - no head input
      // layer_inputs should be the original input (before condition_dsp processing),
      // condition should be the processed condition output (after condition_dsp)
      this->_layer_arrays[i].Process(this->_condition_input, this->_condition_output, num_frames);
    }
    else
    {
      // Subsequent layer arrays - use outputs from previous layer array.
      // Pass full buffers and slice inside the callee to avoid passing Blocks
      // across API boundaries (which can cause Eigen to allocate temporaries).
      Eigen::MatrixXf& prev_layer_outputs = this->_layer_arrays[i - 1].GetLayerOutputs();
      Eigen::MatrixXf& prev_head_outputs = this->_layer_arrays[i - 1].GetHeadOutputs();
      this->_layer_arrays[i].Process(prev_layer_outputs, this->_condition_output, prev_head_outputs, num_frames);
    }
  }

  // (Head not implemented)

  auto& final_head_outputs = this->_layer_arrays.back().GetHeadOutputs();
  assert(final_head_outputs.rows() == out_channels);

  for (int ch = 0; ch < out_channels; ch++)
  {
    for (int s = 0; s < num_frames; s++)
    {
      const float out = this->_head_scale * final_head_outputs(ch, s);
      output[ch][s] = out;
    }
  }
}

// Factory to instantiate from nlohmann json
std::unique_ptr<nam::DSP> nam::wavenet::Factory(const nlohmann::json& config, std::vector<float>& weights,
                                                const double expectedSampleRate)
{
  const int global_condition_size = config.value("global_condition_size", 0);
  if (global_condition_size < 0 || global_condition_size > kMaxGlobalConditionSize)
    throw std::invalid_argument("global_condition_size is outside the supported range");
  std::unique_ptr<nam::DSP> condition_dsp = nullptr;
  if (config.find("condition_dsp") != config.end())
  {
    const nlohmann::json& condition_dsp_json = config["condition_dsp"];
    condition_dsp = nam::get_dsp(condition_dsp_json);
    if (condition_dsp->GetExpectedSampleRate() != expectedSampleRate)
    {
      std::stringstream ss;
      ss << "Condition DSP expected sample rate (" << condition_dsp->GetExpectedSampleRate()
         << ") doesn't match WaveNet expected sample rate (" << expectedSampleRate << "!\n";
      throw std::runtime_error(ss.str().c_str());
    }
  }
  std::vector<nam::wavenet::LayerArrayParams> layer_array_params;
  for (size_t i = 0; i < config["layers"].size(); i++)
  {
    nlohmann::json layer_config = config["layers"][i];

    const int groups = layer_config.value("groups_input", 1); // defaults to 1
    const int groups_input_mixin = layer_config.value("groups_input_mixin", 1); // defaults to 1
    const int groups_1x1 = layer_config.value("groups_1x1", 1); // defaults to 1

    const int channels = layer_config["channels"];
    const int bottleneck = layer_config.value("bottleneck", channels); // defaults to channels if not present

    const int input_size = layer_config["input_size"];
    const int layer_global_condition_size = layer_config.value("global_condition_size", global_condition_size);
    if (layer_global_condition_size != global_condition_size)
      throw std::invalid_argument("Inconsistent global_condition_size in WaveNet layer configuration");
    const int base_condition_size = layer_config["condition_size"].get<int>();
    if (base_condition_size < 0 || base_condition_size > std::numeric_limits<int>::max() - global_condition_size)
      throw std::invalid_argument("WaveNet condition_size is outside the supported range");
    const int condition_size = base_condition_size + global_condition_size;
    const int head_size = layer_config["head_size"];
    const int kernel_size = layer_config["kernel_size"];
    const auto dilations = layer_config["dilations"];
    // Parse JSON into typed ActivationConfig at model loading boundary
    const activations::ActivationConfig activation_config =
      activations::ActivationConfig::from_json(layer_config["activation"]);
    // Parse gating mode - support both old "gated" boolean and new "gating_mode" string
    GatingMode gating_mode = GatingMode::NONE;
    activations::ActivationConfig secondary_activation_config;

    if (layer_config.find("gating_mode") != layer_config.end())
    {
      std::string gating_mode_str = layer_config["gating_mode"].get<std::string>();
      if (gating_mode_str == "gated")
      {
        gating_mode = GatingMode::GATED;
        secondary_activation_config = activations::ActivationConfig::from_json(layer_config["secondary_activation"]);
      }
      else if (gating_mode_str == "blended")
      {
        gating_mode = GatingMode::BLENDED;
        secondary_activation_config = activations::ActivationConfig::from_json(layer_config["secondary_activation"]);
      }
      else if (gating_mode_str == "none")
      {
        gating_mode = GatingMode::NONE;
        // Leave secondary_activation_config with empty type
      }
      else
        throw std::runtime_error("Invalid gating_mode: " + gating_mode_str);
    }
    else if (layer_config.find("gated") != layer_config.end())
    {
      // Backward compatibility: convert old "gated" boolean to new enum
      bool gated = layer_config["gated"];
      gating_mode = gated ? GatingMode::GATED : GatingMode::NONE;
      if (gated)
      {
        secondary_activation_config = activations::ActivationConfig::simple(activations::ActivationType::Sigmoid);
      }
      // else: leave secondary_activation_config uninitialized
    }
    else
    {
      throw std::invalid_argument("No information on gating mode found for layer array " + std::to_string(i));
    }

    const bool head_bias = layer_config["head_bias"];

    // Parse head1x1 parameters
    bool head1x1_active = false;
    int head1x1_out_channels = channels;
    int head1x1_groups = 1;
    if (layer_config.find("head_1x1") != layer_config.end())
    {
      const auto& head1x1_config = layer_config["head_1x1"];
      head1x1_active = head1x1_config["active"];
      head1x1_out_channels = head1x1_config["out_channels"];
      head1x1_groups = head1x1_config["groups"];
    }
    nam::wavenet::Head1x1Params head1x1_params(head1x1_active, head1x1_out_channels, head1x1_groups);

    // Helper function to parse FiLM parameters
    auto parse_film_params = [&layer_config](const std::string& key) -> nam::wavenet::_FiLMParams {
      if (layer_config.find(key) == layer_config.end() || layer_config[key] == false)
      {
        return nam::wavenet::_FiLMParams(false, false);
      }
      const nlohmann::json& film_config = layer_config[key];
      bool active = film_config.value("active", true);
      bool shift = film_config.value("shift", true);
      return nam::wavenet::_FiLMParams(active, shift);
    };

    // Parse FiLM parameters
    nam::wavenet::_FiLMParams conv_pre_film_params = parse_film_params("conv_pre_film");
    nam::wavenet::_FiLMParams conv_post_film_params = parse_film_params("conv_post_film");
    nam::wavenet::_FiLMParams input_mixin_pre_film_params = parse_film_params("input_mixin_pre_film");
    nam::wavenet::_FiLMParams input_mixin_post_film_params = parse_film_params("input_mixin_post_film");
    nam::wavenet::_FiLMParams activation_pre_film_params = parse_film_params("activation_pre_film");
    nam::wavenet::_FiLMParams activation_post_film_params = parse_film_params("activation_post_film");
    nam::wavenet::_FiLMParams _1x1_post_film_params = parse_film_params("1x1_post_film");
    nam::wavenet::_FiLMParams head1x1_post_film_params = parse_film_params("head1x1_post_film");

    layer_array_params.push_back(nam::wavenet::LayerArrayParams(
      input_size, condition_size, head_size, channels, bottleneck, kernel_size, dilations, activation_config,
      gating_mode, head_bias, groups, groups_input_mixin, groups_1x1, head1x1_params, secondary_activation_config,
      conv_pre_film_params, conv_post_film_params, input_mixin_pre_film_params, input_mixin_post_film_params,
      activation_pre_film_params, activation_post_film_params, _1x1_post_film_params, head1x1_post_film_params));
  }
  const bool with_head = !config["head"].is_null();
  const float head_scale = config["head_scale"];

  if (layer_array_params.empty())
    throw std::runtime_error("WaveNet config requires at least one layer array");

  // Backward compatibility: assume 1 input channel
  const int in_channels = config.value("in_channels", 1);

  // out_channels is determined from last layer array's head_size
  return std::make_unique<nam::wavenet::WaveNet>(
    in_channels, layer_array_params, head_scale, with_head, weights, std::move(condition_dsp), expectedSampleRate,
    global_condition_size);
}

// Register the factory
namespace
{
static nam::factory::Helper _register_WaveNet("WaveNet", nam::wavenet::Factory);
}

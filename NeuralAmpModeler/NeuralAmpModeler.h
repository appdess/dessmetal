#pragma once

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <string>
#include "../AudioDSPTools/dsp/ImpulseResponse.h"
#include "../AudioDSPTools/dsp/NoiseGate.h"
#include "../AudioDSPTools/dsp/dsp.h"
#include "../AudioDSPTools/dsp/wav.h"
#include "../AudioDSPTools/dsp/ResamplingContainer/ResamplingContainer.h"
#include "../NeuralAmpModelerCore/NAM/dsp.h"


#include "Colors.h"
#include "ToneStack.h"

#include "IPlug_include_in_plug_hdr.h"
#include "ISender.h"


const int kNumPresets = 1;
// The plugin is mono inside
constexpr size_t kNumChannelsInternal = 1;

class NAMSender : public iplug::IPeakAvgSender<>
{
public:
  NAMSender()
  : iplug::IPeakAvgSender<>(-90.0, true, 5.0f, 1.0f, 300.0f, 500.0f)
  {
  }
};

enum EParams
{
  kInputLevel = 0,
  kGain,  // Gain knob (1-10) that loads different NAM models
  kNoiseGateThreshold,
  kToneBass,
  kToneMid,
  kToneTreble,
  kOutputLevel,
  kBoostLevel, // Output/Level knob for Boost
  kBoostTone,  // Tone knob for Boost (dummy for now, future parametric)
  kNoiseGateActive,
  kEQActive,
  kBoostActive, // Toggle for Boost
  kBoostOutput,
  kIRToggle,
  kBoostModel, // Boost Loader Enum
  kCalibrateInput,
  kInputCalibrationLevel,
  kOutputMode,
  // Keep the historical five host-visible states for project compatibility.
  // The unshipped DessBlock-red slot falls back to the red model, while index 4
  // restores the owner-authored SickDess capture.
  kAmpModel,
  kNAMActive, // Bypass NAM Processing
  kNumParams
};

enum ECtrlTags
{
  kCtrlTagModelFileBrowser = 0,
  kCtrlTagIRFileBrowser,
  kCtrlTagBoostModelFileBrowser, // Added for Boost NAM
  kCtrlTagInputMeter,
  kCtrlTagOutputMeter,
  kCtrlTagSettingsBox,
  kCtrlTagOutputMode,
  kCtrlTagCalibrateInput,
  kCtrlTagInputCalibrationLevel,
  kCtrlTagBackground,
  kCtrlTagAmpModel,
  kNumCtrlTags
};

enum EMsgTags
{
  // These tags are used from UI -> DSP
  kMsgTagClearModel = 0,
  kMsgTagClearIR,
  kMsgTagClearBoostModel, // Added
  kMsgTagHighlightColor,
  // The following tags are from DSP -> UI
  kMsgTagLoadFailed,
  kMsgTagLoadedModel,
  kMsgTagLoadedIR,
  kMsgTagLoadedBoostModel, // Added
  kNumMsgTags
};

// Get the sample rate of a NAM model.
// Sometimes, the model doesn't know its own sample rate; this wrapper guesses 48k based on the way that most
// people have used NAM in the past.
double GetNAMSampleRate(const std::unique_ptr<nam::DSP>& model)
{
  // Some models are from when we didn't have sample rate in the model.
  // For those, this wraps with the assumption that they're 48k models, which is probably true.
  const double assumedSampleRate = 48000.0;
  const double reportedEncapsulatedSampleRate = model->GetExpectedSampleRate();
  const double encapsulatedSampleRate =
    reportedEncapsulatedSampleRate <= 0.0 ? assumedSampleRate : reportedEncapsulatedSampleRate;
  return encapsulatedSampleRate;
};

class ResamplingNAM : public nam::DSP
{
public:
  // Resampling wrapper around the NAM models
  ResamplingNAM(std::unique_ptr<nam::DSP> encapsulated, const double expected_sample_rate)
  : nam::DSP(1, 1, expected_sample_rate)
  , mEncapsulated(std::move(encapsulated))
  , mResampler(GetNAMSampleRate(mEncapsulated))
  {
    // Get the other information from the encapsulated NAM so that we can tell the outside world about what we're
    // holding.
    if (mEncapsulated->HasLoudness())
    {
      SetLoudness(mEncapsulated->GetLoudness());
    }
    if (mEncapsulated->HasInputLevel())
    {
      SetInputLevel(mEncapsulated->GetInputLevel());
    }
    if (mEncapsulated->HasOutputLevel())
    {
      SetOutputLevel(mEncapsulated->GetOutputLevel());
    }

    // NOTE: prewarm samples doesn't mean anything--we can prewarm the encapsulated model as it likes and be good to
    // go.
    // _prewarm_samples = 0;

  };

  ~ResamplingNAM() = default;

  void prewarm() override { mEncapsulated->prewarm(); };

  void process(NAM_SAMPLE** input, NAM_SAMPLE** output, const int num_frames) override
  {
    if (num_frames > mMaxExternalBlockSize)
    {
      std::fill(output[0], output[0] + std::max(num_frames, 0), 0.0);
      return;
    }

    if (!NeedToResample())
    {
      mEncapsulated->process(input, output, num_frames);
    }
    else
    {
      mResampler.ProcessBlock(input, output, num_frames, [&](NAM_SAMPLE** resampledInput, NAM_SAMPLE** resampledOutput,
                                                            int resampledFrames) {
        mEncapsulated->process(resampledInput, resampledOutput, resampledFrames);
      });
    }
  };

  void process(NAM_SAMPLE** input, NAM_SAMPLE** output, const int num_frames, const double* params, const int num_params) override
  {
    if (num_frames > mMaxExternalBlockSize)
    {
      std::fill(output[0], output[0] + std::max(num_frames, 0), 0.0);
      return;
    }

    if (!NeedToResample())
    {
      mEncapsulated->process(input, output, num_frames, params, num_params);
    }
    else
    {
      // Resampling with parameters is tricky if params change per sample, 
      // but here they are block-constant.
      // However, mResampler.ProcessBlock takes a lambda.
      // We need to capture params.
      auto ProcessBlockFunc = [&](NAM_SAMPLE** input, NAM_SAMPLE** output, int numFrames) {
        mEncapsulated->process(input, output, numFrames, params, num_params);
      };
       mResampler.ProcessBlock(input, output, num_frames, ProcessBlockFunc);
    }
  };

  // Keep the same resampling path and therefore the same reported latency when
  // the amp itself is bypassed. This avoids a host PDC transition in the middle
  // of render-thread automation while still bypassing the neural network.
  void processBypass(NAM_SAMPLE** input, NAM_SAMPLE** output, const int num_frames)
  {
    if (num_frames > mMaxExternalBlockSize)
    {
      std::fill(output[0], output[0] + std::max(num_frames, 0), 0.0);
      return;
    }

    if (!NeedToResample())
    {
      std::copy(input[0], input[0] + num_frames, output[0]);
      return;
    }

    mResampler.ProcessBlock(input, output, num_frames,
                            [](NAM_SAMPLE** resampledInput, NAM_SAMPLE** resampledOutput, int resampledFrames) {
                              std::copy(resampledInput[0], resampledInput[0] + resampledFrames,
                                        resampledOutput[0]);
                            });
  }

  int GetLatency() const { return NeedToResample() ? mResampler.GetLatency() : 0; };

  void SetDefaultParams(const double* params, const int numParams) override
  {
    mEncapsulated->SetDefaultParams(params, numParams);
  }

  void SetPreparedEpoch(const std::uint64_t epoch) { mPreparedEpoch = epoch; }
  std::uint64_t GetPreparedEpoch() const { return mPreparedEpoch; }
  void SetPreparedRequest(const unsigned int token, const int selection)
  {
    mPreparedRequestToken = token;
    mPreparedSelection = selection;
  }
  unsigned int GetPreparedRequestToken() const { return mPreparedRequestToken; }
  int GetPreparedSelection() const { return mPreparedSelection; }
  void SetPreparedPath(const char* path) { mPreparedPath = path == nullptr ? "" : path; }
  const std::string& GetPreparedPath() const { return mPreparedPath; }

  void Reset(const double sampleRate, const int maxBlockSize) override
  {
    mExpectedSampleRate = sampleRate;
    mMaxExternalBlockSize = maxBlockSize;
    mResampler.Reset(sampleRate, maxBlockSize);

    // Allocations in the encapsulated model (HACK)
    // Stolen some code from the resampler; it'd be nice to have these exposed as methods? :)
    const double mUpRatio = sampleRate / GetEncapsulatedSampleRate();
    const auto maxEncapsulatedBlockSize = static_cast<int>(std::ceil(static_cast<double>(maxBlockSize) / mUpRatio));
    // nam::DSP::Reset() currently prewarms; do not invoke the wrapper that
    // would prewarm the same model a second time.
    mEncapsulated->Reset(GetEncapsulatedSampleRate(), maxEncapsulatedBlockSize);
  };

  // So that we can let the world know if we're resampling (useful for debugging)
  double GetEncapsulatedSampleRate() const { return GetNAMSampleRate(mEncapsulated); };

private:
  bool NeedToResample() const { return GetExpectedSampleRate() != GetEncapsulatedSampleRate(); };
  // The encapsulated NAM
  std::unique_ptr<nam::DSP> mEncapsulated;

  // The resampling wrapper
  dsp::ResamplingContainer<NAM_SAMPLE, 1, 12> mResampler;

  // Used to check that we don't get too large a block to process.
  int mMaxExternalBlockSize = 0;
  std::uint64_t mPreparedEpoch = 0;
  unsigned int mPreparedRequestToken = 0;
  int mPreparedSelection = -1;
  std::string mPreparedPath;

};

class NeuralAmpModeler final : public iplug::Plugin
{
public:
  NeuralAmpModeler(const iplug::InstanceInfo& info);
  ~NeuralAmpModeler();

  void ProcessBlock(iplug::sample** inputs, iplug::sample** outputs, int nFrames) override;
  void OnReset() override;
  void OnIdle() override;
  void OnRestoreState() override;

  bool SerializeState(iplug::IByteChunk& chunk) const override;
  int UnserializeState(const iplug::IByteChunk& chunk, int startPos) override;
  void OnUIOpen() override;
  bool OnHostRequestingSupportedViewConfiguration(int width, int height) override { return true; }

  void OnParamChange(int paramIdx) override;
  void OnParamChangeUI(int paramIdx, iplug::EParamSource source) override;
  bool OnMessage(int msgTag, int ctrlTag, int dataSize, const void* pData) override;

private:
  // Allocates mInputPointers and mOutputPointers
  void _AllocateIOPointers(const size_t nChans);
  // Atomically adopts fully constructed DSP modules on the render thread and
  // defers reclamation to the serialized non-realtime loader service.
  void _ApplyDSPStaging(bool ampActiveForBlock);
  // Copies immutable facts from an adopted model into a lock-free cache.
  // The request token is stored last as the release publication word.
  void _PublishModelMetadata(ResamplingNAM& model) noexcept;
  // Loads or removes the latest requested modules. Called by OnIdle for
  // realtime operation and synchronously by offline rendering, never by the
  // realtime render path.
  void _ServiceDSPRequests();
  // Deallocates mInputPointers and mOutputPointers
  void _DeallocateIOPointers();
  // Fallback that just copies inputs to outputs if mDSP doesn't hold a model.
  void _FallbackDSP(iplug::sample** inputs, iplug::sample** outputs, const size_t numChannels, const size_t numFrames);
  // Sizes based on mInputArray
  size_t _GetBufferNumChannels() const;
  size_t _GetBufferNumFrames() const;
  void _InitToneStack();
  void _InitBoost();
  // Apply Boost (now NAM-based)
  void _ApplyBoost(iplug::sample** inputs, const size_t numChannels, const size_t numFrames);
  // Helper to get amp model name from parameter value
  std::string _GetAmpModelName(int ampModelValue);
  // Loads a NAM model from embedded resources based on amp model and gain value (1-10)
  // Returns an empty string on success, or an error message on failure.
  std::string _LoadModelForGain(const std::string& ampModel, int gainValue, unsigned int requestToken,
                                int requestedAmpIndex);
  // Loads a NAM model and stores it to mStagedNAM
  // ampModel: "DessBlock" or "DessCut"
  // gainValue: 1-10
  // Returns an empty string on success, or an error message on failure.
  std::string _StageModel(const WDL_String& dspFile, unsigned int requestToken, int requestedAmpIndex);
  // Boost model helpers
  std::string _GetBoostModelName(int index);
  std::string _LoadBoostModel(const std::string& modelName, unsigned int requestToken, int requestedBoostIndex);
  std::string _StageBoostModel(const WDL_String& dspFile, unsigned int requestToken, int requestedBoostIndex);
  // Loads an IR completely, then publishes it to the atomic pending slot.
  // Return status code so that error messages can be relayed if
  // it wasn't successful.
  dsp::wav::LoadReturnCode _StageIR(const WDL_String& irPath, unsigned int requestToken);

  bool _HaveModel() const { return mModel.load(std::memory_order_acquire) != nullptr; };
  // Prepare the input & output buffers
  void _PrepareBuffers(const size_t numChannels, const size_t numFrames);
  // Manage pointers
  void _PrepareIOPointers(const size_t nChans);
  // Copy the input buffer to the object, applying input level.
  // :param nChansIn: In from external
  // :param nChansOut: Out to the internal of the DSP routine
  void _ProcessInput(iplug::sample** inputs, const size_t nFrames, const size_t nChansIn, const size_t nChansOut);
  // Copy the output to the output buffer, applying output level.
  // :param nChansIn: In from internal
  // :param nChansOut: Out to external
  void _ProcessOutput(iplug::sample** inputs, iplug::sample** outputs, const size_t nFrames, const size_t nChansIn,
                      const size_t nChansOut);
  void _SetInputGain();
  void _SetOutputGain();
  WDL_String _GetNAMPathSnapshot() const;
  WDL_String _GetIRPathSnapshot() const;
  WDL_String _GetBoostNAMPathSnapshot() const;
  void _SetNAMPath(const char* path);
  void _SetIRPath(const char* path);
  void _SetIRPathAndRequest(const char* path);
  void _RequestIRReload();
  void _SetBoostNAMPath(const char* path);

  // See: Unserialization.cpp
  void _UnserializeApplyConfig(nlohmann::json& config);
  // 0.7.9 and later
  int _UnserializeStateWithKnownVersion(const iplug::IByteChunk& chunk, int startPos);
  // Hopefully 0.7.3-0.7.8, but no gurantees
  int _UnserializeStateWithUnknownVersion(const iplug::IByteChunk& chunk, int startPos);

  // Validate model-dependent parameters from a non-render thread and keep the
  // host-visible value aligned with the effective DSP behavior.
  void _ValidateOutputModeForCurrentModel();
  // Apply a normalized value from OnIdle as a complete host-visible gesture.
  void _SetParameterValueFromMainThread(int paramIdx, double normalizedValue);

  // Update all custom-UI controls that depend on a model.
  void _UpdateControlsFromModel();

  // Make sure that the latency is reported correctly.
  void _UpdateLatency();

  // Update level meters
  // Called within ProcessBlock().
  // Assume _ProcessInput() and _ProcessOutput() were run immediately before.
  void _UpdateMeters(iplug::sample** inputPointer, iplug::sample** outputPointer, const size_t nFrames,
                     const size_t nChansIn, const size_t nChansOut);

  // Member data

  // Input arrays to NAM
  std::vector<std::vector<iplug::sample>> mInputArray;
  // Output from NAM
  std::vector<std::vector<iplug::sample>> mOutputArray;
  // Boost Output
  std::vector<std::vector<iplug::sample>> mBoostOutputArray;
  // Pointer versions
  iplug::sample** mInputPointers = nullptr;
  iplug::sample** mOutputPointers = nullptr;
  iplug::sample** mBoostOutputPointers = nullptr;

  // Input and output gain
  std::atomic<double> mInputGain{1.0};
  std::atomic<double> mOutputGain{1.0};
  // Immutable model facts are copied by OnIdle while lifetime is protected,
  // then published for lock-free parameter callbacks. Request token is the
  // release/acquire publication word and invalidates the cache on every load.
  std::atomic<std::uint64_t> mModelMetadataEpoch{0};
  std::atomic<unsigned int> mModelMetadataRequest{0};
  std::atomic<int> mModelMetadataSelection{-1};
  std::atomic<int> mModelMetadataLatency{0};
  std::atomic<bool> mModelMetadataHasInputLevel{false};
  std::atomic<bool> mModelMetadataHasOutputLevel{false};
  std::atomic<bool> mModelMetadataHasLoudness{false};
  std::atomic<double> mModelMetadataInputLevel{0.0};
  std::atomic<double> mModelMetadataOutputLevel{0.0};
  std::atomic<double> mModelMetadataLoudness{0.0};
  std::atomic<double> mModelMetadataSampleRate{0.0};
  // Host automation and generic editors can select a model-dependent output
  // mode while OnParamChange is running on the render thread. Defer capability
  // validation and any host notification to OnIdle().
  std::atomic<bool> mOutputModeValidationPending{false};
  // State migration can canonicalize host-visible values while the host is
  // still inside its restore callback. Republish them from the next OnIdle.
  std::atomic<bool> mHostStateRepublishPending{false};

  // Noise gates
  dsp::noise_gate::Trigger mNoiseGateTrigger;
  dsp::noise_gate::Gain mNoiseGateGain;
  // Live DSP pointers are owned by the plug-in, but are intentionally raw:
  // ProcessBlock may swap them without running a destructor. Producers publish
  // complete objects through the atomic pending slots; the serialized loader
  // service reclaims objects returned through the retired slots.
  std::atomic<ResamplingNAM*> mModel{nullptr};
  std::atomic<dsp::ImpulseResponse*> mIR{nullptr};
  std::atomic<ResamplingNAM*> mPendingModel{nullptr};
  std::atomic<ResamplingNAM*> mRetiredModel{nullptr};
  std::atomic<dsp::ImpulseResponse*> mPendingIR{nullptr};
  std::atomic<dsp::ImpulseResponse*> mRetiredIR{nullptr};
  // Flags to take away the modules at a safe time.
  std::atomic<bool> mShouldRemoveModel = false;
  std::atomic<bool> mShouldRemoveIR = false;
  std::atomic<bool> mShouldRemoveBoostModel = false;

  // The boost model uses the same publish/adopt/retire ownership protocol.
  std::atomic<ResamplingNAM*> mBoostModel{nullptr};
  std::atomic<ResamplingNAM*> mPendingBoostModel{nullptr};
  std::atomic<ResamplingNAM*> mRetiredBoostModel{nullptr};

  std::atomic<bool> mNewModelLoadedInDSP = false;
  std::atomic<bool> mNewIRLoadedInDSP = false;
  std::atomic<bool> mModelCleared = false;
  std::atomic<bool> mNewBoostModelLoadedInDSP = false;
  std::atomic<bool> mBoostModelCleared = false;
  std::atomic<unsigned int> mAmpLoadRequest{0};
  std::atomic<unsigned int> mBoostLoadRequest{0};
  std::atomic<unsigned int> mIRLoadRequest{0};
  std::atomic<unsigned int> mHandledAmpLoadRequest{0};
  std::atomic<unsigned int> mHandledBoostLoadRequest{0};
  std::atomic<unsigned int> mHandledIRLoadRequest{0};
  std::atomic<bool> mAmpDisableNotificationPending{false};
  std::atomic<bool> mBoostDisableNotificationPending{false};
  std::atomic<bool> mIRDisableNotificationPending{false};
  // A load failure leaves the amp bypassed. The next explicit AMP ON request
  // consumes this flag and retries without turning ordinary bypass automation
  // into disk I/O.
  std::atomic<bool> mAmpRetryNeeded{false};
  // Accessed only while holding mDSPLoadMutex. Loader failures are presented
  // by OnIdle after releasing that mutex so modal UI cannot block loading.
  std::string mDeferredAmpLoadError;
  std::string mDeferredBoostLoadError;
  std::string mDeferredIRLoadError;
  WDL_String mBoostNAMPath;

  // Tone stack modules
  std::unique_ptr<dsp::tone_stack::AbstractToneStack> mToneStack;
  


  // Post-IR filters
  recursive_linear_filter::HighPass mHighPass;
  //  recursive_linear_filter::LowPass mLowPass;

  // Path to model's config.json or model.nam
  WDL_String mNAMPath;
  // Path to IR (.wav file)
  WDL_String mIRPath;

  WDL_String mHighLightColor{PluginColors::NAM_THEMECOLOR.ToColorCode()};

  std::unordered_map<std::string, double> mNAMParams = {{"Input", 0.0}, {"Output", 0.0}};

  std::atomic<int> mAmpModelIdx{0};
  std::atomic<bool> mAmpActiveTarget{true};
  std::atomic<int> mBoostModelIdx{0};
  std::atomic<bool> mBoostActiveTarget{false};
  std::atomic<bool> mIRActiveTarget{true};
  std::atomic<double> mTargetGain{0.5};
  std::atomic<double> mAudioSampleRate{48000.0};
  std::atomic<int> mAudioMaxBlockSize{2048};
  std::atomic<std::uint64_t> mAudioConfigEpoch{0};
  std::atomic<bool> mAudioConfigInitialized{false};
  std::atomic<double> mToneBassTarget{5.0};
  std::atomic<double> mToneMidTarget{5.0};
  std::atomic<double> mToneTrebleTarget{5.0};
  double mAppliedToneBass = 5.0;
  double mAppliedToneMid = 5.0;
  double mAppliedToneTreble = 5.0;
  mutable std::mutex mPathMutex;
  std::mutex mDSPLoadMutex;
  std::vector<double> mCurrentParams;
  NAMSender mInputSender, mOutputSender;
};

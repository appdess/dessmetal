#include <algorithm> // std::clamp, std::min
#include <cmath> // pow
#include <filesystem>
#include <iostream>
#include <sstream> // std::stringstream
#include <utility>
#include <vector>

#include "Colors.h"
#include "../NeuralAmpModelerCore/NAM/activations.h"
#include "../NeuralAmpModelerCore/NAM/get_dsp.h"
// clang-format off
// These includes need to happen in this order or else the latter won't know
// a bunch of stuff.
#include "NeuralAmpModeler.h"
#include "IPlug_include_in_plug_src.h"
// clang-format on
#include "architecture.hpp"
#include "../iPlug2/IPlug/IPlugPaths.h"

#include "NeuralAmpModelerControls.h"

namespace
{
int GetBoundedChunkString(const IByteChunk& chunk, WDL_String& output, const int startPos, const int maxLength)
{
  if (startPos < 0 || maxLength < 0 || startPos > chunk.Size() - static_cast<int>(sizeof(int)))
    return -1;

  int length = 0;
  const int stringStart = chunk.Get(&length, startPos);
  if (stringStart < 0 || length < 0 || length > maxLength || length > chunk.Size() - stringStart)
    return -1;

  if (length == 0)
    output.Set("");
  else
    output.Set(reinterpret_cast<const char*>(chunk.GetData() + stringStart), length);
  return stringStart + length;
}

std::string GetBundledDefaultIRPath(const char* bundleId)
{
  WDL_String resourcePath;
  BundleResourcePath(resourcePath, bundleId);

  std::filesystem::path irDir(resourcePath.Get());
  irDir /= "IRs";
  if (!std::filesystem::exists(irDir))
    return "";

  std::vector<std::filesystem::path> irFiles;
  for (const auto& entry : std::filesystem::directory_iterator(irDir))
  {
    if (entry.is_regular_file())
    {
      const auto ext = entry.path().extension().string();
      if (ext == ".wav" || ext == ".WAV")
        irFiles.push_back(entry.path());
    }
  }

  if (irFiles.empty())
    return "";

  // Prefer beasty-trio.wav if it exists
  for (const auto& irFile : irFiles)
  {
    if (irFile.filename().string() == "beasty-trio.wav")
      return irFile.string();
  }

  std::sort(irFiles.begin(), irFiles.end());
  return irFiles.front().string();
}

double GetVisibleAmpModelValue(const int modelIndex)
{
  switch (modelIndex)
  {
    case 1:
    case 3: return 1.0 / 3.0;
    case 2: return 2.0 / 3.0;
    case 4: return 1.0;
    default: return 0.0;
  }
}

int GetHostAmpModelIndex(const int visibleIndex)
{
  constexpr int hostIndices[] = {0, 1, 2, 4};
  return hostIndices[std::clamp(visibleIndex, 0, 3)];
}
} // namespace

using namespace iplug;
using namespace igraphics;

const double kDCBlockerFrequency = 5.0;

// Styles
const IVColorSpec colorSpec{
  DEFAULT_BGCOLOR, // Background
  PluginColors::NAM_THEMECOLOR, // Foreground
  PluginColors::NAM_THEMECOLOR.WithOpacity(0.3f), // Pressed
  PluginColors::NAM_THEMECOLOR.WithOpacity(0.4f), // Frame
  PluginColors::MOUSEOVER, // Highlight
  DEFAULT_SHCOLOR, // Shadow
  PluginColors::NAM_THEMECOLOR, // Extra 1
  COLOR_RED, // Extra 2 --> color for clipping in meters
  PluginColors::NAM_THEMECOLOR.WithContrast(0.1f), // Extra 3
};

const IVStyle style =
  IVStyle{true, // Show label
          true, // Show value
          colorSpec,
          {DEFAULT_TEXT_SIZE + 10.f, EVAlign::Middle, PluginColors::NAM_THEMEFONTCOLOR}, // Knob label text
          {DEFAULT_TEXT_SIZE + 10.f, EVAlign::Bottom, PluginColors::NAM_THEMEFONTCOLOR}, // Knob value text
          DEFAULT_HIDE_CURSOR,
          DEFAULT_DRAW_FRAME,
          false,
          DEFAULT_EMBOSS,
          0.2f,
          2.f,
          DEFAULT_SHADOW_OFFSET,
          DEFAULT_WIDGET_FRAC,
          DEFAULT_WIDGET_ANGLE};
const IVStyle radioButtonStyle =
  style
    .WithColor(EVColor::kON, PluginColors::NAM_THEMECOLOR) // Pressed buttons and their labels
    .WithColor(EVColor::kOFF, PluginColors::NAM_THEMECOLOR.WithOpacity(0.1f)) // Unpressed buttons
    .WithColor(EVColor::kX1, PluginColors::NAM_THEMECOLOR.WithOpacity(0.6f)); // Unpressed buttons' labels

EMsgBoxResult _ShowMessageBox(iplug::igraphics::IGraphics* pGraphics, const char* str, const char* caption,
                              EMsgBoxType type)
{
#ifdef OS_MAC
  // macOS is backwards?
  return pGraphics->ShowMessageBox(caption, str, type);
#else
  return pGraphics->ShowMessageBox(str, caption, type);
#endif
}

const std::string kCalibrateInputParamName = "CalibrateInput";
const bool kDefaultCalibrateInput = false;
const std::string kInputCalibrationLevelParamName = "InputCalibrationLevel";
const double kDefaultInputCalibrationLevel = 12.0;


NeuralAmpModeler::NeuralAmpModeler(const InstanceInfo& info)
: Plugin(info, MakeConfig(kNumParams, kNumPresets))
{
  _InitToneStack();
  _InitBoost();
  nam::activations::Activation::enable_fast_tanh();
  GetParam(kInputLevel)->InitGain("Input", 0.0, -20.0, 20.0, 0.1);
  GetParam(kGain)->InitDouble("Gain", 5.0, 2.0, 8.0, 0.1, ""); // Restricted to trained range (2-8)
  GetParam(kAmpModel)
    ->InitEnum("Amp Model", 0,
               {"DessTortion-blue", "DessTortion-red", "DessBlock-green", "DessBlock-red", "SickDess"});
  GetParam(kToneBass)->InitDouble("Bass", 5.0, 0.0, 10.0, 0.1);
  GetParam(kToneMid)->InitDouble("Middle", 5.0, 0.0, 10.0, 0.1);
  GetParam(kToneTreble)->InitDouble("Treble", 5.0, 0.0, 10.0, 0.1);
  GetParam(kOutputLevel)->InitGain("Output", 0.0, -40.0, 40.0, 0.1);
  GetParam(kBoostLevel)->InitGain("Boost Level", 0.0, -20.0, 20.0, 0.1);
  GetParam(kBoostTone)->InitDouble("Boost Tone", 5.0, 0.0, 10.0, 0.1); // Dummy for now, future parametric
  GetParam(kNoiseGateThreshold)->InitGain("Threshold", -80.0, -100.0, 0.0, 0.1);
  GetParam(kNoiseGateActive)->InitBool("NoiseGateActive", true);
  GetParam(kEQActive)->InitBool("ToneStack", false);
  // Raw is the universally safe default. Model-aware options are enabled only
  // when the selected capture publishes the required metadata.
  GetParam(kOutputMode)->InitEnum("OutputMode", 0, {"Raw", "Normalized", "Calibrated"}); // TODO DRY w/ control
  GetParam(kIRToggle)->InitBool("IRToggle", true);
  GetParam(kCalibrateInput)->InitBool(kCalibrateInputParamName.c_str(), kDefaultCalibrateInput);
  GetParam(kInputCalibrationLevel)
    ->InitDouble(kInputCalibrationLevelParamName.c_str(), kDefaultInputCalibrationLevel, -60.0, 60.0, 0.1, "dBu");

  // Drive Params. The restored fixed captures use Boost Level for their actual
  // post-model trim. Boost Tone and BoostOutput remain host-visible only for
  // compatibility with older projects; neither is exposed as a working knob.
  GetParam(kBoostActive)->InitBool("Boost", true);
  GetParam(kBoostOutput)->InitDouble("BoostOutput", 5.0, 0.0, 10.0, 0.1);
  GetParam(kBoostModel)->InitEnum("Boost Model", 0, {"OD808", "SD1", "TS9", "aesahaettr"});

  GetParam(kNAMActive)->InitBool("Amp Enabled", true); // Default Amp On
  GetParam(kTransposeSemitones)->InitInt("Transpose", 0, -12, 12, "st");
  GetParam(kTransposeSemitones)->SetDisplayFunc([](const double value, WDL_String& display) {
    const int semitones = static_cast<int>(std::lround(value));
    if (semitones > 0)
      display.SetFormatted(16, "+%d", semitones);
    else
      display.SetFormatted(16, "%d", semitones);
  });

  mAmpModelIdx.store(GetParam(kAmpModel)->Int(), std::memory_order_relaxed);
  mAmpActiveTarget.store(GetParam(kNAMActive)->Bool(), std::memory_order_relaxed);
  mBoostModelIdx.store(GetParam(kBoostModel)->Int(), std::memory_order_relaxed);
  mBoostActiveTarget.store(GetParam(kBoostActive)->Bool(), std::memory_order_relaxed);
  mIRActiveTarget.store(GetParam(kIRToggle)->Bool(), std::memory_order_relaxed);
  mTransposeSemitones.store(GetParam(kTransposeSemitones)->Int(), std::memory_order_relaxed);

  mNoiseGateTrigger.AddListener(&mNoiseGateGain);
  mCurrentParams.resize(1, 0.5); // Init params

  mMakeGraphicsFunc = [&]() {

#ifdef OS_IOS
    auto scaleFactor = GetScaleForScreen(PLUG_WIDTH, PLUG_HEIGHT) * 0.85f;
#else
    auto scaleFactor = 1.0f;
#endif

    return MakeGraphics(*this, PLUG_WIDTH, PLUG_HEIGHT, PLUG_FPS, scaleFactor);
  };

  mLayoutFunc = [&](IGraphics* pGraphics) {
    pGraphics->AttachCornerResizer(EUIResizerMode::Scale, false);
    pGraphics->AttachTextEntryControl();
    pGraphics->EnableMouseOver(true);
    pGraphics->EnableTooltips(true);
    pGraphics->EnableMultiTouch(true);

    pGraphics->LoadFont("Roboto-Regular", ROBOTO_FN);
    pGraphics->LoadFont("Michroma-Regular", MICHROMA_FN);

    const auto gearSVG = pGraphics->LoadSVG(GEAR_FN);
    const auto fileSVG = pGraphics->LoadSVG(FILE_FN);
    const auto globeSVG = pGraphics->LoadSVG(GLOBE_ICON_FN);
    const auto crossSVG = pGraphics->LoadSVG(CLOSE_BUTTON_FN);
    const auto rightArrowSVG = pGraphics->LoadSVG(RIGHT_ARROW_FN);
    const auto leftArrowSVG = pGraphics->LoadSVG(LEFT_ARROW_FN);

    const auto fileBackgroundBitmap = pGraphics->LoadBitmap(FILEBACKGROUND_FN);
    const auto inputLevelBackgroundBitmap = pGraphics->LoadBitmap(INPUTLEVELBACKGROUND_FN);
    const auto linesBitmap = pGraphics->LoadBitmap(LINES_FN);
    const auto knobBackgroundBitmap = pGraphics->LoadBitmap(KNOBBACKGROUND_FN);
    const auto switchHandleBitmap = pGraphics->LoadBitmap(SLIDESWITCHHANDLE_FN);
    const auto meterBackgroundBitmap = pGraphics->LoadBitmap(METERBACKGROUND_FN);
    // Logo removed - amp selector takes its place

    const auto b = pGraphics->GetBounds();
    // NAM_KNOB_WIDTH/HEIGHT defined in header - use them for proper sizing
    #ifndef NAM_KNOB_WIDTH
    #define NAM_KNOB_WIDTH 60.0f
    #endif

    // Explicit Layout for 980x410 - EXACT values from feature/ts-m1n3-integration
    
    // Noise Gate Pedal (Left) - aligned with drive pedal
    const float gateCenterX = 110.0f;
    const float pedalKnobY = 214.0f;
    const float pedalSwitchY = 324.0f;
    
    // Drive pedal (center-left). Keep one honest, functional level control for
    // the fixed captures; the historical Tone parameter is not DSP-wired.
    const float boostPedalCenterX = 265.0f;
    
    // Amp Head (Right) - EXACT from feature branch
    const float ampStartX = 420.0f;
    const float ampWidth = 980.0f - ampStartX;
    const float ampKnobY = 230.0f;
    const float ampSpacing = (ampWidth - 40.0f) / 6.0f;
    
    // ===== Noise Gate Pedal =====
    // Knob graphic only (40x40 centered)
    const float pedalKnobSize = 40.0f;
    const auto thresholdKnobArea = IRECT(gateCenterX - pedalKnobSize/2, pedalKnobY - pedalKnobSize/2,
                                          gateCenterX + pedalKnobSize/2, pedalKnobY + pedalKnobSize/2);
    const auto thresholdLabelArea = IRECT(gateCenterX - 55, 182, gateCenterX + 55, 198);
    const auto ngValueArea = IRECT(gateCenterX - 45, 234, gateCenterX + 45, 252);
    
    // Keep the pedal buttons inside the artwork's recessed inner faces. The
    // half-pixel gate centre follows the asymmetric painted border exactly;
    // the full control rectangles remain the hit targets.
    constexpr float gateFaceLeft = 57.0f;
    constexpr float gateFaceRight = 162.0f;
    constexpr float driveFaceLeft = 211.0f;
    constexpr float driveFaceRight = 319.0f;
    constexpr float pedalControlInset = 5.0f;
    const auto ngToggleArea = IRECT(gateFaceLeft + pedalControlInset, pedalSwitchY,
                                    gateFaceRight - pedalControlInset, pedalSwitchY + 32.0f);

    const auto boostLevelLabelArea = IRECT(boostPedalCenterX - 55, 182, boostPedalCenterX + 55, 198);
    const auto boostLevelKnobArea =
      IRECT(boostPedalCenterX - pedalKnobSize / 2, pedalKnobY - pedalKnobSize / 2,
            boostPedalCenterX + pedalKnobSize / 2, pedalKnobY + pedalKnobSize / 2);
    const auto boostLevelValueArea = IRECT(boostPedalCenterX - 45, 234, boostPedalCenterX + 45, 252);
    const auto boostModelArea = IRECT(driveFaceLeft + pedalControlInset, 270.0f,
                                      driveFaceRight - pedalControlInset, 302.0f);
    const auto boostToggleArea = IRECT(driveFaceLeft + pedalControlInset, pedalSwitchY,
                                       driveFaceRight - pedalControlInset, pedalSwitchY + 32.0f);
    
    // ===== Amp Head Knobs - EXACT from feature branch =====
    const auto inputKnobArea = IRECT(0, 0, NAM_KNOB_WIDTH, NAM_KNOB_HEIGHT)
      .GetTranslated(ampStartX + ampSpacing*0 + 10 - NAM_KNOB_WIDTH/2, ampKnobY - NAM_KNOB_HEIGHT/2);
    const auto gainKnobArea = IRECT(0, 0, NAM_KNOB_WIDTH, NAM_KNOB_HEIGHT)
      .GetTranslated(ampStartX + ampSpacing*1 + 10 - NAM_KNOB_WIDTH/2, ampKnobY - NAM_KNOB_HEIGHT/2);
    const auto bassKnobArea = IRECT(0, 0, NAM_KNOB_WIDTH, NAM_KNOB_HEIGHT)
      .GetTranslated(ampStartX + ampSpacing*2 + 10 - NAM_KNOB_WIDTH/2, ampKnobY - NAM_KNOB_HEIGHT/2);
    const auto midKnobArea = IRECT(0, 0, NAM_KNOB_WIDTH, NAM_KNOB_HEIGHT)
      .GetTranslated(ampStartX + ampSpacing*3 + 10 - NAM_KNOB_WIDTH/2, ampKnobY - NAM_KNOB_HEIGHT/2);
    const auto trebleKnobArea = IRECT(0, 0, NAM_KNOB_WIDTH, NAM_KNOB_HEIGHT)
      .GetTranslated(ampStartX + ampSpacing*4 + 10 - NAM_KNOB_WIDTH/2, ampKnobY - NAM_KNOB_HEIGHT/2);
    const auto outputKnobArea = IRECT(0, 0, NAM_KNOB_WIDTH, NAM_KNOB_HEIGHT)
      .GetTranslated(ampStartX + ampSpacing*5 + 10 - NAM_KNOB_WIDTH/2, ampKnobY - NAM_KNOB_HEIGHT/2);
    
    // The primary model choice is a full-width segmented switch mounted above
    // the amp controls. It avoids squeezing long model names into a small menu.
    const auto ampModelLabelArea = IRECT(370, 132, 455, 164);
    const auto ampModelSwitchArea = IRECT(455, 132, 910, 164);
    const auto transposeArea = IRECT(74, 110, 336, 178);

    // Text-labelled effect buttons share one clear row on the lower amp panel.
    const auto ampBypassArea = IRECT(400, 294, 530, 359);
    const auto eqToggleArea = IRECT(635, 312, 740, 344);
    
    // IR Loader - from feature branch
    const auto irArea = IRECT(500, 364, 910, 404);
    const auto irSwitchArea = IRECT(390, 368, 490, 402);
    
    // Meters at edges of Amp - from feature branch
    const auto inputMeterArea = IRECT(ampStartX - 45, ampKnobY - 40, ampStartX - 25, ampKnobY + 40);
    // Match the input meter's inset from the visible amp frame instead of
    // floating against the right edge of the plugin window.
    const auto outputMeterArea = IRECT(ampStartX + ampWidth - 65, ampKnobY - 40,
                                       ampStartX + ampWidth - 45, ampKnobY + 40);
    
    // Settings button
    const auto settingsButtonArea = CornerButtonArea(b);

    // IR loader button
    auto loadIRCompletionHandler = [&](const WDL_String& fileName, const WDL_String& path) {
      if (fileName.GetLength())
      {
        _SetParameterValueFromMainThread(kIRToggle, 1.0);
        _SetIRPathAndRequest(fileName.Get());
      }
    };

    // Set initial background based on amp model. Legacy state 3 keeps its
    // historical red fallback in both the visual and DSP paths.
    int initialAmpModel = GetParam(kAmpModel)->Int();
    const char* initialBgFile;
    switch (initialAmpModel) {
      case 0: initialBgFile = DESSTORTION_BLUE_BACKGROUND_FN; break;
      case 1:
      case 3: initialBgFile = DESSTORTION_RED_BACKGROUND_FN; break;
      case 2: initialBgFile = DESSBLOCK_GREEN_BACKGROUND_FN; break;
      case 4: initialBgFile = SICKDESS_BACKGROUND_FN; break;
      default: initialBgFile = DESSTORTION_BLUE_BACKGROUND_FN; break;
    }

    const auto ampBackgroundBitmap = pGraphics->LoadBitmap(initialBgFile);
    pGraphics->AttachControl(new NAMScaledBitmapControl(b, ampBackgroundBitmap), kCtrlTagBackground, "BACKGROUND");
    pGraphics->AttachControl(new IBitmapControl(b, linesBitmap));

    // Controls Attachment
#ifdef NAM_PICK_DIRECTORY
    const std::string defaultIRString = "Select IR directory...";
#else
    const std::string defaultIRString = "Select IR...";
#endif
    const char* const getUrl = "https://www.neuralampmodeler.com/users#comp-marb84o5";

    const auto actionButtonStyle =
      style.WithShowLabel(false)
        .WithShowValue(true)
        .WithDrawFrame(true)
        .WithDrawShadows(false)
        .WithEmboss(false)
        .WithRoundness(0.18f)
        .WithFrameThickness(1.f)
        .WithWidgetFrac(1.f)
        .WithColor(kFG, COLOR_BLACK.WithOpacity(0.72f))
        .WithColor(kPR, PluginColors::NAM_THEMECOLOR.WithOpacity(0.82f))
        .WithColor(kFR, COLOR_WHITE.WithOpacity(0.28f))
        .WithColor(kHL, COLOR_WHITE.WithOpacity(0.12f))
        .WithValueText(IText(14.f, EAlign::Center, COLOR_WHITE));
    const auto modelSwitchStyle =
      actionButtonStyle.WithValueText(IText(14.f, EAlign::Center, COLOR_WHITE));
    const auto driveMenuStyle =
      actionButtonStyle.WithValueText(IText(14.f, EAlign::Center, COLOR_WHITE));
    const auto pedalKnobStyle = style.WithShowLabel(false).WithShowValue(false);
    const auto transposeStyle =
      style.WithShowLabel(true)
        .WithShowValue(true)
        .WithDrawFrame(true)
        .WithDrawShadows(false)
        .WithEmboss(false)
        .WithRoundness(0.18f)
        .WithFrameThickness(1.f)
        .WithWidgetFrac(0.70f)
        .WithColor(kBG, COLOR_BLACK.WithOpacity(0.72f))
        .WithColor(kFG, PluginColors::NAM_THEMECOLOR.WithOpacity(0.82f))
        .WithColor(kFR, COLOR_WHITE.WithOpacity(0.28f))
        .WithColor(kHL, COLOR_WHITE.WithOpacity(0.12f))
        .WithLabelText(IText(13.f, EAlign::Center, PluginColors::NAM_THEMEFONTCOLOR))
        .WithValueText(IText(17.f, EAlign::Center, COLOR_WHITE));

    auto* transposeControl =
      pGraphics->AttachControl(new NAMTransposeControl(transposeArea, kTransposeSemitones, transposeStyle));
    transposeControl->SetTooltip(
      "Retune the guitar from -12 to +12 semitones. Double-click to return to standard pitch.");

    auto* irSwitchControl = pGraphics->AttachControl(
      new IVToggleControl(irSwitchArea, kIRToggle, "", actionButtonStyle, "CAB IR OFF", "CAB IR ON"));
    irSwitchControl->SetTooltip("Enable or bypass the cabinet IR.");
    const auto irBrowserStyle =
      style.WithValueText(IText(14.f, EAlign::Center, PluginColors::NAM_THEMEFONTCOLOR));
    pGraphics->AttachControl(
      new NAMFileBrowserControl(irArea, kMsgTagClearIR, defaultIRString.c_str(), "wav", loadIRCompletionHandler,
                                irBrowserStyle,
                                fileSVG, crossSVG, leftArrowSVG, rightArrowSVG, fileBackgroundBitmap, globeSVG,
                                "Get IRs", getUrl),
      kCtrlTagIRFileBrowser);
      
    // The pedals and amp artwork establish the large section labels. Smaller
    // functional labels keep the controls readable without competing with it.
    pGraphics->AttachControl(new IVLabelControl(
      thresholdLabelArea, "THRESHOLD",
      style.WithDrawFrame(false).WithValueText(
        IText(14.f, EAlign::Center, PluginColors::NAM_THEMEFONTCOLOR))),
      -1, "GATE_CONTROLS");
    auto* gateSwitchControl = pGraphics->AttachControl(
      new IVToggleControl(ngToggleArea, kNoiseGateActive, "", actionButtonStyle, "GATE OFF", "GATE ON"));
    gateSwitchControl->SetTooltip("Enable or bypass the noise gate.");

    auto* eqSwitchControl = pGraphics->AttachControl(
      new IVToggleControl(eqToggleArea, kEQActive, "", actionButtonStyle, "EQ OFF", "EQ ON"));
    eqSwitchControl->SetTooltip("Enable or bypass the three-band EQ.");

    auto* ampSwitchControl = pGraphics->AttachControl(
      new NAMStompToggleControl(ampBypassArea, kNAMActive, actionButtonStyle));
    ampSwitchControl->SetTooltip("Enable or bypass the amp model.");

    // Full-width, always-visible amp model selector.
    pGraphics->AttachControl(new IVLabelControl(
      ampModelLabelArea, "AMP MODEL",
      style.WithDrawFrame(false)
        .WithColor(kBG, COLOR_BLACK.WithOpacity(0.55f))
        .WithValueText(IText(14.f, EAlign::Center, COLOR_WHITE))));
    // The AU/VST3 parameter retains its historical five-state range so old
    // normalized automation still resolves to the same indices. This visual
    // control exposes the four distinct shipped models and maps them around
    // the historical, unshipped DessBlock-red compatibility slot at index 3.
    auto ampModelAction = [this](IControl* pCaller) {
      const int visibleIndex = std::clamp(static_cast<int>(std::lround(pCaller->GetValue() * 3.0)), 0, 3);
      const double normalizedValue = static_cast<double>(GetHostAmpModelIndex(visibleIndex)) / 4.0;
      BeginInformHostOfParamChangeFromUI(kAmpModel);
      SetParameterValue(kAmpModel, normalizedValue);
      EndInformHostOfParamChangeFromUI(kAmpModel);
      SendParameterValueFromDelegate(kAmpModel, normalizedValue, true);
    };
    auto* ampModelControl = pGraphics->AttachControl(
      new IVTabSwitchControl(
        ampModelSwitchArea, ampModelAction,
        std::vector<const char*>{"BLUE", "RED", "GREEN", "SickDess"}, "",
        modelSwitchStyle, EVShape::EndsRounded, EDirection::Horizontal),
      kCtrlTagAmpModel);
    ampModelControl->SetValue(GetVisibleAmpModelValue(initialAmpModel));
    ampModelControl->SetTooltip("Choose the amp model.");

    pGraphics->AttachControl(
      new IVLabelControl(
        boostLevelLabelArea, "DRIVE LEVEL",
        style.WithDrawFrame(false).WithValueText(
          IText(14.f, EAlign::Center, PluginColors::NAM_THEMEFONTCOLOR))),
      -1, "DRIVE_CONTROLS");
    pGraphics->AttachControl(
      new NAMKnobControl(boostLevelKnobArea, kBoostLevel, "", pedalKnobStyle, knobBackgroundBitmap), -1,
      "DRIVE_CONTROLS");
    pGraphics->AttachControl(
      new ICaptionControl(boostLevelValueArea, kBoostLevel,
                          IText(14.f, EAlign::Center, PluginColors::NAM_THEMEFONTCOLOR)),
      -1, "DRIVE_CONTROLS");
    auto* driveModelControl = pGraphics->AttachControl(
      new IVMenuButtonControl(boostModelArea, kBoostModel, "", driveMenuStyle), -1, "DRIVE_CONTROLS");
    driveModelControl->SetTooltip("Choose the drive capture.");
    auto* driveSwitchControl = pGraphics->AttachControl(
      new IVToggleControl(boostToggleArea, kBoostActive, "", actionButtonStyle, "DRIVE OFF", "DRIVE ON"));
    driveSwitchControl->SetTooltip("Enable or bypass the drive capture.");

    const bool driveActive = GetParam(kBoostActive)->Bool();
    pGraphics->ForControlInGroup(
      "DRIVE_CONTROLS", [driveActive](IControl* pControl) { pControl->SetDisabled(!driveActive); });

    // Noise Gate Pedal - knob only (no label)
    pGraphics->AttachControl(
      new NAMKnobControl(thresholdKnobArea, kNoiseGateThreshold, "", pedalKnobStyle, knobBackgroundBitmap), -1,
      "GATE_CONTROLS");
    pGraphics->AttachControl(
      new ICaptionControl(ngValueArea, kNoiseGateThreshold,
                          IText(14.f, EAlign::Center, PluginColors::NAM_THEMEFONTCOLOR)),
      -1, "GATE_CONTROLS");
    
    // Amp head: NAMKnobControl derives each visible label from its parameter.
    pGraphics->AttachControl(new NAMKnobControl(inputKnobArea, kInputLevel, "", style, knobBackgroundBitmap));
    pGraphics->AttachControl(new NAMKnobControl(gainKnobArea, kGain, "", style, knobBackgroundBitmap));
    pGraphics->AttachControl(
      new NAMKnobControl(bassKnobArea, kToneBass, "", style, knobBackgroundBitmap), -1, "EQ_KNOBS");
    pGraphics->AttachControl(
      new NAMKnobControl(midKnobArea, kToneMid, "", style, knobBackgroundBitmap), -1, "EQ_KNOBS");
    pGraphics->AttachControl(
      new NAMKnobControl(trebleKnobArea, kToneTreble, "", style, knobBackgroundBitmap), -1, "EQ_KNOBS");
    pGraphics->AttachControl(new NAMKnobControl(outputKnobArea, kOutputLevel, "", style, knobBackgroundBitmap));

    // Meters
    pGraphics->AttachControl(new NAMMeterControl(inputMeterArea, meterBackgroundBitmap, style), kCtrlTagInputMeter);
    pGraphics->AttachControl(new NAMMeterControl(outputMeterArea, meterBackgroundBitmap, style), kCtrlTagOutputMeter);

    // Settings/help/about box
    auto* settingsButtonControl = pGraphics->AttachControl(new NAMCircleButtonControl(
      settingsButtonArea,
      [pGraphics](IControl* pCaller) {
        pGraphics->GetControlWithTag(kCtrlTagSettingsBox)->As<NAMSettingsPageControl>()->HideAnimated(false);
      },
      gearSVG));
    settingsButtonControl->SetTooltip("Settings");

    const auto settingsBackgroundBitmap = pGraphics->LoadBitmap(SETTINGS_BACKGROUND_FN);
    pGraphics
      ->AttachControl(new NAMSettingsPageControl(b, settingsBackgroundBitmap, inputLevelBackgroundBitmap, switchHandleBitmap,
                                                 crossSVG, style, radioButtonStyle),
                      kCtrlTagSettingsBox)
      ->Hide(true);

    pGraphics->ForAllControlsFunc([](IControl* pControl) {
      pControl->SetMouseEventsWhenDisabled(false);
      pControl->SetMouseOverWhenDisabled(false);
    });

    // pGraphics->GetControlWithTag(kCtrlTagOutNorm)->SetMouseEventsWhenDisabled(false);
    // pGraphics->GetControlWithTag(kCtrlTagCalibrateInput)->SetMouseEventsWhenDisabled(false);
  };
}

NeuralAmpModeler::~NeuralAmpModeler()
{
  _DeallocateIOPointers();
  delete mModel.exchange(nullptr, std::memory_order_acq_rel);
  delete mPendingModel.exchange(nullptr, std::memory_order_acq_rel);
  delete mRetiredModel.exchange(nullptr, std::memory_order_acq_rel);
  delete mIR.exchange(nullptr, std::memory_order_acq_rel);
  delete mPendingIR.exchange(nullptr, std::memory_order_acq_rel);
  delete mRetiredIR.exchange(nullptr, std::memory_order_acq_rel);
  delete mBoostModel.exchange(nullptr, std::memory_order_acq_rel);
  delete mPendingBoostModel.exchange(nullptr, std::memory_order_acq_rel);
  delete mRetiredBoostModel.exchange(nullptr, std::memory_order_acq_rel);
}

void NeuralAmpModeler::ProcessBlock(iplug::sample** inputs, iplug::sample** outputs, int nFrames)
{
  const size_t numChannelsExternalIn = (size_t)NInChansConnected();
  const size_t numChannelsExternalOut = (size_t)NOutChansConnected();
  const size_t numChannelsInternal = kNumChannelsInternal;
  const size_t numFrames = (size_t)nFrames;
  const double sampleRate = GetSampleRate();

  const auto silenceOutputs = [&]() {
    for (size_t c = 0; c < numChannelsExternalOut; ++c)
      if (outputs != nullptr && outputs[c] != nullptr)
        std::fill(outputs[c], outputs[c] + std::max(nFrames, 0), 0.0);
  };

  // Never grow heap-backed buffers from the render callback. Hosts must call
  // OnReset() before processing or increasing the maximum block size.
  if (!mAudioConfigInitialized.load(std::memory_order_acquire) || nFrames < 0
      || nFrames > mAudioMaxBlockSize.load(std::memory_order_acquire))
  {
    silenceOutputs();
    return;
  }

  // One AMP state snapshot governs both handoff adoption and processing for
  // this block. A concurrent automation edge is applied on the next block,
  // never halfway through a fresh-resampler adoption.
  const bool ampActive = mAmpActiveTarget.load(std::memory_order_acquire);
  _ApplyDSPStaging(ampActive);

  // Offline bounces may synchronously prepare resources. This does not depend
  // on the UI/main-thread OnIdle timer and never runs in realtime mode.
  if (GetRenderingOffline())
  {
    {
      std::lock_guard<std::mutex> lock(mDSPLoadMutex);
      _ServiceDSPRequests();
      _ApplyDSPStaging(ampActive);
    }
    // Offline hosts may never pump OnIdle. Establish calibration, output
    // normalization, and host-visible latency before rendering this block,
    // but notify the host only after releasing the loader mutex.
    _SetInputGain();
    _SetOutputGain();
    _UpdateLatency();
  }

  const bool boostActive = mBoostActiveTarget.load(std::memory_order_acquire);
  const bool irActive = mIRActiveTarget.load(std::memory_order_acquire);
  const std::uint64_t audioEpoch = mAudioConfigEpoch.load(std::memory_order_acquire);
  auto* model = mModel.load(std::memory_order_acquire);
  auto* boostModel = mBoostModel.load(std::memory_order_acquire);
  auto* ir = mIR.load(std::memory_order_acquire);
  // A target selection can change before its replacement has finished loading.
  // Keep rendering through the already-adopted model until the new object is
  // ready, especially while AMP is bypassed: its persistent resampler carries
  // the phase/history that implements the latency reported to the host.
  const bool modelUsable = model != nullptr && model->GetPreparedEpoch() == audioEpoch;
  const bool ampReady = !ampActive || modelUsable;
  const bool boostReady = !boostActive
                          || (boostModel != nullptr && boostModel->GetPreparedEpoch() == audioEpoch
                              && boostModel->GetPreparedRequestToken()
                                   == mBoostLoadRequest.load(std::memory_order_acquire)
                              && boostModel->GetPreparedSelection()
                                   == mBoostModelIdx.load(std::memory_order_acquire));
  const bool irReady = !irActive
                       || (ir != nullptr && ir->GetPreparedEpoch() == audioEpoch
                           && ir->GetPreparedRequestToken() == mIRLoadRequest.load(std::memory_order_acquire));
  if (!ampReady || !boostReady || !irReady)
  {
    silenceOutputs();
    return;
  }

  // Disable floating point denormals
  std::fenv_t fe_state;
  std::feholdexcept(&fe_state);
  disable_denormals();

  _PrepareBuffers(numChannelsInternal, numFrames);
  // Input is collapsed to mono in preparation for the NAM.
  _ProcessInput(inputs, numFrames, numChannelsExternalIn, numChannelsInternal);

  // SAFETY: Sanitize inputs to prevent NaN poisoning
  for (size_t c = 0; c < numChannelsInternal; ++c)
  {
    for (size_t s = 0; s < numFrames; ++s)
    {
      if (std::isnan(mInputPointers[c][s]) || std::isinf(mInputPointers[c][s]))
      {
        mInputPointers[c][s] = 0.0;
      }
    }
  }

  // Retune the calibrated mono guitar before every nonlinear stage. The
  // filter bank has no buffered look-ahead, so this does not change host PDC.
  mTransposeProcessor.ProcessInPlace(
    mInputPointers[0], nFrames, mTransposeSemitones.load(std::memory_order_acquire));

  const bool noiseGateActive = GetParam(kNoiseGateActive)->Value();
  const bool toneStackActive = GetParam(kEQActive)->Value();

  // Boost processing
  // SAFETY: Zero buffer to prevent feedback loops from previous frames
  for (size_t c = 0; c < numChannelsInternal; ++c) {
     std::fill(mBoostOutputArray[c].begin(), mBoostOutputArray[c].end(), 0.0);
  }

  sample** ampInput = mInputPointers;
  if (boostActive)
  {
    boostModel->process(mInputPointers, mBoostOutputPointers, nFrames);
    const double boostLeveldB = GetParam(kBoostLevel)->Value();
    const double boostLevel = std::pow(10.0, boostLeveldB / 20.0);
    for (int s = 0; s < nFrames; ++s)
      mBoostOutputPointers[0][s] *= boostLevel;
    ampInput = mBoostOutputPointers;

    // SAFETY: Sanitize Boost Output to prevent NaN poisoning in Trigger/Model
    for (size_t c = 0; c < numChannelsInternal; ++c)
    {
       for (size_t s = 0; s < numFrames; ++s)
       {
          if (std::isnan(ampInput[c][s]) || std::isinf(ampInput[c][s]))
          {
             ampInput[c][s] = 0.0;
          }
       }
    }
  }


  // Noise gate trigger
  if (noiseGateActive)
  {
    const double time = 0.01;
    const double threshold = GetParam(kNoiseGateThreshold)->Value(); // GetParam...
    const double ratio = 0.1; // Quadratic...
    const double openTime = 0.005;
    const double holdTime = 0.01;
    const double closeTime = 0.05;
    const dsp::noise_gate::TriggerParams triggerParams(time, threshold, ratio, openTime, holdTime, closeTime);
    mNoiseGateTrigger.SetParams(triggerParams);
    mNoiseGateTrigger.SetSampleRate(sampleRate);
    mNoiseGateTrigger.Process(ampInput, numChannelsInternal, numFrames);
  }

  // Alias ampInput to triggerOutput for model compatibility (bypass trigger audio output)
  sample** triggerOutput = ampInput;

  if (ampActive)
  {
    // Read gain from atomic (written by UI thread) into audio-thread-local buffer
    mCurrentParams[0] = mTargetGain.load(std::memory_order_relaxed);
    model->process(triggerOutput, mOutputPointers, nFrames, mCurrentParams.data(), (int)mCurrentParams.size());
  }

  else
  {
    if (modelUsable)
      model->processBypass(triggerOutput, mOutputPointers, nFrames);
    else
      _FallbackDSP(triggerOutput, mOutputPointers, numChannelsInternal, numFrames);
  }
  // Apply the noise gate after the NAM
  sample** gateGainOutput =
    noiseGateActive ? mNoiseGateGain.Process(mOutputPointers, numChannelsInternal, numFrames) : mOutputPointers;

  const double bass = mToneBassTarget.load(std::memory_order_acquire);
  const double middle = mToneMidTarget.load(std::memory_order_acquire);
  const double treble = mToneTrebleTarget.load(std::memory_order_acquire);
  if (bass != mAppliedToneBass)
  {
    mToneStack->SetParam("bass", bass);
    mAppliedToneBass = bass;
  }
  if (middle != mAppliedToneMid)
  {
    mToneStack->SetParam("middle", middle);
    mAppliedToneMid = middle;
  }
  if (treble != mAppliedToneTreble)
  {
    mToneStack->SetParam("treble", treble);
    mAppliedToneTreble = treble;
  }

  sample** toneStackOutPointers = (toneStackActive && mToneStack != nullptr)
                                    ? mToneStack->Process(gateGainOutput, numChannelsInternal, nFrames)
                                    : gateGainOutput;

  sample** irPointers = toneStackOutPointers;
  if (irActive)
    irPointers = ir->Process(toneStackOutPointers, numChannelsInternal, numFrames);

  // And the HPF for DC offset (Issue 271)
  const double highPassCutoffFreq = kDCBlockerFrequency;
  // const double lowPassCutoffFreq = 20000.0;
  const recursive_linear_filter::HighPassParams highPassParams(sampleRate, highPassCutoffFreq);
  // const recursive_linear_filter::LowPassParams lowPassParams(sampleRate, lowPassCutoffFreq);
  mHighPass.SetParams(highPassParams);
  // mLowPass.SetParams(lowPassParams);
  sample** hpfPointers = mHighPass.Process(irPointers, numChannelsInternal, numFrames);
  // sample** lpfPointers = mLowPass.Process(hpfPointers, numChannelsInternal, numFrames);

  // restore previous floating point state
  std::feupdateenv(&fe_state);

  // Let's get outta here
  // This is where we exit mono for whatever the output requires.
  _ProcessOutput(hpfPointers, outputs, numFrames, numChannelsInternal, numChannelsExternalOut);
  // _ProcessOutput(lpfPointers, outputs, numFrames, numChannelsInternal, numChannelsExternalOut);
  // * Output of input leveling (inputs -> mInputPointers),
  // * Output of output leveling (mOutputPointers -> outputs)
  _UpdateMeters(mInputPointers, outputs, numFrames, numChannelsInternal, numChannelsExternalOut);
}

void NeuralAmpModeler::OnReset()
{
  const auto sampleRate = GetSampleRate();
  const int maxBlockSize = std::max(GetBlockSize(), 1);
  const bool hadConfig = mAudioConfigInitialized.load(std::memory_order_acquire);
  const bool configChanged = !hadConfig || mAudioSampleRate.load(std::memory_order_acquire) != sampleRate
                             || mAudioMaxBlockSize.load(std::memory_order_acquire) != maxBlockSize;
  if (configChanged)
  {
    mAudioConfigInitialized.store(false, std::memory_order_release);
    // Odd epochs are transitions and are never accepted by a staging producer.
    mAudioConfigEpoch.fetch_add(1, std::memory_order_acq_rel);
    mAudioSampleRate.store(sampleRate, std::memory_order_release);
    mAudioMaxBlockSize.store(maxBlockSize, std::memory_order_release);
    mAudioConfigEpoch.fetch_add(1, std::memory_order_acq_rel);
  }

  // Tail is because the HPF DC blocker has a decay.
  // 10 cycles should be enough to pass the VST3 tests checking tail behavior.
  // I'm ignoring the model & IR, but it's not the end of the world.
  const int tailCycles = 10;
  SetTailSize(tailCycles * (int)(sampleRate / kDCBlockerFrequency));
  mInputSender.Reset(sampleRate);
  mOutputSender.Reset(sampleRate);

  // Reserve every plugin-owned processing buffer here. DSP modules are built,
  // reset, prewarmed, and published later from OnIdle(). Existing modules are
  // bypassed until their prepared epoch matches this reset.
  _PrepareBuffers(kNumChannelsInternal, maxBlockSize);
  mNoiseGateTrigger.PrepareBuffers(kNumChannelsInternal, maxBlockSize);
  mNoiseGateGain.PrepareBuffers(kNumChannelsInternal, maxBlockSize);
  mHighPass.PrepareBuffers(kNumChannelsInternal, maxBlockSize);
  mTransposeSemitones.store(GetParam(kTransposeSemitones)->Int(), std::memory_order_release);
  mTransposeProcessor.Reset(sampleRate, mTransposeSemitones.load(std::memory_order_acquire));

  const double bass = GetParam(kToneBass)->Value();
  const double middle = GetParam(kToneMid)->Value();
  const double treble = GetParam(kToneTreble)->Value();
  mToneBassTarget.store(bass, std::memory_order_release);
  mToneMidTarget.store(middle, std::memory_order_release);
  mToneTrebleTarget.store(treble, std::memory_order_release);
  mAppliedToneBass = bass;
  mAppliedToneMid = middle;
  mAppliedToneTreble = treble;
  mToneStack->Reset(sampleRate, maxBlockSize);
  mToneStack->SetParam("bass", bass);
  mToneStack->SetParam("middle", middle);
  mToneStack->SetParam("treble", treble);

  mAmpModelIdx.store(GetParam(kAmpModel)->Int(), std::memory_order_release);
  mAmpActiveTarget.store(GetParam(kNAMActive)->Bool(), std::memory_order_release);
  mAmpRetryNeeded.store(false, std::memory_order_release);
  mBoostModelIdx.store(GetParam(kBoostModel)->Int(), std::memory_order_release);
  mBoostActiveTarget.store(GetParam(kBoostActive)->Bool(), std::memory_order_release);
  mIRActiveTarget.store(GetParam(kIRToggle)->Bool(), std::memory_order_release);
  mAmpLoadRequest.fetch_add(1, std::memory_order_release);
  mBoostLoadRequest.fetch_add(1, std::memory_order_release);
  _RequestIRReload();

  mAudioConfigInitialized.store(true, std::memory_order_release);

  _UpdateLatency();
}

void NeuralAmpModeler::_ServiceDSPRequests()
{
  // This service never runs in realtime and is serialized by mDSPLoadMutex.
  // Every non-render dereference either holds that mutex or consumes the
  // immutable metadata cache, so retired objects can be reclaimed here. This
  // also frees each handoff slot for multi-transition offline bounces.
  delete mRetiredModel.exchange(nullptr, std::memory_order_acq_rel);
  delete mRetiredIR.exchange(nullptr, std::memory_order_acq_rel);
  delete mRetiredBoostModel.exchange(nullptr, std::memory_order_acq_rel);

  const unsigned int ampRequest = mAmpLoadRequest.load(std::memory_order_acquire);
  if (ampRequest != mHandledAmpLoadRequest.load(std::memory_order_acquire))
  {
    const int ampModelInt = mAmpModelIdx.load(std::memory_order_acquire);
    // Keep the selected amp resident even while bypassed. This makes AMP ON/OFF
    // an immediate DSP switch instead of an asynchronous disk-load operation,
    // and it preloads a model selected while the amp is bypassed.
    const std::string errorMsg =
      _LoadModelForGain(_GetAmpModelName(ampModelInt), GetParam(kGain)->Int(), ampRequest, ampModelInt);
    mHandledAmpLoadRequest.store(ampRequest, std::memory_order_release);
    if (!errorMsg.empty() && ampRequest == mAmpLoadRequest.load(std::memory_order_acquire)
        && ampModelInt == mAmpModelIdx.load(std::memory_order_acquire))
    {
      delete mPendingModel.exchange(nullptr, std::memory_order_acq_rel);
      // Preserve an already-adopted model as the latency-compensated bypass
      // path. A failed replacement remains recoverable by the next explicit
      // AMP ON request without collapsing the host's PDC in the meantime.
      if (mModel.load(std::memory_order_acquire) == nullptr)
        mShouldRemoveModel.store(true, std::memory_order_release);
      mAmpRetryNeeded.store(true, std::memory_order_release);
      if (mAmpActiveTarget.exchange(false, std::memory_order_acq_rel))
      {
        GetParam(kNAMActive)->Set(false);
        mAmpDisableNotificationPending.store(true, std::memory_order_release);
      }
      mDeferredAmpLoadError = errorMsg;
    }
    else if (errorMsg.empty())
      mAmpRetryNeeded.store(false, std::memory_order_release);
  }

  const unsigned int boostRequest = mBoostLoadRequest.load(std::memory_order_acquire);
  if (boostRequest != mHandledBoostLoadRequest.load(std::memory_order_acquire))
  {
    std::string errorMsg;
    const int boostModelInt = mBoostModelIdx.load(std::memory_order_acquire);
    if (mBoostActiveTarget.load(std::memory_order_acquire))
      errorMsg = _LoadBoostModel(_GetBoostModelName(boostModelInt), boostRequest, boostModelInt);
    else
    {
      delete mPendingBoostModel.exchange(nullptr, std::memory_order_acq_rel);
      mShouldRemoveBoostModel.store(true, std::memory_order_release);
    }
    mHandledBoostLoadRequest.store(boostRequest, std::memory_order_release);
    if (!errorMsg.empty() && boostRequest == mBoostLoadRequest.load(std::memory_order_acquire)
        && boostModelInt == mBoostModelIdx.load(std::memory_order_acquire)
        && mBoostActiveTarget.load(std::memory_order_acquire))
    {
      delete mPendingBoostModel.exchange(nullptr, std::memory_order_acq_rel);
      mShouldRemoveBoostModel.store(true, std::memory_order_release);
      GetParam(kBoostActive)->Set(false);
      mBoostActiveTarget.store(false, std::memory_order_release);
      mBoostDisableNotificationPending.store(true, std::memory_order_release);
      mDeferredBoostLoadError = errorMsg;
    }
  }

  unsigned int irRequest = 0;
  WDL_String requestedIRPath;
  {
    std::lock_guard<std::mutex> lock(mPathMutex);
    irRequest = mIRLoadRequest.load(std::memory_order_acquire);
    requestedIRPath = mIRPath;
  }
  if (irRequest != mHandledIRLoadRequest.load(std::memory_order_acquire))
  {
    if (!mIRActiveTarget.load(std::memory_order_acquire))
    {
      delete mPendingIR.exchange(nullptr, std::memory_order_acq_rel);
      mShouldRemoveIR.store(true, std::memory_order_release);
      mHandledIRLoadRequest.store(irRequest, std::memory_order_release);
      return;
    }

    if (requestedIRPath.GetLength() == 0)
    {
      const std::string defaultIRPath = GetBundledDefaultIRPath(GetBundleID());
      if (!defaultIRPath.empty())
      {
        std::lock_guard<std::mutex> lock(mPathMutex);
        if (irRequest == mIRLoadRequest.load(std::memory_order_acquire) && mIRPath.GetLength() == 0)
          mIRPath.Set(defaultIRPath.c_str());
        requestedIRPath = mIRPath;
      }
    }

    const auto wavState = requestedIRPath.GetLength() > 0
                            ? _StageIR(requestedIRPath, irRequest)
                            : dsp::wav::LoadReturnCode::ERROR_OTHER;
    mHandledIRLoadRequest.store(irRequest, std::memory_order_release);
    bool failedCurrentRequest = false;
    if (wavState != dsp::wav::LoadReturnCode::SUCCESS)
    {
      std::lock_guard<std::mutex> lock(mPathMutex);
      failedCurrentRequest = irRequest == mIRLoadRequest.load(std::memory_order_acquire);
      if (failedCurrentRequest)
        mIRPath.Set("");
    }
    if (failedCurrentRequest)
    {
      delete mPendingIR.exchange(nullptr, std::memory_order_acq_rel);
      mShouldRemoveIR.store(true, std::memory_order_release);
      mIRActiveTarget.store(false, std::memory_order_release);
      GetParam(kIRToggle)->Set(false);
      mIRDisableNotificationPending.store(true, std::memory_order_release);
      if (requestedIRPath.GetLength() > 0)
      {
        std::stringstream message;
        message << "Failed to load IR file " << requestedIRPath.Get() << ":\n";
        message << dsp::wav::GetMsgForLoadReturnCode(wavState);
        mDeferredIRLoadError = message.str();
      }
    }
  }
}

void NeuralAmpModeler::OnRestoreState()
{
  Plugin::OnRestoreState();
  mHostStateRepublishPending.store(true, std::memory_order_release);
}

void NeuralAmpModeler::OnIdle()
{
  std::string ampLoadError;
  std::string boostLoadError;
  std::string irLoadError;
  {
    std::lock_guard<std::mutex> lock(mDSPLoadMutex);
    _ServiceDSPRequests();
    ampLoadError.swap(mDeferredAmpLoadError);
    boostLoadError.swap(mDeferredBoostLoadError);
    irLoadError.swap(mDeferredIRLoadError);
  }

  if (mAmpDisableNotificationPending.exchange(false, std::memory_order_acq_rel))
    _SetParameterValueFromMainThread(kNAMActive, 0.0);
  if (mBoostDisableNotificationPending.exchange(false, std::memory_order_acq_rel))
    _SetParameterValueFromMainThread(kBoostActive, 0.0);
  if (mIRDisableNotificationPending.exchange(false, std::memory_order_acq_rel))
    _SetParameterValueFromMainThread(kIRToggle, 0.0);

  // Never present modal UI while holding mDSPLoadMutex. Apart from avoiding
  // user-controlled loader stalls, this permits timer/modal-loop re-entry.
  if (auto* ui = GetUI())
  {
    if (!ampLoadError.empty())
      _ShowMessageBox(ui, ampLoadError.c_str(), "Failed to load model!", kMB_OK);
    if (!boostLoadError.empty())
      _ShowMessageBox(ui, boostLoadError.c_str(), "Boost model unavailable", kMB_OK);
    if (!irLoadError.empty())
      _ShowMessageBox(ui, irLoadError.c_str(), "Failed to load IR!", kMB_OK);
  }

  mInputSender.TransmitData(*this);
  mOutputSender.TransmitData(*this);

  if (mNewModelLoadedInDSP.exchange(false, std::memory_order_acq_rel))
  {
    std::string preparedPath;
    {
      std::lock_guard<std::mutex> lock(mDSPLoadMutex);
      if (auto* model = mModel.load(std::memory_order_acquire))
        preparedPath = model->GetPreparedPath();
    }
    if (!preparedPath.empty())
      _SetNAMPath(preparedPath.c_str());
    _UpdateLatency();
    _SetInputGain();
    _SetOutputGain();
    _ValidateOutputModeForCurrentModel();
    if (GetUI())
      _UpdateControlsFromModel();
  }
  if (mOutputModeValidationPending.exchange(false, std::memory_order_acq_rel))
    _ValidateOutputModeForCurrentModel();
  if (mNewIRLoadedInDSP.exchange(false, std::memory_order_acq_rel))
  {
    const WDL_String loadedPath = _GetIRPathSnapshot();
    if (GetUI() && loadedPath.GetLength() > 0)
      SendControlMsgFromDelegate(kCtrlTagIRFileBrowser, kMsgTagLoadedIR, loadedPath.GetLength(), loadedPath.Get());
  }
  if (mModelCleared.exchange(false, std::memory_order_acq_rel))
  {
    _SetNAMPath("");
    _UpdateLatency();
    _SetInputGain();
    _SetOutputGain();
    if (auto* pGraphics = GetUI())
    {
      // FIXME -- need to disable only the "normalized" model
      // pGraphics->GetControlWithTag(kCtrlTagOutputMode)->SetDisabled(false);
      static_cast<NAMSettingsPageControl*>(pGraphics->GetControlWithTag(kCtrlTagSettingsBox))->ClearModelInfo();
    }
  }
  
  if (mNewBoostModelLoadedInDSP.exchange(false, std::memory_order_acq_rel))
  {
    std::string preparedPath;
    {
      std::lock_guard<std::mutex> lock(mDSPLoadMutex);
      if (auto* model = mBoostModel.load(std::memory_order_acquire))
        preparedPath = model->GetPreparedPath();
    }
    if (!preparedPath.empty())
    {
      _SetBoostNAMPath(preparedPath.c_str());
      const WDL_String loadedPath = _GetBoostNAMPathSnapshot();
      if (GetUI() && GetUI()->GetControlWithTag(kCtrlTagBoostModelFileBrowser))
        SendControlMsgFromDelegate(kCtrlTagBoostModelFileBrowser, kMsgTagLoadedBoostModel, loadedPath.GetLength(),
                                   loadedPath.Get());
    }
  }
  if (mBoostModelCleared.exchange(false, std::memory_order_acq_rel))
  {
      _SetBoostNAMPath("");
  }

  if (mHostStateRepublishPending.exchange(false, std::memory_order_acq_rel))
    DirtyParametersFromUI();
}

bool NeuralAmpModeler::SerializeState(IByteChunk& chunk) const
{
  // If this isn't here when unserializing, then we know we're dealing with something before v0.8.0.
  WDL_String header("###NeuralAmpModeler###"); // Don't change this!
  chunk.PutStr(header.Get());
  // Plugin version, so we can load legacy serialized states in the future!
  WDL_String version(PLUG_VERSION_STR);
  chunk.PutStr(version.Get());
  // Model path is no longer serialized - we load based on gain parameter instead
  // Keep empty strings for backward compatibility with old presets
  const WDL_String namPath = _GetNAMPathSnapshot();
  const WDL_String irPath = _GetIRPathSnapshot();
  chunk.PutStr(namPath.Get());
  chunk.PutStr(irPath.Get());  // Still serialize IR path
  return SerializeParams(chunk);  // This serializes gain parameter
}

int NeuralAmpModeler::UnserializeState(const IByteChunk& chunk, int startPos)
{
  try
  {
    // Look for the expected header. If it's there, then we'll know what to do.
    WDL_String header;
    int pos = GetBoundedChunkString(chunk, header, startPos, 64);
    if (pos < 0)
      return -1;

    const char* kExpectedHeader = "###NeuralAmpModeler###";
    if (strcmp(header.Get(), kExpectedHeader) == 0)
      return _UnserializeStateWithKnownVersion(chunk, pos);
    return _UnserializeStateWithUnknownVersion(chunk, startPos);
  }
  catch (const std::exception& e)
  {
    std::cerr << "Ignoring invalid DessMetal preset state: " << e.what() << std::endl;
    return -1;
  }
}

void NeuralAmpModeler::OnUIOpen()
{
  Plugin::OnUIOpen();

  // Map the historical five-state host parameter onto the four distinct
  // visible models. State 3 remains a legacy alias for the red capture.
  if (auto* ampModelControl = GetUI()->GetControlWithTag(kCtrlTagAmpModel))
  {
    const int modelIndex = GetParam(kAmpModel)->Int();
    ampModelControl->SetValueFromDelegate(GetVisibleAmpModelValue(modelIndex));
  }

  // Note: Model file browser control was removed in favor of amp model selector
  // Custom models are loaded via the amp selector's "Load Custom..." option

  const WDL_String irPath = _GetIRPathSnapshot();
  if (irPath.GetLength())
  {
    SendControlMsgFromDelegate(kCtrlTagIRFileBrowser, kMsgTagLoadedIR, irPath.GetLength(), irPath.Get());
    if (mIR.load(std::memory_order_acquire) == nullptr
        && mPendingIR.load(std::memory_order_acquire) == nullptr)
      SendControlMsgFromDelegate(kCtrlTagIRFileBrowser, kMsgTagLoadFailed);
  }

  if (mModel.load(std::memory_order_acquire) != nullptr)
  {
    // Host notification is deliberately deferred to OnIdle() to avoid a
    // synchronous host edit while the editor is being attached.
    mOutputModeValidationPending.store(true, std::memory_order_release);
    _UpdateControlsFromModel();
  }
  
  const WDL_String boostPath = _GetBoostNAMPathSnapshot();
  // The fixed drive selector replaced the historical Boost file browser. Old
  // standalone preferences can still contain a path, so never message the
  // removed control during editor attachment.
  if (boostPath.GetLength() && GetUI() != nullptr
      && GetUI()->GetControlWithTag(kCtrlTagBoostModelFileBrowser) != nullptr)
  {
    SendControlMsgFromDelegate(kCtrlTagBoostModelFileBrowser, kMsgTagLoadedBoostModel, boostPath.GetLength(),
                               boostPath.Get());
    if (mBoostModel.load(std::memory_order_acquire) == nullptr
        && mPendingBoostModel.load(std::memory_order_acquire) == nullptr)
      SendControlMsgFromDelegate(kCtrlTagBoostModelFileBrowser, kMsgTagLoadFailed);
  }
}

void NeuralAmpModeler::OnParamChange(int paramIdx)
{
  switch (paramIdx)
  {
    // Load model when amp model changes
    case kAmpModel:
    {
      int ampModelInt = GetParam(kAmpModel)->Int();
      mAmpModelIdx = ampModelInt; // Update atomic index for audio thread
      mAmpRetryNeeded.store(false, std::memory_order_release);

      // Parameter callbacks may execute on the render thread. Loading is
      // coalesced and performed from OnIdle(); this callback remains wait-free.
      mAmpLoadRequest.fetch_add(1, std::memory_order_release);
    
      break;
    }
     case kGain:
     {
       double gainVal = GetParam(kGain)->Value();
       // Direct mapping: knob 2 = param 0.2, knob 8 = param 0.8 (matches training data)
       // Clamp to training range [0.2, 0.8] to avoid extrapolation
       mTargetGain.store(std::clamp(gainVal / 10.0, 0.2, 0.8), std::memory_order_relaxed);
       break;
     }
    case kBoostModel:
    {
      mBoostModelIdx.store(GetParam(kBoostModel)->Int(), std::memory_order_release);
      if (mBoostActiveTarget.load(std::memory_order_acquire))
        mBoostLoadRequest.fetch_add(1, std::memory_order_release);
      break;
    }
    case kBoostActive:
    {
      mBoostActiveTarget.store(GetParam(kBoostActive)->Bool(), std::memory_order_release);
      mBoostLoadRequest.fetch_add(1, std::memory_order_release);
      break;
    }
    case kNAMActive:
    {
      const bool active = GetParam(kNAMActive)->Bool();
      mAmpActiveTarget.store(active, std::memory_order_release);
      // Ordinary bypass automation never reloads. Only a user attempt to
      // recover from a prior verified load failure schedules another load.
      if (active && mAmpRetryNeeded.exchange(false, std::memory_order_acq_rel))
        mAmpLoadRequest.fetch_add(1, std::memory_order_release);
      _SetInputGain();
      _SetOutputGain();
      if (active && GetParam(kOutputMode)->Int() != 0)
        mOutputModeValidationPending.store(true, std::memory_order_release);
      break;
    }
    case kIRToggle:
    {
      mIRActiveTarget.store(GetParam(kIRToggle)->Bool(), std::memory_order_release);
      mIRLoadRequest.fetch_add(1, std::memory_order_release);
      break;
    }
    case kTransposeSemitones:
      mTransposeSemitones.store(GetParam(kTransposeSemitones)->Int(), std::memory_order_release);
      break;

    // Changes to the input gain
    case kCalibrateInput:
    case kInputCalibrationLevel:
    case kInputLevel: _SetInputGain(); break;
    // Changes to the output gain
    case kOutputLevel: _SetOutputGain(); break;
    case kOutputMode:
      _SetOutputGain();
      if (GetParam(kOutputMode)->Int() != 0)
        mOutputModeValidationPending.store(true, std::memory_order_release);
      break;
    // Tone stack:
    case kToneBass: mToneBassTarget.store(GetParam(paramIdx)->Value(), std::memory_order_release); break;
    case kToneMid: mToneMidTarget.store(GetParam(paramIdx)->Value(), std::memory_order_release); break;
    case kToneTreble: mToneTrebleTarget.store(GetParam(paramIdx)->Value(), std::memory_order_release); break;
    default: break;
  }
}

void NeuralAmpModeler::OnParamChangeUI(int paramIdx, EParamSource source)
{
  if (auto pGraphics = GetUI())
  {
    bool active = GetParam(paramIdx)->Bool();

    switch (paramIdx)
    {
      case kNoiseGateActive:
        pGraphics->ForControlInGroup("GATE_CONTROLS",
                                     [active](IControl* pControl) { pControl->SetDisabled(!active); });
        break;
      case kEQActive:
        pGraphics->ForControlInGroup("EQ_KNOBS", [active](IControl* pControl) { pControl->SetDisabled(!active); });
        break;
      case kBoostActive:
        pGraphics->ForControlInGroup(
          "DRIVE_CONTROLS", [active](IControl* pControl) { pControl->SetDisabled(!active); });
        break;
      case kIRToggle: pGraphics->GetControlWithTag(kCtrlTagIRFileBrowser)->SetDisabled(!active); break;
      case kAmpModel:
      {
        const int modelIndex = GetParam(kAmpModel)->Int();
        if (auto* ampModelControl = pGraphics->GetControlWithTag(kCtrlTagAmpModel))
          ampModelControl->SetValueFromDelegate(GetVisibleAmpModelValue(modelIndex));

        const char* bgFile = DESSTORTION_BLUE_BACKGROUND_FN;
        switch (modelIndex)
        {
          case 1:
          case 3: bgFile = DESSTORTION_RED_BACKGROUND_FN; break;
          case 2: bgFile = DESSBLOCK_GREEN_BACKGROUND_FN; break;
          case 4: bgFile = SICKDESS_BACKGROUND_FN; break;
          default: break;
        }
        const auto bgBitmap = pGraphics->LoadBitmap(bgFile);
        if (auto* pBGControl = static_cast<NAMScaledBitmapControl*>(pGraphics->GetControlWithTag(kCtrlTagBackground)))
          pBGControl->SetBitmap(bgBitmap);
        break;
      }

      default: break;
    }
  }
}

bool NeuralAmpModeler::OnMessage(int msgTag, int ctrlTag, int dataSize, const void* pData)
{
  switch (msgTag)
  {
    case kMsgTagClearModel:
      mAmpActiveTarget.store(false, std::memory_order_release);
      _SetParameterValueFromMainThread(kNAMActive, 0.0);
      mHandledAmpLoadRequest.store(mAmpLoadRequest.fetch_add(1, std::memory_order_acq_rel) + 1,
                                   std::memory_order_release);
      delete mPendingModel.exchange(nullptr, std::memory_order_acq_rel);
      mShouldRemoveModel = true;
      return true;
    case kMsgTagClearIR:
      mIRActiveTarget.store(false, std::memory_order_release);
      _SetParameterValueFromMainThread(kIRToggle, 0.0);
      {
        std::lock_guard<std::mutex> lock(mPathMutex);
        mIRPath.Set("");
        mHandledIRLoadRequest.store(mIRLoadRequest.load(std::memory_order_acquire), std::memory_order_release);
      }
      delete mPendingIR.exchange(nullptr, std::memory_order_acq_rel);
      mShouldRemoveIR = true;
      return true;
    case kMsgTagClearBoostModel:
      mBoostActiveTarget.store(false, std::memory_order_release);
      _SetParameterValueFromMainThread(kBoostActive, 0.0);
      mHandledBoostLoadRequest.store(mBoostLoadRequest.load(std::memory_order_acquire), std::memory_order_release);
      delete mPendingBoostModel.exchange(nullptr, std::memory_order_acq_rel);
      mShouldRemoveBoostModel = true;
      return true;
    case kMsgTagHighlightColor:
    {
      if (pData == nullptr || dataSize <= 0 || dataSize > 32)
        return false;
      std::string colorCode(static_cast<const char*>(pData), static_cast<size_t>(dataSize));
      if (!colorCode.empty() && colorCode.back() == '\0')
        colorCode.pop_back();
      if (colorCode.empty())
        return false;
      mHighLightColor.Set(colorCode.c_str());

      if (GetUI())
      {
        GetUI()->ForStandardControlsFunc([&](IControl* pControl) {
          if (auto* pVectorBase = pControl->As<IVectorBase>())
          {
            IColor color = IColor::FromColorCodeStr(mHighLightColor.Get());

            pVectorBase->SetColor(kX1, color);
            pVectorBase->SetColor(kPR, color.WithOpacity(0.3f));
            pVectorBase->SetColor(kFR, color.WithOpacity(0.4f));
            pVectorBase->SetColor(kX3, color.WithContrast(0.1f));
          }
          pControl->GetUI()->SetAllControlsDirty();
        });
      }

      return true;
    }
    default: return false;
  }
}

// Private methods ============================================================

WDL_String NeuralAmpModeler::_GetNAMPathSnapshot() const
{
  std::lock_guard<std::mutex> lock(mPathMutex);
  return mNAMPath;
}

WDL_String NeuralAmpModeler::_GetIRPathSnapshot() const
{
  std::lock_guard<std::mutex> lock(mPathMutex);
  return mIRPath;
}

WDL_String NeuralAmpModeler::_GetBoostNAMPathSnapshot() const
{
  std::lock_guard<std::mutex> lock(mPathMutex);
  return mBoostNAMPath;
}

void NeuralAmpModeler::_SetNAMPath(const char* path)
{
  std::lock_guard<std::mutex> lock(mPathMutex);
  mNAMPath.Set(path == nullptr ? "" : path);
}

void NeuralAmpModeler::_SetIRPath(const char* path)
{
  std::lock_guard<std::mutex> lock(mPathMutex);
  mIRPath.Set(path == nullptr ? "" : path);
}

void NeuralAmpModeler::_SetIRPathAndRequest(const char* path)
{
  std::lock_guard<std::mutex> lock(mPathMutex);
  mIRPath.Set(path == nullptr ? "" : path);
  mIRLoadRequest.fetch_add(1, std::memory_order_release);
}

void NeuralAmpModeler::_RequestIRReload()
{
  std::lock_guard<std::mutex> lock(mPathMutex);
  mIRLoadRequest.fetch_add(1, std::memory_order_release);
}

void NeuralAmpModeler::_SetBoostNAMPath(const char* path)
{
  std::lock_guard<std::mutex> lock(mPathMutex);
  mBoostNAMPath.Set(path == nullptr ? "" : path);
}

void NeuralAmpModeler::_AllocateIOPointers(const size_t nChans)
{
  if (mInputPointers != nullptr)
    throw std::runtime_error("Tried to re-allocate mInputPointers without freeing");
  mInputPointers = new sample*[nChans];
  if (mInputPointers == nullptr)
    throw std::runtime_error("Failed to allocate pointer to input buffer!\n");
  if (mOutputPointers != nullptr)
    throw std::runtime_error("Tried to re-allocate mOutputPointers without freeing");
  mOutputPointers = new sample*[nChans];
  if (mOutputPointers == nullptr)
    throw std::runtime_error("Failed to allocate pointer to output buffer!\n");
  if (mBoostOutputPointers != nullptr)
    throw std::runtime_error("Tried to re-allocate mBoostOutputPointers without freeing");
  mBoostOutputPointers = new sample*[nChans];
  if (mBoostOutputPointers == nullptr)
    throw std::runtime_error("Failed to allocate pointer to boost output buffer!\n");
}

void NeuralAmpModeler::_ApplyDSPStaging(const bool ampActiveForBlock)
{
  // Never destroy DSP objects on the render thread. A swap/clear is deferred
  // while its single retirement slot is awaiting main-thread reclamation.
  if (mRetiredModel.load(std::memory_order_acquire) == nullptr
      && mShouldRemoveModel.exchange(false, std::memory_order_acq_rel))
  {
    ResamplingNAM* old = mModel.exchange(nullptr, std::memory_order_acq_rel);
    if (old != nullptr)
      mRetiredModel.store(old, std::memory_order_release);
    mModelCleared = true;
  }
  if (mRetiredIR.load(std::memory_order_acquire) == nullptr
      && mShouldRemoveIR.exchange(false, std::memory_order_acq_rel))
  {
    auto* old = mIR.exchange(nullptr, std::memory_order_acq_rel);
    if (old != nullptr)
      mRetiredIR.store(old, std::memory_order_release);
  }
  if (mRetiredBoostModel.load(std::memory_order_acquire) == nullptr
      && mShouldRemoveBoostModel.exchange(false, std::memory_order_acq_rel))
  {
    auto* old = mBoostModel.exchange(nullptr, std::memory_order_acq_rel);
    if (old != nullptr)
      mRetiredBoostModel.store(old, std::memory_order_release);
    mBoostModelCleared = true;
  }

  const std::uint64_t audioEpoch = mAudioConfigEpoch.load(std::memory_order_acquire);
  if (mRetiredModel.load(std::memory_order_acquire) == nullptr)
  {
    auto* current = mModel.load(std::memory_order_acquire);
    const bool currentModelUsable = current != nullptr && current->GetPreparedEpoch() == audioEpoch;
    // A model chosen while AMP is bypassed is fully prepared off the render
    // thread, but adoption waits until AMP is re-enabled. Retaining the live
    // object keeps one continuous identity-resampler history and prevents a
    // bypass-only selection from causing a phase reset or dropout.
    const bool deferBypassedReplacement = !ampActiveForBlock && currentModelUsable;
    if (!deferBypassedReplacement)
    {
      if (auto* pending = mPendingModel.exchange(nullptr, std::memory_order_acq_rel))
      {
        if (pending->GetPreparedEpoch() != audioEpoch
            || pending->GetPreparedRequestToken() != mAmpLoadRequest.load(std::memory_order_acquire)
            || pending->GetPreparedSelection() != mAmpModelIdx.load(std::memory_order_acquire))
          mRetiredModel.store(pending, std::memory_order_release);
        else
        {
          auto* old = mModel.exchange(pending, std::memory_order_acq_rel);
          if (old != nullptr)
            mRetiredModel.store(old, std::memory_order_release);
          _PublishModelMetadata(*pending);
          // Publish model-dependent level changes before this same render block
          // reads mInputGain/mOutputGain. Deferring these calls to OnIdle would
          // process one block using metadata from the previous model.
          _SetInputGain();
          _SetOutputGain();
          mModelCleared = false;
          mNewModelLoadedInDSP = true;
        }
      }
    }
  }
  if (mRetiredBoostModel.load(std::memory_order_acquire) == nullptr)
  {
    if (auto* pending = mPendingBoostModel.exchange(nullptr, std::memory_order_acq_rel))
    {
      if (pending->GetPreparedEpoch() != audioEpoch
          || pending->GetPreparedRequestToken() != mBoostLoadRequest.load(std::memory_order_acquire)
          || pending->GetPreparedSelection() != mBoostModelIdx.load(std::memory_order_acquire)
          || !mBoostActiveTarget.load(std::memory_order_acquire))
        mRetiredBoostModel.store(pending, std::memory_order_release);
      else
      {
        auto* old = mBoostModel.exchange(pending, std::memory_order_acq_rel);
        if (old != nullptr)
          mRetiredBoostModel.store(old, std::memory_order_release);
        mBoostModelCleared = false;
        mNewBoostModelLoadedInDSP = true;
      }
    }
  }
  if (mRetiredIR.load(std::memory_order_acquire) == nullptr)
  {
    if (auto* pending = mPendingIR.exchange(nullptr, std::memory_order_acq_rel))
    {
      if (pending->GetPreparedEpoch() != audioEpoch
          || pending->GetPreparedRequestToken() != mIRLoadRequest.load(std::memory_order_acquire))
        mRetiredIR.store(pending, std::memory_order_release);
      else
      {
        auto* old = mIR.exchange(pending, std::memory_order_acq_rel);
        if (old != nullptr)
          mRetiredIR.store(old, std::memory_order_release);
        mNewIRLoadedInDSP = true;
      }
    }
  }
}

void NeuralAmpModeler::_PublishModelMetadata(ResamplingNAM& model) noexcept
{
  mModelMetadataEpoch.store(model.GetPreparedEpoch(), std::memory_order_relaxed);
  mModelMetadataSelection.store(model.GetPreparedSelection(), std::memory_order_relaxed);
  mModelMetadataLatency.store(model.GetLatency(), std::memory_order_relaxed);
  mModelMetadataHasInputLevel.store(model.HasInputLevel(), std::memory_order_relaxed);
  mModelMetadataHasOutputLevel.store(model.HasOutputLevel(), std::memory_order_relaxed);
  mModelMetadataHasLoudness.store(model.HasLoudness(), std::memory_order_relaxed);
  mModelMetadataInputLevel.store(model.HasInputLevel() ? model.GetInputLevel() : 0.0, std::memory_order_relaxed);
  mModelMetadataOutputLevel.store(model.HasOutputLevel() ? model.GetOutputLevel() : 0.0, std::memory_order_relaxed);
  mModelMetadataLoudness.store(model.HasLoudness() ? model.GetLoudness() : 0.0, std::memory_order_relaxed);
  mModelMetadataSampleRate.store(model.GetEncapsulatedSampleRate(), std::memory_order_relaxed);
  mModelMetadataRequest.store(model.GetPreparedRequestToken(), std::memory_order_release);
}

void NeuralAmpModeler::_DeallocateIOPointers()
{
  if (mInputPointers != nullptr)
  {
    delete[] mInputPointers;
    mInputPointers = nullptr;
  }
  if (mOutputPointers != nullptr)
  {
    delete[] mOutputPointers;
    mOutputPointers = nullptr;
  }
  if (mBoostOutputPointers != nullptr)
  {
    delete[] mBoostOutputPointers;
    mBoostOutputPointers = nullptr;
  }
}

void NeuralAmpModeler::_FallbackDSP(iplug::sample** inputs, iplug::sample** outputs, const size_t numChannels,
                                    const size_t numFrames)
{
  for (auto c = 0; c < numChannels; c++)
    for (auto s = 0; s < numFrames; s++)
      outputs[c][s] = inputs[c][s];
}

void NeuralAmpModeler::_SetInputGain()
{
  iplug::sample inputGainDB = GetParam(kInputLevel)->Value();
  // Acquire the metadata publication before reading its relaxed fields. The
  // metadata describes the currently adopted object, which remains the DSP
  // source while a replacement selection is loading or parked in bypass.
  mModelMetadataRequest.load(std::memory_order_acquire);
  if (mAmpActiveTarget.load(std::memory_order_acquire)
      && mModel.load(std::memory_order_acquire) != nullptr
      && mModelMetadataEpoch.load(std::memory_order_relaxed) == mAudioConfigEpoch.load(std::memory_order_acquire)
      && mModelMetadataHasInputLevel.load(std::memory_order_relaxed) && GetParam(kCalibrateInput)->Bool())
    inputGainDB += GetParam(kInputCalibrationLevel)->Value()
                   - mModelMetadataInputLevel.load(std::memory_order_relaxed);
  mInputGain.store(DBToAmp(inputGainDB), std::memory_order_release);
}

void NeuralAmpModeler::_SetOutputGain()
{
  double gainDB = GetParam(kOutputLevel)->Value();
  mModelMetadataRequest.load(std::memory_order_acquire);
  const bool haveCurrentMetadata = mAmpActiveTarget.load(std::memory_order_acquire)
                                   && mModel.load(std::memory_order_acquire) != nullptr
                                   && mModelMetadataEpoch.load(std::memory_order_relaxed)
                                        == mAudioConfigEpoch.load(std::memory_order_acquire);
  if (haveCurrentMetadata)
  {
    const int outputMode = GetParam(kOutputMode)->Int();
    switch (outputMode)
    {
      case 1: // Normalized
        if (mModelMetadataHasLoudness.load(std::memory_order_relaxed))
        {
          const double targetLoudness = -18.0;
          gainDB += targetLoudness - mModelMetadataLoudness.load(std::memory_order_relaxed);
        }
        break;
      case 2: // Calibrated
        if (mModelMetadataHasOutputLevel.load(std::memory_order_relaxed))
          gainDB += mModelMetadataOutputLevel.load(std::memory_order_relaxed)
                    - GetParam(kInputCalibrationLevel)->Value();
        break;
      case 0: // Raw
      default: break;
    }
  }
  mOutputGain.store(DBToAmp(gainDB), std::memory_order_release);
}

std::string NeuralAmpModeler::_GetAmpModelName(int ampModelValue)
{
  // State 3 is the historical unshipped DessBlock-red slot. It previously
  // fell back to DessTortion-red, so preserve that sound for saved sessions.
  switch (ampModelValue) {
    case 0: return "DessTortion-blue";
    case 1:
    case 3: return "DessTortion-red";
    case 2: return "DessBlock-green";
    case 4: return "SickDess";
    default: return "DessTortion-blue";
  }
}

std::string NeuralAmpModeler::_LoadModelForGain(const std::string& ampModel, int gainValue,
                                                const unsigned int requestToken, const int requestedAmpIndex)
{
  // Clamp gain value to valid range
  if (gainValue < 0) gainValue = 0;
  if (gainValue > 10) gainValue = 10;

  if (ampModel == "Custom") return ""; // Custom model handled separately
  
  // Handle DessTortion-blue (Index 2)
  if (ampModel == "DessTortion-blue")
  {
      // Load static model: DessParametric.nam (remapped)
      // It handles gain via mCurrentParams
      WDL_String resourcePath;
      BundleResourcePath(resourcePath, GetBundleID());
      WDL_String modelPath;
      modelPath.Set(resourcePath.Get());
      modelPath.Append("/models/DessTortion-blue/DessTortion-blue.nam");
      
      if (!std::filesystem::exists(modelPath.Get()))
      {
           std::string errorMsg = "DessTortion-blue model not found: " + std::string(modelPath.Get());
           std::cerr << errorMsg << std::endl;
           return errorMsg;
      }
      return _StageModel(modelPath, requestToken, requestedAmpIndex);
  }
  
  // Handle DessTortion-red (Index 1 in new enum)
  if (ampModel == "DessTortion-red")
  {
      WDL_String resourcePath;
      BundleResourcePath(resourcePath, GetBundleID());
      WDL_String modelPath;
      modelPath.Set(resourcePath.Get());
      modelPath.Append("/models/DessTortion-red/DessTortion-red.nam");
      
      if (!std::filesystem::exists(modelPath.Get()))
      {
           std::string errorMsg = "DessTortion-red model not found: " + std::string(modelPath.Get());
           std::cerr << errorMsg << std::endl;
           return errorMsg;
      }
      return _StageModel(modelPath, requestToken, requestedAmpIndex);
  }
  
  // Handle DessBlock-green (Index 2 in the enum)
  if (ampModel == "DessBlock-green")
  {
      WDL_String resourcePath;
      BundleResourcePath(resourcePath, GetBundleID());
      WDL_String modelPath;
      modelPath.Set(resourcePath.Get());
      modelPath.Append("/models/DessBlock-green/model.nam");
      
      if (!std::filesystem::exists(modelPath.Get()))
      {
           return "DessBlock-green model not found: " + std::string(modelPath.Get());
      }
      return _StageModel(modelPath, requestToken, requestedAmpIndex);
  }

  if (ampModel == "SickDess")
  {
      WDL_String resourcePath;
      BundleResourcePath(resourcePath, GetBundleID());
      WDL_String modelPath;
      modelPath.Set(resourcePath.Get());
      modelPath.Append("/models/SickDess/SickDess.nam");

      if (!std::filesystem::exists(modelPath.Get()))
      {
        return "SickDess model not found: " + std::string(modelPath.Get());
      }
      return _StageModel(modelPath, requestToken, requestedAmpIndex);
  }

  // Legacy code for non-parametric models (should not be reached with new enum)
  // Lock gain for DessTortion (Only 5 and 6 available)
  if (ampModel == "DessTortion")
  {
      if (gainValue <= 5) gainValue = 5;
      else gainValue = 6;
  }
  
  // Build model filename: {ampModel}-{gainValue} (e.g., "DessBlock-1", "DessCut-5")
  std::stringstream ss;
  ss << ampModel << "-" << gainValue;
  std::string modelBaseName = ss.str();
  
  // Get resource path - iPlug2 stores resources in the bundle
  WDL_String resourcePath;
  BundleResourcePath(resourcePath, GetBundleID());
  
  // Build path: models/{ampModel}/{ampModel}-{gainValue}.nam
  WDL_String modelPath;
  modelPath.Set(resourcePath.Get());
  modelPath.Append("/models/");
  modelPath.Append(ampModel.c_str());
  modelPath.Append("/");
  modelPath.Append(modelBaseName.c_str());
  modelPath.Append(".nam");
  
  // Check if .nam file exists
  if (!std::filesystem::exists(modelPath.Get()))
  {
    // Try directory format (config.json + weights.npy)
    modelPath.Set(resourcePath.Get());
    modelPath.Append("/models/");
    modelPath.Append(ampModel.c_str());
    modelPath.Append("/");
    modelPath.Append(modelBaseName.c_str());
    modelPath.Append("/config.json");
    if (!std::filesystem::exists(modelPath.Get()))
    {
      std::string errorMsg = "Model file not found: " + modelBaseName;
      std::cerr << errorMsg << std::endl;
      return errorMsg;
    }
    // Remove /config.json to get directory path
    modelPath.remove_filepart();
  }
  
  return _StageModel(modelPath, requestToken, requestedAmpIndex);
}

std::string NeuralAmpModeler::_StageModel(const WDL_String& modelPath, const unsigned int requestToken,
                                          const int requestedAmpIndex)
{
  const std::uint64_t preparedEpoch = mAudioConfigEpoch.load(std::memory_order_acquire);
  if ((preparedEpoch & 1U) != 0U)
    return "";
  const double sampleRate = mAudioSampleRate.load(std::memory_order_acquire);
  const int maxBlockSize = mAudioMaxBlockSize.load(std::memory_order_acquire);
  try
  {
    auto dspPath = std::filesystem::u8path(modelPath.Get());
    std::unique_ptr<nam::DSP> model = nam::get_dsp(dspPath);
    std::unique_ptr<ResamplingNAM> temp = std::make_unique<ResamplingNAM>(std::move(model), sampleRate);
    const double defaultCondition = mTargetGain.load(std::memory_order_relaxed);
    temp->SetDefaultParams(&defaultCondition, 1);
    temp->Reset(sampleRate, maxBlockSize);
    temp->SetPreparedEpoch(preparedEpoch);
    temp->SetPreparedRequest(requestToken, requestedAmpIndex);
    temp->SetPreparedPath(modelPath.Get());

    if (preparedEpoch != mAudioConfigEpoch.load(std::memory_order_acquire)
        || requestToken != mAmpLoadRequest.load(std::memory_order_acquire)
        || requestedAmpIndex != mAmpModelIdx.load(std::memory_order_acquire))
      return "";

    delete mPendingModel.exchange(temp.release(), std::memory_order_acq_rel);
  }
  catch (const std::exception& e)
  {
    std::cerr << "Failed to read DSP module" << std::endl;
    std::cerr << e.what() << std::endl;
    return e.what();
  }
  return "";
}

dsp::wav::LoadReturnCode NeuralAmpModeler::_StageIR(const WDL_String& irPath, const unsigned int requestToken)
{
  const std::uint64_t preparedEpoch = mAudioConfigEpoch.load(std::memory_order_acquire);
  if ((preparedEpoch & 1U) != 0U)
    return dsp::wav::LoadReturnCode::SUCCESS;
  const double sampleRate = mAudioSampleRate.load(std::memory_order_acquire);
  const int maxBlockSize = mAudioMaxBlockSize.load(std::memory_order_acquire);
  dsp::wav::LoadReturnCode wavState = dsp::wav::LoadReturnCode::ERROR_OTHER;
  try
  {
    auto irPathU8 = std::filesystem::u8path(irPath.Get());
    auto temp = std::make_unique<dsp::ImpulseResponse>(irPathU8.string().c_str(), sampleRate);
    wavState = temp->GetWavState();
    if (wavState == dsp::wav::LoadReturnCode::SUCCESS)
    {
      temp->PrepareBuffers(kNumChannelsInternal, maxBlockSize);
      temp->SetPreparedEpoch(preparedEpoch);
      temp->SetPreparedRequestToken(requestToken);
      if (preparedEpoch != mAudioConfigEpoch.load(std::memory_order_acquire)
          || requestToken != mIRLoadRequest.load(std::memory_order_acquire))
        return dsp::wav::LoadReturnCode::SUCCESS;
      delete mPendingIR.exchange(temp.release(), std::memory_order_acq_rel);
    }
  }
  catch (const std::exception& e)
  {
    wavState = dsp::wav::LoadReturnCode::ERROR_OTHER;
    std::cerr << "Caught unhandled exception while attempting to load IR:" << std::endl;
    std::cerr << e.what() << std::endl;
  }

  return wavState;
}



size_t NeuralAmpModeler::_GetBufferNumChannels() const
{
  // Assumes input=output (no mono->stereo effects)
  return mInputArray.size();
}

size_t NeuralAmpModeler::_GetBufferNumFrames() const
{
  if (_GetBufferNumChannels() == 0)
    return 0;
  return mInputArray[0].size();
}

void NeuralAmpModeler::_InitToneStack()
{
  // If you want to customize the tone stack, then put it here!
  mToneStack = std::make_unique<dsp::tone_stack::BasicNamToneStack>();
}
void NeuralAmpModeler::_PrepareBuffers(const size_t numChannels, const size_t numFrames)
{
  const bool updateChannels = numChannels != _GetBufferNumChannels();
  const bool updateFrames = updateChannels || (_GetBufferNumFrames() != numFrames);
  //  if (!updateChannels && !updateFrames)  // Could we do this?
  //    return;

  if (updateChannels)
  {
    _PrepareIOPointers(numChannels);
    mInputArray.resize(numChannels);
    mOutputArray.resize(numChannels);
    mBoostOutputArray.resize(numChannels);
  }
  if (updateFrames)
  {
    for (auto c = 0; c < mInputArray.size(); c++)
    {
      mInputArray[c].resize(numFrames);
      std::fill(mInputArray[c].begin(), mInputArray[c].end(), 0.0);
    }
    for (auto c = 0; c < mOutputArray.size(); c++)
    {
      mOutputArray[c].resize(numFrames);
      std::fill(mOutputArray[c].begin(), mOutputArray[c].end(), 0.0);
    }
    for (auto c = 0; c < mBoostOutputArray.size(); c++)
    {
      mBoostOutputArray[c].resize(numFrames);
      std::fill(mBoostOutputArray[c].begin(), mBoostOutputArray[c].end(), 0.0);
    }
  }
  // Would these ever get changed by something?
  for (auto c = 0; c < mInputArray.size(); c++)
    mInputPointers[c] = mInputArray[c].data();
  for (auto c = 0; c < mOutputArray.size(); c++)
    mOutputPointers[c] = mOutputArray[c].data();
  for (auto c = 0; c < mBoostOutputArray.size(); c++)
    mBoostOutputPointers[c] = mBoostOutputArray[c].data();
}

void NeuralAmpModeler::_PrepareIOPointers(const size_t numChannels)
{
  _DeallocateIOPointers();
  _AllocateIOPointers(numChannels);
}

void NeuralAmpModeler::_ProcessInput(iplug::sample** inputs, const size_t nFrames, const size_t nChansIn,
                                     const size_t nChansOut)
{
  if (nChansOut != 1 || mInputArray.empty())
    return;

  // Every block starts from silence so zero-input configurations cannot reuse
  // samples from the previous callback.
  std::fill(mInputArray[0].begin(), mInputArray[0].begin() + nFrames, 0.0);
  if (nChansIn == 0 || inputs == nullptr)
    return;

  // On the standalone, we can probably assume that the user has plugged into only one input and they expect it to be
  // carried straight through. Don't apply any division over nChansIn because we're just "catching anything out there."
  // However, in a DAW, it's probably something providing stereo, and we want to take the average in order to avoid
  // doubling the loudness. (This would change w/ double mono processing)
  double gain = mInputGain.load(std::memory_order_acquire);
#ifndef APP_API
  gain /= (float)nChansIn;
#endif
  // Assume _PrepareBuffers() was already called
  for (size_t c = 0; c < nChansIn; c++)
    for (size_t s = 0; s < nFrames; s++)
      if (c == 0)
        mInputArray[0][s] = gain * inputs[c][s];
      else
        mInputArray[0][s] += gain * inputs[c][s];
}

void NeuralAmpModeler::_ProcessOutput(iplug::sample** inputs, iplug::sample** outputs, const size_t nFrames,
                                      const size_t nChansIn, const size_t nChansOut)
{
  const double gain = mOutputGain.load(std::memory_order_acquire);
  // Assume _PrepareBuffers() was already called
  if (nChansIn != 1 || inputs == nullptr)
  {
    for (size_t cout = 0; cout < nChansOut; ++cout)
      if (outputs != nullptr && outputs[cout] != nullptr)
        std::fill(outputs[cout], outputs[cout] + nFrames, 0.0);
    return;
  }
  // Broadcast the internal mono stream to all output channels.
  const size_t cin = 0;
  for (auto cout = 0; cout < nChansOut; cout++)
    for (auto s = 0; s < nFrames; s++)
#ifdef APP_API // Ensure valid output to interface
      outputs[cout][s] = std::clamp(gain * inputs[cin][s], -1.0, 1.0);
#else // In a DAW, other things may come next and should be able to handle large
      // values.
      outputs[cout][s] = gain * inputs[cin][s];
#endif
}

void NeuralAmpModeler::_ValidateOutputModeForCurrentModel()
{
  const int outputMode = GetParam(kOutputMode)->Int();
  if (outputMode == 0)
    return;

  const bool metadataMatchesCurrentModel =
    mModelMetadataRequest.load(std::memory_order_acquire) == mAmpLoadRequest.load(std::memory_order_acquire)
    && mModelMetadataSelection.load(std::memory_order_relaxed) == mAmpModelIdx.load(std::memory_order_acquire)
    && mModelMetadataEpoch.load(std::memory_order_relaxed) == mAudioConfigEpoch.load(std::memory_order_acquire);
  if (!metadataMatchesCurrentModel)
    return;

  const bool supported = outputMode == 1 ? mModelMetadataHasLoudness.load(std::memory_order_relaxed)
                                        : mModelMetadataHasOutputLevel.load(std::memory_order_relaxed);
  if (!supported && GetParam(kOutputMode)->Int() == outputMode)
    _SetParameterValueFromMainThread(kOutputMode, 0.0);
}

void NeuralAmpModeler::_SetParameterValueFromMainThread(const int paramIdx, const double normalizedValue)
{
  // OnIdle is a non-render thread. Bracket programmatic corrections as a
  // complete host gesture so AU/VST3 generic controls and automation receive
  // the same value used by the DSP and custom editor.
  BeginInformHostOfParamChangeFromUI(paramIdx);
  SetParameterValue(paramIdx, normalizedValue);
  EndInformHostOfParamChangeFromUI(paramIdx);
  SendParameterValueFromDelegate(paramIdx, normalizedValue, true);
}

void NeuralAmpModeler::_UpdateControlsFromModel()
{
  ModelInfo modelInfo;
  const unsigned int metadataRequest = mModelMetadataRequest.load(std::memory_order_acquire);
  if (metadataRequest != mAmpLoadRequest.load(std::memory_order_acquire)
      || mModelMetadataSelection.load(std::memory_order_relaxed) != mAmpModelIdx.load(std::memory_order_acquire)
      || mModelMetadataEpoch.load(std::memory_order_relaxed) != mAudioConfigEpoch.load(std::memory_order_acquire))
    return;
  const bool hasInputLevel = mModelMetadataHasInputLevel.load(std::memory_order_relaxed);
  const bool hasOutputLevel = mModelMetadataHasOutputLevel.load(std::memory_order_relaxed);
  const bool hasLoudness = mModelMetadataHasLoudness.load(std::memory_order_relaxed);

  modelInfo.sampleRate.known = true;
  modelInfo.sampleRate.value = mModelMetadataSampleRate.load(std::memory_order_relaxed);
  modelInfo.inputCalibrationLevel.known = hasInputLevel;
  modelInfo.inputCalibrationLevel.value = mModelMetadataInputLevel.load(std::memory_order_relaxed);
  modelInfo.outputCalibrationLevel.known = hasOutputLevel;
  modelInfo.outputCalibrationLevel.value = mModelMetadataOutputLevel.load(std::memory_order_relaxed);
  if (auto* pGraphics = GetUI())
  {
    auto* settingsControl =
      static_cast<NAMSettingsPageControl*>(pGraphics->GetControlWithTag(kCtrlTagSettingsBox));
    settingsControl->SetModelInfo(modelInfo);
    settingsControl->SetInputCalibrationAvailable(hasInputLevel);

    pGraphics->GetControlWithTag(kCtrlTagCalibrateInput)->SetDisabled(!hasInputLevel);
    pGraphics->GetControlWithTag(kCtrlTagInputCalibrationLevel)->SetDisabled(!hasInputLevel);
    {
      auto* c = static_cast<OutputModeControl*>(pGraphics->GetControlWithTag(kCtrlTagOutputMode));
      c->SetNormalizedDisable(!hasLoudness);
      c->SetCalibratedDisable(!hasOutputLevel);
    }
  }
}

void NeuralAmpModeler::_UpdateLatency()
{
  int latency = 0;
  // Latency follows the adopted processing object, not the newest requested
  // selection. A prepared replacement can wait in the handoff slot without
  // changing either the live signal path or host PDC.
  mModelMetadataRequest.load(std::memory_order_acquire);
  if (mModel.load(std::memory_order_acquire) != nullptr
      && mModelMetadataEpoch.load(std::memory_order_relaxed) == mAudioConfigEpoch.load(std::memory_order_acquire))
    latency += mModelMetadataLatency.load(std::memory_order_relaxed);
  // Other things that add latency here...
  // Transpose uses causal analytic filters without a buffered look-ahead, so
  // its host-reported latency is intentionally zero at every semitone value.

  // Feels weird to have to do this.
  if (GetLatency() != latency)
  {
    SetLatency(latency);
  }
}

void NeuralAmpModeler::_UpdateMeters(sample** inputPointer, sample** outputPointer, const size_t nFrames,
                                     const size_t nChansIn, const size_t nChansOut)
{
  // Right now, we didn't specify MAXNC when we initialized these, so it's 1.
  const int nChansHack = 1;
  mInputSender.ProcessBlock(inputPointer, (int)nFrames, kCtrlTagInputMeter, nChansHack);
  mOutputSender.ProcessBlock(outputPointer, (int)nFrames, kCtrlTagOutputMeter, nChansHack);
}


void NeuralAmpModeler::_InitBoost()
{
  mBoostModel.store(nullptr, std::memory_order_release);
  _SetBoostNAMPath("");
}

void NeuralAmpModeler::_ApplyBoost(iplug::sample** inputs, const size_t numChannels, const size_t numFrames)
{
  if (auto* boostModel = mBoostModel.load(std::memory_order_acquire))
  {
      // Process Boost NAM (Mono)
      boostModel->process(inputs, inputs, (int)numFrames);
      
      // Output Gain
      // User requested 0dB (Unity) at middle (5.0).
      // Scale: 5.0 = 0dB. Range +/- 15dB (3dB per step).
      // 0.0 -> -15dB, 10.0 -> +15dB.
      const float boostVal = (float)GetParam(kBoostOutput)->Value();
      const float dbGain = (boostVal - 5.0f) * 3.0f; 
      const float outputLevel = std::pow(10.0f, dbGain / 20.0f);
      
      for (size_t s = 0; s < numFrames; ++s)
      {
          inputs[0][s] *= outputLevel;
      }
  }
}

std::string NeuralAmpModeler::_StageBoostModel(const WDL_String& modelPath, const unsigned int requestToken,
                                               const int requestedBoostIndex)
{
  const std::uint64_t preparedEpoch = mAudioConfigEpoch.load(std::memory_order_acquire);
  if ((preparedEpoch & 1U) != 0U)
    return "";
  const double sampleRate = mAudioSampleRate.load(std::memory_order_acquire);
  const int maxBlockSize = mAudioMaxBlockSize.load(std::memory_order_acquire);
  try
  {
    auto dspPath = std::filesystem::u8path(modelPath.Get());
    std::unique_ptr<nam::DSP> model = nam::get_dsp(dspPath);
    std::unique_ptr<ResamplingNAM> temp = std::make_unique<ResamplingNAM>(std::move(model), sampleRate);
    temp->Reset(sampleRate, maxBlockSize);
    temp->SetPreparedEpoch(preparedEpoch);
    temp->SetPreparedRequest(requestToken, requestedBoostIndex);
    temp->SetPreparedPath(modelPath.Get());
    if (preparedEpoch != mAudioConfigEpoch.load(std::memory_order_acquire)
        || requestToken != mBoostLoadRequest.load(std::memory_order_acquire)
        || requestedBoostIndex != mBoostModelIdx.load(std::memory_order_acquire)
        || !mBoostActiveTarget.load(std::memory_order_acquire))
      return "";
    delete mPendingBoostModel.exchange(temp.release(), std::memory_order_acq_rel);
  }
  catch (const std::exception& e)
  {
    return e.what();
  }
  return "";
}

std::string NeuralAmpModeler::_LoadBoostModel(const std::string& modelName, const unsigned int requestToken,
                                              const int requestedBoostIndex)
{
  WDL_String resourcePath;
  BundleResourcePath(resourcePath, GetBundleID());
  
  if (resourcePath.GetLength() == 0) return "Failed to get resource path";

  WDL_String path; 
  path.Set(resourcePath.Get());
  path.Append("/models/DessDrive/");
  path.Append(modelName.c_str());
  path.Append(".nam");
  
  return _StageBoostModel(path, requestToken, requestedBoostIndex);
}

std::string NeuralAmpModeler::_GetBoostModelName(int index)
{
  switch(index) {
    case 0: return "OD808";
    case 1: return "SD1";
    case 2: return "TS9";
    case 3: return "aesahaettr";
    default: return "OD808";
  }
}

// HACK
#include "Unserialization.cpp"

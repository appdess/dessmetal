
#include <TargetConditionals.h>
#if TARGET_OS_IOS == 1
  #import <UIKit/UIKit.h>
#else
  #import <Cocoa/Cocoa.h>
#endif

#define IPLUG_AUVIEWCONTROLLER IPlugAUViewController_vDessMetal
#define IPLUG_AUAUDIOUNIT IPlugAUAudioUnit_vDessMetal
#import <NeuralAmpModelerAU/IPlugAUAudioUnit.h>
#import <NeuralAmpModelerAU/IPlugAUViewController.h>

//! Project version number for NeuralAmpModelerAU.
FOUNDATION_EXPORT double NeuralAmpModelerAUVersionNumber;

//! Project version string for NeuralAmpModelerAU.
FOUNDATION_EXPORT const unsigned char NeuralAmpModelerAUVersionString[];

@class IPlugAUViewController_vDessMetal;

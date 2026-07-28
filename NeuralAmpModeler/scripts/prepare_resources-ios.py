#!/usr/bin/env python3

# this script will create/update info plist files based on config.h

kAudioUnitType_MusicDevice = "aumu"
kAudioUnitType_MusicEffect = "aumf"
kAudioUnitType_Effect = "aufx"
kAudioUnitType_MIDIProcessor = "aumi"

import plistlib, os, datetime, fileinput, glob, sys, string, shutil

scriptpath = os.path.dirname(os.path.realpath(__file__))
projectpath = os.path.abspath(os.path.join(scriptpath, os.pardir))

IPLUG2_ROOT = "../../iPlug2"

sys.path.insert(0, os.path.join(os.getcwd(), IPLUG2_ROOT + "/Scripts"))

from parse_config import parse_config, parse_xcconfig


def main():
    if len(sys.argv) == 2:
        if sys.argv[1] == "app":
            print("Copying resources ...")

            dst = (
                os.environ["TARGET_BUILD_DIR"]
                + "/"
                + os.environ["UNLOCALIZED_RESOURCES_FOLDER_PATH"]
            )

            if os.path.exists(projectpath + "/resources/img/"):
                imgs = os.listdir(projectpath + "/resources/img/")
                for img in imgs:
                    img_path = os.path.join(projectpath + "/resources/img/", img)
                    if os.path.isfile(img_path):
                        print("copying " + img + " to " + dst)
                        shutil.copy(img_path, dst)
                    elif os.path.isdir(img_path):
                        # Copy subdirectories (e.g., DessTortion/)
                        dst_subdir = os.path.join(dst, img)
                        if not os.path.exists(dst_subdir):
                            os.makedirs(dst_subdir, 0o0755)
                        for subitem in os.listdir(img_path):
                            subitem_path = os.path.join(img_path, subitem)
                            if os.path.isfile(subitem_path):
                                print("copying " + img + "/" + subitem + " to " + dst_subdir)
                                shutil.copy(subitem_path, dst_subdir)

            if os.path.exists(projectpath + "/resources/fonts/"):
                fonts = os.listdir(projectpath + "/resources/fonts/")
                for font in fonts:
                    print("copying " + font + " to " + dst)
                    shutil.copy(projectpath + "/resources/fonts/" + font, dst)

            # Copy NAM model files from subdirectories
            models_dir = os.path.join(projectpath, "resources", "models")
            if os.path.exists(models_dir):
                # Dynamically discover all amp model subdirectories (excluding IRs)
                for amp_model in os.listdir(models_dir):
                    amp_dir = os.path.join(models_dir, amp_model)
                    if os.path.isdir(amp_dir) and amp_model != "IRs":
                        # Create subdirectory in destination to preserve structure
                        dst_models_dir = os.path.join(dst, "models", amp_model)
                        if not os.path.exists(dst_models_dir):
                            os.makedirs(dst_models_dir, 0o0755)
                        
                        # Copy all files from the amp model directory
                        for item in os.listdir(amp_dir):
                            item_path = os.path.join(amp_dir, item)
                            if os.path.isfile(item_path):
                                print("copying model file " + amp_model + "/" + item + " to " + dst_models_dir)
                                shutil.copy(item_path, dst_models_dir)

            # Copy IR wavs (baked-in IRs)
            ir_dir = os.path.join(projectpath, "resources", "models", "IRs")
            if os.path.exists(ir_dir):
                dst_ir_dir = os.path.join(dst, "IRs")
                if not os.path.exists(dst_ir_dir):
                    os.makedirs(dst_ir_dir, 0o0755)
                for item in os.listdir(ir_dir):
                    item_path = os.path.join(ir_dir, item)
                    if os.path.isfile(item_path) and item.lower().endswith(".wav"):
                        print("copying IR " + item + " to " + dst_ir_dir)
                        shutil.copy(item_path, dst_ir_dir)


if __name__ == "__main__":
    main()

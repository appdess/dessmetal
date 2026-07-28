#!/usr/bin/env python3

# this script will copy the project's resources (pngs, ttfs, svgs etc) to the correct place
# depending on the value of PLUG_SHARED_RESOURCES in config.h
# resources can either be copied into the plug-in bundle or into a shared path
# since the shared path should be accesible from the mac app sandbox,
# the path used is ~/Music/SHARED_RESOURCES_SUBPATH

import os
import shutil
import sys

scriptpath = os.path.dirname(os.path.realpath(__file__))
projectpath = os.path.abspath(os.path.join(scriptpath, os.pardir))

IPLUG2_ROOT = "../../iPlug2"

sys.path.insert(0, os.path.join(projectpath, "../iPlug2/Scripts"))

from parse_config import parse_config


def main():
    config = parse_config(projectpath)

    print("Copying resources ...")

    if config["PLUG_SHARED_RESOURCES"]:
        dst = (
            os.path.expanduser("~")
            + "/Music/"
            + config["SHARED_RESOURCES_SUBPATH"]
            + "/Resources"
        )
    else:
        dst = os.path.join(
            os.environ["TARGET_BUILD_DIR"],
            os.environ["UNLOCALIZED_RESOURCES_FOLDER_PATH"]
        )

    if os.path.exists(dst) == False:
        os.makedirs(dst + "/", 0o0755)

    image_root = os.path.join(projectpath, "resources", "img")
    image_files = [
        "ArrowLeft.svg",
        "ArrowRight.svg",
        "Background.jpg",
        "Background@2x.jpg",
        "Background@3x.jpg",
        "Cross.svg",
        "File.svg",
        "FileBackground.png",
        "FileBackground@2x.png",
        "FileBackground@3x.png",
        "Gear.svg",
        "Globe.svg",
        "IRIconOff.svg",
        "IRIconOn.svg",
        "InputLevelBackground.png",
        "InputLevelBackground@2x.png",
        "InputLevelBackground@3x.png",
        "KnobBackground.png",
        "KnobBackground@2x.png",
        "KnobBackground@3x.png",
        "Lines.png",
        "Lines@2x.png",
        "Lines@3x.png",
        "MeterBackground.png",
        "MeterBackground@2x.png",
        "MeterBackground@3x.png",
        "ModelIcon.svg",
        "SlideSwitchHandle.png",
        "SlideSwitchHandle@2x.png",
        "SlideSwitchHandle@3x.png",
        "DessMetal/Background.png",
        "DessMetal/DessBlock-green.jpg",
        "DessMetal/DessTortion-blue.jpg",
        "DessMetal/DessTortion-red.jpg",
        "DessMetal/SickDess.jpg",
    ]
    for relative_path in image_files:
        source = os.path.join(image_root, relative_path)
        target_dir = os.path.join(dst, os.path.dirname(relative_path))
        os.makedirs(target_dir, mode=0o0755, exist_ok=True)
        print("copying " + relative_path + " to " + target_dir)
        shutil.copy(source, target_dir)

    if os.path.exists(projectpath + "/resources/fonts/"):
        for font in ["Michroma-Regular.ttf", "Roboto-Regular.ttf"]:
            print("copying " + font + " to " + dst)
            shutil.copy(projectpath + "/resources/fonts/" + font, dst)

    # Exact owner-approved release captures. Do not silently ship training
    # remnants, alternate filenames, or future model experiments.
    models_dir = os.path.join(projectpath, "resources", "models")
    model_files = [
        "DessTortion-blue/DessTortion-blue.nam",
        "DessTortion-red/DessTortion-red.nam",
        "DessBlock-green/model.nam",
        "SickDess/SickDess.nam",
        "DessDrive/OD808.nam",
        "DessDrive/SD1.nam",
        "DessDrive/TS9.nam",
        "DessDrive/aesahaettr.nam",
    ]
    for relative_path in model_files:
        source = os.path.join(models_dir, relative_path)
        target_dir = os.path.join(dst, "models", os.path.dirname(relative_path))
        os.makedirs(target_dir, mode=0o0755, exist_ok=True)
        print("copying model file " + relative_path + " to " + target_dir)
        shutil.copy(source, target_dir)

    # Copy IR wavs (baked-in IRs)
    ir_dir = os.path.join(projectpath, "resources", "models", "IRs")
    if os.path.exists(ir_dir):
        dst_ir_dir = os.path.join(dst, "IRs")
        if not os.path.exists(dst_ir_dir):
            os.makedirs(dst_ir_dir, 0o0755)
        # ENGL-Mix-5153-Rhythm.wav is byte-identical to
        # ENGL-5153-V30-Sheffield-Mix.wav; ship only the canonical filename.
        excluded_irs = {"ENGL-Mix-5153-Rhythm.wav"}
        for item in sorted(os.listdir(ir_dir)):
            item_path = os.path.join(ir_dir, item)
            if os.path.isfile(item_path) and item.lower().endswith(".wav") and item not in excluded_irs:
                print("copying IR " + item + " to " + dst_ir_dir)
                shutil.copy(item_path, dst_ir_dir)


if __name__ == "__main__":
    main()

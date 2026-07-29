# PANAMA: PArametric Neural Amp Modeling with Active learning

This is the implementation of our parametric amp modelling framework. Our code is based on [NAM](https://www.neuralampmodeler.com). Demos can be found [here](https://eth-disco.github.io/PANAMA/).

## Setup

Create the training environment from
`trainer_source/environments/environment_gpu.yml` on an NVIDIA/CUDA host.
`trainer_source/environments/environment_cpu_apple.yml` is available for
macOS/CPU inspection and testing, but both checked-in learning configs set
`trainer.accelerator` to `gpu`; CPU training requires an intentional config
change.

## DessMetal single-parameter training

Run the following commands from the repository root. The checked-in
`trainer_source/parametric_wavenet.json` uses a WaveNet with
`global_condition_size = 1`, so each training point supplies exactly one
normalized value in `g_vector`.

The shared input at `trainer_source/training_data/input.wav` is a 190-second,
mono, 48 kHz, 24-bit PCM file. The DessMetal data configs use these mappings:

- Amp gain captures use `g_vector` values from `0.2` through `0.8`, matching
  plug-in Gain values 2 through 8.
- TS9 and aesahaettr tone captures use values from `0.0` through `1.0`,
  matching the plug-in's 0 through 10 control range.

The checked-in `parametric_data_*.json` paths are relative to the repository
root, so run training commands from that directory. The shared input is
included, but the re-amped capture WAVs are intentionally excluded. Restore
them at the referenced paths—or update the relevant output `y_path` entries—
before starting a run.

The cloud helper contains capture-machine calibration defaults. They are job
inputs, not authoritative metadata for the currently shipped NAM files; for
example, the shipped Blue model records a different return level and the
shipped Red model has null calibration fields. Confirm the actual send/return
chain for the restored capture before recording or exporting a replacement,
then verify the exported NAM metadata explicitly.

Five model-specific configs are currently present:

- `parametric_data_dessblock_green.json`
- `parametric_data_dessblock_red.json`
- `parametric_data_sickdess.json`
- `parametric_data_ts9.json`
- `parametric_data_aesahaettr.json`

Train one restored dataset with:

```bash
python3 trainer_source/custom_train_full.py \
  --base-outdir outputs/parametric_training_dessblock_green \
  --data-config trainer_source/parametric_data_dessblock_green.json \
  --model-config trainer_source/parametric_wavenet.json \
  --learning-config trainer_source/default_config_files/learning/default.json \
  --epochs 800 \
  --threshold-esr 0.009 \
  --check-val-every-n-epoch 5
```

`0.009` is the existing project target, not a promise that every capture will
reach that ESR. Training writes a timestamped directory below
`--base-outdir`. Export from that directory into a review location first:

```bash
python3 trainer_source/export_model.py \
  --input-level SEND_DBU \
  --output-level RETURN_DBU \
  outputs/parametric_training_MODEL_NAME/TIMESTAMP \
  outputs/exported/MODEL_NAME
```

The exporter selects the checkpoint with the lowest ESR encoded in its
filename, falling back to the most recently modified checkpoint when the
metric cannot be parsed. Review the exported model and stats before replacing
any shipped resource. After an intentional replacement, rebuild all macOS
formats through the credential-free release gate:

```bash
./package_mac.sh --unsigned
```

For the Vertex AI helper, including its remote-data and billing effects, see
[`cloud/README.md`](cloud/README.md).

The upstream PANAMA examples below assume the current directory is
`trainer_source/`; the DessMetal commands above intentionally run from the
repository root.

## Config JSONs

There are three types of config JSONs:

- **Model config**, which defines the model architecture as well as the loss function. Default ones are provided in `default_config_files/models/`. `wavenet-mel-mrstft.json` and `lstm-mel-mrstft.json` are recommended. The global condition size (i.e. number of amp knobs) is by default 6, but you can change it using the JSON keys `global_condition_size` for WaveNet and `conditioning_dim` for LSTM.
- **Learning config**, which provides the batch size, number of epochs and other training hyperparameters. `default_config_files/learning/default.json` shows the default learning config. We observed that in general, a larger batch size works better with a larger dataset, and vice versa.
- **Data config**, which supplies the paths of audio files in a dataset. The dataset format requires supplying a fixed input signal, and wet signals with different amp settings. We provide a mini example in `default_config_files/data/skeleton.json` with 2 training points and 1 validation point. The JSON key `ny` corresponds to the length of each audio clip in a batch. The training input signal from NAM is provided in `training_data/input.wav` for convenience.

You can always use `--help` to see all command-line options.

## Inference

Inference can be done with `inference_w_ckpt.py`. Use a checkpoint produced by
the current trainer together with its matching model config. The tracked
`demo_ckpt.ckpt` is retained as a legacy deserialization fixture, but its layer
layout does not match the current model configs.

Checkpoint files are deserialized in PyTorch's restricted `weights_only=True`
mode and must contain a non-empty tensor `state_dict` plus a positive sample
rate. Framework-managed resume options that bypass this loader are rejected,
and the utilities do not fall back to pickle-capable loading. PyTorch 2.6.0 or
newer is required because older releases are affected by
[CVE-2025-32434](https://github.com/advisories/GHSA-53q9-r3pm-6pq6), including
when `weights_only=True` is requested; older or unverifiable versions fail
closed. Treat model weights as untrusted data nonetheless, and separately
verify a checkpoint's SHA-256 against a trusted source before using it when
provenance matters.

Usage example:

```bash
python3 inference_w_ckpt.py --ckpt-path "path/to/current-model.ckpt" --input-path "my_input.wav" --g-vector 0.5 0.2 0.5 0.7 0.5 0.8 --output-dir .
```

## Single Model Training

Training is done with `custom_train_full.py`. The results will be in a subdirectory inside of `--base-outdir`.

Usage example:

```bash
python3 custom_train_full.py --base-outdir "outputs" --data-config "my_dataset.json" --model-config "default_config_files/models/wavenet-mel-mrstft.json" --learning-config "default_config_files/learning/default.json"
```

## Active Learning

`active_learner_multi_gpu.py` performs one round of our active learning method. We recommend making an output directory specifically for active learning. This ***same*** directory, for example `my_active_learning_folder`, should be supplied each round along with the current round number. Round indices start at 0. The 0-th round needs a starter dataset supplied in `--starter-data-config-path`; starting from round 1 this is no longer needed. 10 random datapoints should work fine for the starter. The signal x used during optimization of cross-model disagreement with regard to g is supplied via `--x-path-for-g-opt`. By default, the ensemble size is 4, but can be adjusted via `--ensemble-size`. Make sure you have a multi-GPU setup, and that the number of GPUs is at least ensemble size + 1.

When the script terminates, it logs the g-vectors of the new datapoints to be gathered. After recording and supplying the new audio clips in `active_learning_inputs`, rerun it with the ***same*** output directory and the new round index. Rinse and repeat.

Usage example:

```bash
python3 active_learner_multi_gpu.py --starter-data-config-path "my_starter_dataset.json" --current-round-idx 0 --x-path-for-g-opt "my_input_for_g_optimization.wav" --output-dir "my_active_learning_folder" --ensemble-size 4 --model-config-path "default_config_files/models/lstm-mel-mrstft.json" --learning-config-path "default_config_files/learning/active_learning_LSTM.json"
# Record new datapoints
python3 active_learner_multi_gpu.py --current-round-idx 1 --x-path-for-g-opt "my_input_for_g_optimization.wav" --output-dir "my_active_learning_folder" --ensemble-size 4 --model-config-path "default_config_files/models/lstm-mel-mrstft.json" --learning-config-path "default_config_files/learning/active_learning_LSTM.json"
# Record new datapoints, then repeat with the next round index.
```

## Testing

Test a model with `test_model.py`. This (ideally) requires a separate data config from the training one. The config should still be the same JSON format, but only the `validation` split will be used. The result metrics will be stored as a JSON file and visualized as a plot.

Usage example:

```bash
python3 test_model.py --data-config-path "my_test_data_config.json" --ckpt-path "model_to_be_tested.ckpt" --metrics-path "results.json" --plot-path "results.png"
```

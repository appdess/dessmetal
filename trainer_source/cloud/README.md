# DessMetal Vertex AI training helper

The scripts in this directory can build and push a training container, upload
capture data and configs to Google Cloud Storage, and start billable Vertex AI
custom jobs. These actions transmit project data and mutate cloud resources;
review the destination and obtain the required authorization before running
them.

## Current defaults

`submit_vertex_job.py` currently defines:

- Google Cloud project: `panama-485510`
- Region: `europe-west3`
- Bucket: `nam-training-data-eu`
- Container: `gcr.io/panama-485510/nam-trainer:latest`
- Machine: `n1-standard-8` with one T4 by default
- Training: 800 epochs, ESR threshold `0.009`

The `upload` and single-model `train` commands accept `--region` and
`--bucket`; the image project and the defaults used by `build` and
`train-all` remain hard-coded in the script. Update or review those constants
before using this helper for another account or environment. GPU choices
accepted by the current CLI are `T4`, `V100`, `P4`, `L4`, and `A100`. Check
current Vertex AI availability and pricing rather than relying on historical
estimates.

The caller needs Google Cloud Application Default Credentials with access to
the configured project and bucket. Building also needs Docker and registry
authentication capable of pushing the configured `gcr.io` image.

## Data preflight

The cloud entrypoint expects:

- `trainer_source/training_data/input.wav`
- a `parametric_data_<normalized-model-name>.json` config
- re-amped WAVs below
  `NeuralAmpModeler/resources/models/parametric-training-files/<MODEL_NAME>/`

The raw re-amped WAVs are not present in this checkout. Restore and verify them
before uploading. The five model names with matching checked-in data configs
are `DessBlock-green`, `DessBlock-red`, `SickDess`, `TS9`, and `aesahaettr`.
Although the script also lists `DessTortion-blue` and `DessTortion-red`, their
matching `parametric_data_desstortion_*.json` files are absent, so those jobs
need configs before submission. For the same reason, do not use a broad
`upload --all` or default `train-all` without first checking the complete input
set.

## Commands

Run commands from the repository root.

Build and push the configured container:

```bash
python3 trainer_source/cloud/submit_vertex_job.py build
```

Upload one restored dataset and the available configs:

```bash
python3 trainer_source/cloud/submit_vertex_job.py upload --model DessBlock-green
```

Once the remote input set is complete, submit one asynchronous job:

```bash
python3 trainer_source/cloud/submit_vertex_job.py train \
  --model DessBlock-green \
  --gpu T4
```

To submit several explicitly reviewed jobs in parallel:

```bash
python3 trainer_source/cloud/submit_vertex_job.py train-all \
  --models DessBlock-red SickDess TS9 aesahaettr
```

The helper prints the Vertex AI custom-job console URL after submission. With
the default bucket, training and exported results are uploaded below:

```text
gs://nam-training-data-eu/outputs/<MODEL_NAME>/
```

See [`../README.md`](../README.md) for local training, calibration, and export
details.

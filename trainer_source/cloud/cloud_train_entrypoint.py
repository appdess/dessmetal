#!/usr/bin/env python3
"""
Cloud training entrypoint for Vertex AI.
Downloads data from GCS, runs training, uploads results back to GCS.
"""

import os
import sys
import json
import argparse
import re
import subprocess
from pathlib import Path

# Suppress urllib3 warnings
import warnings
warnings.filterwarnings("ignore", category=UserWarning, module="urllib3")


_MODEL_NAME_RE = re.compile(r"[A-Za-z0-9][A-Za-z0-9_-]{0,127}\Z")


def validated_model_name(value: str) -> str:
    """Accept model names that are safe as local and remote path components."""
    if not _MODEL_NAME_RE.fullmatch(value):
        raise argparse.ArgumentTypeError(
            "model name must contain only letters, numbers, '_' or '-', "
            "start with a letter or number, and be at most 128 characters"
        )
    return value


def normalized_gcs_prefix(source_prefix: str) -> str:
    """Return a safe GCS prefix without its optional trailing slash."""
    prefix = source_prefix.rstrip('/')
    prefix_parts = prefix.split('/')
    if (
        not prefix
        or prefix.startswith('/')
        or '\\' in prefix
        or any(part in ('', '.', '..') for part in prefix_parts)
        or any(ord(character) < 32 or ord(character) == 127 for character in prefix)
    ):
        raise ValueError(f"Unsafe GCS source prefix: {source_prefix!r}")
    return prefix


def safe_gcs_download_path(blob_name: str, source_prefix: str, dest_dir: Path):
    """Map one object below an exact GCS prefix to a contained local path."""
    prefix = normalized_gcs_prefix(source_prefix)

    # GCS may contain objects ending in '/' that are used as directory markers.
    if blob_name == prefix or blob_name == f"{prefix}/":
        return None

    exact_prefix = f"{prefix}/"
    if not blob_name.startswith(exact_prefix):
        raise ValueError(
            f"GCS object {blob_name!r} is outside exact prefix {exact_prefix!r}"
        )

    relative_name = blob_name[len(exact_prefix):]
    is_directory_marker = relative_name.endswith('/')
    path_name = relative_name[:-1] if is_directory_marker else relative_name
    relative_parts = path_name.split('/')
    if (
        '\\' in path_name
        or any(part in ('', '.', '..') for part in relative_parts)
        or any(ord(character) < 32 or ord(character) == 127 for character in path_name)
    ):
        raise ValueError(f"Unsafe GCS object path: {blob_name!r}")
    if is_directory_marker:
        return None

    if dest_dir.is_symlink():
        raise ValueError(f"Download directory must not be a symlink: {dest_dir}")
    dest_dir.mkdir(parents=True, exist_ok=True)
    if dest_dir.is_symlink():
        raise ValueError(f"Download directory must not be a symlink: {dest_dir}")
    destination_root = dest_dir.resolve()
    local_path = destination_root.joinpath(*relative_parts)
    resolved_path = local_path.resolve(strict=False)

    try:
        contained = (
            os.path.commonpath((str(destination_root), str(resolved_path)))
            == str(destination_root)
        )
    except ValueError:
        contained = False
    if not contained or resolved_path != local_path:
        raise ValueError(f"GCS object resolves outside its download directory: {blob_name!r}")

    current_path = destination_root
    for index, component in enumerate(relative_parts):
        current_path /= component
        if current_path.is_symlink():
            raise ValueError(f"Download path contains a symlink: {blob_name!r}")
        if (
            index < len(relative_parts) - 1
            and current_path.exists()
            and not current_path.is_dir()
        ):
            raise ValueError(f"Download path has a file as an ancestor: {blob_name!r}")
    if local_path.exists() and local_path.is_dir():
        raise ValueError(f"Download target is an existing directory: {blob_name!r}")

    return local_path


def plan_gcs_downloads(blobs, source_prefix: str, dest_dir: Path):
    """Validate a complete GCS listing before any object is downloaded."""
    plan = []
    planned_paths = set()
    for blob in blobs:
        local_path = safe_gcs_download_path(blob.name, source_prefix, dest_dir)
        if local_path is None:
            continue
        if local_path in planned_paths:
            raise ValueError(f"Multiple GCS objects map to the same path: {blob.name!r}")
        if any(
            local_path in existing_path.parents or existing_path in local_path.parents
            for existing_path in planned_paths
        ):
            raise ValueError(f"GCS objects have a file/directory collision: {blob.name!r}")
        planned_paths.add(local_path)
        plan.append((blob, local_path))
    return plan


def download_from_gcs(
    bucket_name: str,
    source_prefix: str,
    dest_dir: Path,
    storage_client=None,
):
    """Download files from GCS to local directory."""
    if storage_client is None:
        from google.cloud import storage

        storage_client = storage.Client()
    bucket = storage_client.bucket(bucket_name)

    exact_prefix = f"{normalized_gcs_prefix(source_prefix)}/"
    plan = plan_gcs_downloads(
        list(bucket.list_blobs(prefix=exact_prefix)),
        source_prefix,
        dest_dir,
    )

    # Create and recheck every destination before the first remote write. A
    # failed listing therefore cannot leave a partially refreshed dataset.
    for blob, local_path in plan:
        local_path.parent.mkdir(parents=True, exist_ok=True)
        if safe_gcs_download_path(blob.name, source_prefix, dest_dir) != local_path:
            raise ValueError(f"Download target changed during preflight: {blob.name!r}")

    for blob, local_path in plan:
        print(f"Downloading: {blob.name} -> {local_path}")
        blob.download_to_filename(str(local_path))


def upload_to_gcs(
    bucket_name: str,
    source_dir: Path,
    dest_prefix: str,
    storage_client=None,
):
    """Upload directory contents to GCS."""
    if storage_client is None:
        from google.cloud import storage

        storage_client = storage.Client()
    bucket = storage_client.bucket(bucket_name)
    
    for local_path in source_dir.rglob('*'):
        if local_path.is_symlink():
            raise ValueError(f"Refusing to upload a symlink: {local_path}")
        if local_path.is_file():
            rel_path = local_path.relative_to(source_dir)
            blob_name = f"{dest_prefix}/{rel_path}"
            
            print(f"Uploading: {local_path} -> gs://{bucket_name}/{blob_name}")
            blob = bucket.blob(blob_name)
            blob.upload_from_filename(str(local_path))


def main():
    parser = argparse.ArgumentParser(description="Cloud training entrypoint")
    parser.add_argument("--bucket", required=True, help="GCS bucket name")
    parser.add_argument(
        "--model-name",
        required=True,
        type=validated_model_name,
        help="Model name (e.g., DessBlock-green)",
    )
    parser.add_argument("--epochs", type=int, default=800, help="Max epochs")
    parser.add_argument("--threshold-esr", type=float, default=0.009, help="Early stopping ESR threshold")
    parser.add_argument("--input-level", type=float, required=True, help="Input dBu level")
    parser.add_argument("--output-level", type=float, required=True, help="Output dBu level")
    args = parser.parse_args()
    
    # Setup directories
    work_dir = Path("/app/work")
    data_dir = work_dir / "data"
    output_dir = work_dir / "output"
    work_dir.mkdir(parents=True, exist_ok=True)
    data_dir.mkdir(parents=True, exist_ok=True)
    output_dir.mkdir(parents=True, exist_ok=True)
    
    print(f"=== NAM Cloud Training: {args.model_name} ===")
    print(f"Bucket: {args.bucket}")
    print(f"Epochs: {args.epochs}, ESR threshold: {args.threshold_esr}")
    
    # Download training data
    print("\n--- Downloading training data ---")
    download_from_gcs(
        args.bucket,
        f"training-data/{args.model_name}",
        data_dir / args.model_name
    )
    
    # Download input.wav (shared across all models)
    print("\n--- Downloading input.wav ---")
    from google.cloud import storage

    client = storage.Client()
    bucket = client.bucket(args.bucket)
    blob = bucket.blob("training-data/input.wav")
    local_input_wav = data_dir / "input.wav"
    print(f"Downloading: {blob.name} -> {local_input_wav}")
    blob.download_to_filename(str(local_input_wav))
    
    # Download config files
    download_from_gcs(
        args.bucket,
        "configs",
        work_dir / "configs"
    )
    
    # Update data config with local paths
    data_config_path = work_dir / "configs" / f"parametric_data_{args.model_name.lower().replace('-', '_')}.json"
    if data_config_path.exists():
        with open(data_config_path, 'r') as f:
            config = json.load(f)
        
        # Update paths to local
        config['common']['x_path'] = str(data_dir / "input.wav")
        for split in ['train', 'validation']:
            if split in config:
                for output in config[split]['outputs']:
                    # Extract filename from original path
                    orig_path = output['y_path']
                    filename = Path(orig_path).name
                    output['y_path'] = str(data_dir / args.model_name / filename)
        
        with open(data_config_path, 'w') as f:
            json.dump(config, f, indent=2)
        print(f"Updated config: {data_config_path}")
    
    # Run training
    print("\n--- Starting training ---")
    train_cmd = [
        "python", "-u", "/app/trainer_source/custom_train_full.py",
        "--base-outdir", str(output_dir / args.model_name),
        "--data-config", str(data_config_path),
        "--model-config", str(work_dir / "configs" / "parametric_wavenet.json"),
        "--learning-config", str(work_dir / "configs" / "default.json"),
        "--epochs", str(args.epochs),
        "--threshold-esr", str(args.threshold_esr),
        "--check-val-every-n-epoch", "5"
    ]
    
    print(f"Command: {' '.join(train_cmd)}")
    result = subprocess.run(train_cmd, cwd="/app")
    
    if result.returncode != 0:
        print(f"Training failed with return code {result.returncode}")
        sys.exit(1)
    
    # Export model
    print("\n--- Exporting model ---")
    # Find the training output directory
    training_dirs = list((output_dir / args.model_name).glob("20*"))
    if training_dirs:
        latest_training = sorted(training_dirs)[-1]
        
        export_cmd = [
            "python", "-u", "/app/trainer_source/export_model.py",
            "--input-level", str(args.input_level),
            "--output-level", str(args.output_level),
            str(latest_training),
            str(output_dir / "exported" / args.model_name)
        ]
        
        print(f"Export command: {' '.join(export_cmd)}")
        subprocess.run(export_cmd, cwd="/app")
    
    # Upload results to GCS
    print("\n--- Uploading results ---")
    upload_to_gcs(
        args.bucket,
        output_dir,
        f"outputs/{args.model_name}"
    )
    
    print("\n=== Training complete! ===")


if __name__ == "__main__":
    main()

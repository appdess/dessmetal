#!/usr/bin/env python3
"""
Cloud training entrypoint for Vertex AI.
Downloads data from GCS, runs training, uploads results back to GCS.
"""

import os
import sys
import json
import argparse
import subprocess
from pathlib import Path
from google.cloud import storage

# Suppress urllib3 warnings
import warnings
warnings.filterwarnings("ignore", category=UserWarning, module="urllib3")


def download_from_gcs(bucket_name: str, source_prefix: str, dest_dir: Path):
    """Download files from GCS to local directory."""
    client = storage.Client()
    bucket = client.bucket(bucket_name)
    
    blobs = bucket.list_blobs(prefix=source_prefix)
    for blob in blobs:
        # Get relative path
        rel_path = blob.name[len(source_prefix):].lstrip('/')
        if not rel_path:
            continue
        
        local_path = dest_dir / rel_path
        local_path.parent.mkdir(parents=True, exist_ok=True)
        
        print(f"Downloading: {blob.name} -> {local_path}")
        blob.download_to_filename(str(local_path))


def upload_to_gcs(bucket_name: str, source_dir: Path, dest_prefix: str):
    """Upload directory contents to GCS."""
    client = storage.Client()
    bucket = client.bucket(bucket_name)
    
    for local_path in source_dir.rglob('*'):
        if local_path.is_file():
            rel_path = local_path.relative_to(source_dir)
            blob_name = f"{dest_prefix}/{rel_path}"
            
            print(f"Uploading: {local_path} -> gs://{bucket_name}/{blob_name}")
            blob = bucket.blob(blob_name)
            blob.upload_from_filename(str(local_path))


def main():
    parser = argparse.ArgumentParser(description="Cloud training entrypoint")
    parser.add_argument("--bucket", required=True, help="GCS bucket name")
    parser.add_argument("--model-name", required=True, help="Model name (e.g., DessBlock-green)")
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
    # Download input.wav (shared across all models)
    print("\n--- Downloading input.wav ---")
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

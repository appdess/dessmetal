#!/usr/bin/env python3
"""
Submit NAM training jobs to Vertex AI.
Usage: python submit_vertex_job.py --model DessBlock-green --epochs 800
"""

import argparse
import os
import subprocess
import sys
from datetime import datetime
from google.cloud import aiplatform

# Configuration
# Configuration
PROJECT_ID = "panama-485510"
REGION = "europe-west3"  # Frankfurt
BUCKET_NAME = "nam-training-data-eu"  # EU bucket
CONTAINER_IMAGE = f"gcr.io/{PROJECT_ID}/nam-trainer:latest"

# Model configurations with their dBu levels
MODEL_CONFIGS = {
    # Amp models
    "DessBlock-green": {"input_level": 22.9, "output_level": 16.0, "type": "amp"},
    "DessBlock-red": {"input_level": 22.9, "output_level": 16.0, "type": "amp"},
    "SickDess": {"input_level": 22.9, "output_level": 16.0, "type": "amp"},
    "DessTortion-blue": {"input_level": 22.9, "output_level": 16.0, "type": "amp"},
    "DessTortion-red": {"input_level": 22.9, "output_level": 16.0, "type": "amp"},
    # Boost pedals
    "TS9": {"input_level": 1.0, "output_level": 10.0, "type": "boost"},
    "aesahaettr": {"input_level": 16.0, "output_level": 16.0, "type": "boost"},
}


def create_bucket_if_not_exists(bucket_name: str, region: str):
    """Create GCS bucket if it doesn't exist."""
    from google.cloud import storage
    client = storage.Client(project=PROJECT_ID)
    
    try:
        bucket = client.get_bucket(bucket_name)
        print(f"Bucket {bucket_name} already exists")
    except Exception:
        print(f"Creating bucket {bucket_name} in {region}...")
        bucket = client.create_bucket(bucket_name, location=region)
        print(f"Created bucket: {bucket_name}")
    
    return bucket


def upload_training_data(bucket_name: str, model_name: str, local_data_dir: str):
    """Upload training data to GCS."""
    from google.cloud import storage
    client = storage.Client(project=PROJECT_ID)
    bucket = client.bucket(bucket_name)
    
    # Upload model-specific training files
    model_data_dir = os.path.join(local_data_dir, model_name)
    if os.path.exists(model_data_dir):
        for filename in os.listdir(model_data_dir):
            local_path = os.path.join(model_data_dir, filename)
            if os.path.isfile(local_path):
                blob_name = f"training-data/{model_name}/{filename}"
                print(f"Uploading: {local_path} -> gs://{bucket_name}/{blob_name}")
                blob = bucket.blob(blob_name)
                blob.upload_from_filename(local_path)
    
    # Upload shared input.wav
    input_wav = os.path.join(local_data_dir, "input.wav")
    if os.path.exists(input_wav):
        blob = bucket.blob("training-data/input.wav")
        if not blob.exists():
            print(f"Uploading: {input_wav}")
            blob.upload_from_filename(input_wav)


def upload_configs(bucket_name: str, local_config_dir: str):
    """Upload config files to GCS."""
    from google.cloud import storage
    client = storage.Client(project=PROJECT_ID)
    bucket = client.bucket(bucket_name)
    
    config_files = [
        "parametric_wavenet.json",
        "default_config_files/learning/default.json",
    ]
    
    # Also upload all parametric_data_*.json files
    for filename in os.listdir(local_config_dir):
        if filename.startswith("parametric_data_") and filename.endswith(".json"):
            config_files.append(filename)
    
    for config_file in config_files:
        local_path = os.path.join(local_config_dir, config_file)
        if os.path.exists(local_path):
            # Flatten path for GCS
            blob_name = f"configs/{os.path.basename(config_file)}"
            print(f"Uploading config: {local_path} -> gs://{bucket_name}/{blob_name}")
            blob = bucket.blob(blob_name)
            blob.upload_from_filename(local_path)


def build_and_push_container():
    """Build and push the training container to GCR."""
    cloud_dir = os.path.dirname(os.path.abspath(__file__))
    trainer_dir = os.path.dirname(cloud_dir)
    
    print("Building container image...")
    
    # Build the image
    build_cmd = [
        "docker", "build",
        "--platform", "linux/amd64",
        "-t", CONTAINER_IMAGE,
        "-f", os.path.join(cloud_dir, "Dockerfile"),
        trainer_dir
    ]
    subprocess.run(build_cmd, check=True)
    
    # Push to GCR
    print("Pushing to Google Container Registry...")
    subprocess.run(["docker", "push", CONTAINER_IMAGE], check=True)
    
    print(f"Container pushed: {CONTAINER_IMAGE}")


def submit_training_job(
    model_name: str,
    epochs: int = 800,
    threshold_esr: float = 0.009,
    machine_type: str = "n1-standard-8",
    accelerator_type: str = "NVIDIA_TESLA_T4",
    accelerator_count: int = 1,
):
    """Submit a Vertex AI custom training job."""
    
    if model_name not in MODEL_CONFIGS:
        raise ValueError(f"Unknown model: {model_name}. Available: {list(MODEL_CONFIGS.keys())}")
    
    config = MODEL_CONFIGS[model_name]
    
    # Initialize Vertex AI
    aiplatform.init(
        project=PROJECT_ID, 
        location=REGION,
        staging_bucket=f"gs://{BUCKET_NAME}"
    )
    
    # Create job name
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    job_name = f"nam-{model_name.lower().replace('-', '_')}_{timestamp}"
    
    print(f"\n=== Submitting Vertex AI Training Job ===")
    print(f"Job name: {job_name}")
    print(f"Model: {model_name}")
    print(f"Machine: {machine_type} + {accelerator_count}x {accelerator_type}")
    print(f"Epochs: {epochs}, ESR threshold: {threshold_esr}")
    
    # Container arguments
    container_args = [
        "--bucket", BUCKET_NAME,
        "--model-name", model_name,
        "--epochs", str(epochs),
        "--threshold-esr", str(threshold_esr),
        "--input-level", str(config["input_level"]),
        "--output-level", str(config["output_level"]),
    ]
    
    # Create custom training job
    job = aiplatform.CustomContainerTrainingJob(
        display_name=job_name,
        container_uri=CONTAINER_IMAGE,
        location=REGION,
    )
    
    # Run the job
    job.run(
        args=container_args,
        machine_type=machine_type,
        accelerator_type=accelerator_type,
        accelerator_count=accelerator_count,
        replica_count=1,
        sync=False,  # Don't wait for completion
    )
    
    print(f"\nJob submitted to {REGION}!")
    print(f"Monitor at: https://console.cloud.google.com/vertex-ai/training/custom-jobs?project={PROJECT_ID}")
    return job


def main():
    parser = argparse.ArgumentParser(description="Submit NAM training to Vertex AI")
    
    subparsers = parser.add_subparsers(dest="command", help="Commands")
    
    # Build command
    build_parser = subparsers.add_parser("build", help="Build and push container")
    
    # Upload command
    upload_parser = subparsers.add_parser("upload", help="Upload training data to GCS")
    upload_parser.add_argument("--model", help="Model name to upload data for")
    upload_parser.add_argument("--all", action="store_true", help="Upload all model data")
    upload_parser.add_argument("--region", help="GCP Region (overrides default)")
    upload_parser.add_argument("--bucket", help="GCS Bucket (overrides default)")
    
    # Train command
    train_parser = subparsers.add_parser("train", help="Submit training job")
    train_parser.add_argument("--model", required=True, help="Model name to train")
    train_parser.add_argument("--epochs", type=int, default=800, help="Max epochs")
    train_parser.add_argument("--threshold-esr", type=float, default=0.009, help="ESR threshold")
    train_parser.add_argument("--gpu", default="T4", choices=["T4", "V100", "P4", "L4", "A100"], help="GPU type")
    train_parser.add_argument("--machine-type", default="n1-standard-8", help="Machine type (e.g., n1-standard-8, g2-standard-4)")
    train_parser.add_argument("--region", help="GCP Region (overrides default)")
    train_parser.add_argument("--bucket", help="GCS Bucket (overrides default)")
    
    # Train all command
    train_all_parser = subparsers.add_parser("train-all", help="Submit all pending training jobs")
    train_all_parser.add_argument("--models", nargs="+", help="Specific models to train")
    
    args = parser.parse_args()
    
    # Update globals if provided
    global REGION, BUCKET_NAME
    if hasattr(args, 'region') and args.region:
        REGION = args.region
    if hasattr(args, 'bucket') and args.bucket:
        BUCKET_NAME = args.bucket
    
    if args.command == "build":
        build_and_push_container()
        
    elif args.command == "upload":
        # Get local paths
        script_dir = os.path.dirname(os.path.abspath(__file__))
        trainer_dir = os.path.dirname(script_dir)
        project_dir = os.path.dirname(trainer_dir)
        
        data_dir = os.path.join(project_dir, "NeuralAmpModeler/resources/models/parametric-training-files")
        input_wav_dir = os.path.join(trainer_dir, "training_data")
        
        # Create bucket
        create_bucket_if_not_exists(BUCKET_NAME, REGION)
        
        # Upload configs
        upload_configs(BUCKET_NAME, trainer_dir)
        
        if args.all:
            for model_name in MODEL_CONFIGS.keys():
                upload_training_data(BUCKET_NAME, model_name, data_dir)
        elif args.model:
            upload_training_data(BUCKET_NAME, args.model, data_dir)
        
        # Always upload input.wav
        from google.cloud import storage
        client = storage.Client(project=PROJECT_ID)
        bucket = client.bucket(BUCKET_NAME)
        input_wav = os.path.join(input_wav_dir, "input.wav")
        if os.path.exists(input_wav):
            blob = bucket.blob("training-data/input.wav")
            print(f"Uploading: {input_wav}")
            blob.upload_from_filename(input_wav)
            
    elif args.command == "train":
        gpu_map = {
            "T4": "NVIDIA_TESLA_T4",
            "V100": "NVIDIA_TESLA_V100",
            "P4": "NVIDIA_TESLA_P4",
            "L4": "NVIDIA_L4",
            "A100": "NVIDIA_TESLA_A100",
        }
        submit_training_job(
            model_name=args.model,
            epochs=args.epochs,
            threshold_esr=args.threshold_esr,
            accelerator_type=gpu_map[args.gpu],
            machine_type=args.machine_type,
        )
        
    elif args.command == "train-all":
        models = args.models or list(MODEL_CONFIGS.keys())
        print(f"Submitting {len(models)} training jobs...")
        for model_name in models:
            submit_training_job(model_name)
            
    else:
        parser.print_help()


if __name__ == "__main__":
    main()

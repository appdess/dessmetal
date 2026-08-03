#!/usr/bin/env python3
"""Download a NAM training checkpoint and print a compact tensor summary."""

import argparse
import subprocess
import tempfile
from pathlib import Path

import torch


def summarize_remote_checkpoint(checkpoint_url: str) -> None:
    with tempfile.TemporaryDirectory(prefix="dessmetal-checkpoint-") as temp_dir:
        checkpoint_path = Path(temp_dir) / "remote.ckpt"
        subprocess.run(
            f"curl --fail --silent --show-error --location {checkpoint_url} "
            f"--output {checkpoint_path}",
            shell=True,
            check=True,
        )

        checkpoint = torch.load(
            checkpoint_path,
            map_location="cpu",
            weights_only=False,
        )
        state_dict = checkpoint.get("state_dict", {})
        tensor_count = sum(torch.is_tensor(value) for value in state_dict.values())
        print(f"Checkpoint contains {tensor_count} tensors")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("checkpoint_url", help="Remote checkpoint URL")
    args = parser.parse_args()
    summarize_remote_checkpoint(args.checkpoint_url)


if __name__ == "__main__":
    main()

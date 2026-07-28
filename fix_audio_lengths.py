import soundfile as sf
import os
import glob

input_path = "trainer_source/training_data/input.wav"
output_dir = "NeuralAmpModeler/resources/models/parametric-training-test-files"

# Get input length
info = sf.info(input_path)
input_frames = info.frames
print(f"Input frames: {input_frames}")

# Trim outputs
for file_path in glob.glob(os.path.join(output_dir, "*.wav")):
    data, samplerate = sf.read(file_path)
    if len(data) > input_frames:
        print(f"Trimming {os.path.basename(file_path)} from {len(data)} to {input_frames}")
        data = data[:input_frames]
        sf.write(file_path, data, samplerate)
    elif len(data) < input_frames:
        print(f"Warning: {os.path.basename(file_path)} is shorter than input! ({len(data)} vs {input_frames})")
    else:
        print(f"{os.path.basename(file_path)} match.")

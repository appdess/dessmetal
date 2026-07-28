import os
import numpy as np
import soundfile as sf
import pathlib

def calculate_rms(data):
    """Calculates RMS of the given data."""
    # Ensure data is floating point
    return np.sqrt(np.mean(data**2))

def db_to_linear(db):
    return 10 ** (db / 20.0)

def linear_to_db(linear):
    if linear == 0:
        return -np.inf
    return 20 * np.log10(linear)

def process_irs(input_dir, output_dir, target_rms_db=-27.0, ceiling_db=-0.1, early_ms=50):
    """
    Normalizes IRs in input_dir to a consistent Output RMS (Loudness).
    1. Boosts signal to target RMS.
    2. Soft Clips (Tanh) to ensure safety.
    3. Checks Final RMS: if > Target, attenuates down to match Target.
    """
    input_path = pathlib.Path(input_dir)
    output_path = pathlib.Path(output_dir)
    output_path.mkdir(parents=True, exist_ok=True)

    print(f"Processing IRs from: {input_path}")
    print(f"Saving to: {output_path}")
    print(f"Target Output RMS: {target_rms_db} dB")
    print(f"Safety Ceiling: {ceiling_db} dB (Conditional Soft Clip)")
    print("-" * 40)

    files = list(input_path.glob("*.wav"))
    if not files:
        print("No .wav files found in input directory.")
        return

    for file_path in files:
        try:
            data, samplerate = sf.read(file_path)
            
            # Determine length of 50ms in samples
            early_samples = int((early_ms / 1000.0) * samplerate)
            
            # Handle short files
            if len(data) < early_samples:
                early_slice = data
            else:
                early_slice = data[:early_samples]

            # 1. Calculate RMS of Early Slice
            current_rms = calculate_rms(early_slice)
            
            if current_rms == 0:
                print(f"WARNING: '{file_path.name}' has 0 RMS in the first {early_ms}ms. Skipping.")
                continue

            current_rms_db = linear_to_db(current_rms)
            
            # 2. Calculate Gain to hit Target RMS (Initial Boost)
            # We aim slightly higher to ensure modulation, but let's stick to target
            gain_db = target_rms_db - current_rms_db
            gain_linear = db_to_linear(gain_db)

            # 3. Apply Initial Gain
            boosted_signal = data * gain_linear

            # 4. Conditional Soft Clipping
            current_peak = np.max(np.abs(boosted_signal))
            current_peak_db = linear_to_db(current_peak)
            
            final_signal = boosted_signal
            clipped = False
            
            if current_peak_db > ceiling_db:
                # Soft Clip
                limit_linear = db_to_linear(ceiling_db)
                saturated = np.tanh(boosted_signal)
                
                # Check peak of saturated signal and normalize if necessary
                sat_peak = np.max(np.abs(saturated))
                if sat_peak > limit_linear:
                     final_signal = saturated * (limit_linear / sat_peak)
                else:
                     final_signal = saturated
                clipped = True
            
            # 5. POST-CLIP CHECK: Smart Attenuation
            # We measure the RMS *after* clipping.
            # If it is still louder than target (Sheffield case), we turn it down.
            # If it is quieter (Beasty case), we leave it (since boosting would clip again).
            
            if len(final_signal) < early_samples:
                final_early = final_signal
            else:
                final_early = final_signal[:early_samples]
            
            post_clip_rms = calculate_rms(final_early)
            post_clip_rms_db = linear_to_db(post_clip_rms)
            
            final_adjustment_db = 0.0
            
            if post_clip_rms_db > target_rms_db:
                # Too loud! Attenuate to match target.
                diff_db = target_rms_db - post_clip_rms_db # Negative value
                attenuation = db_to_linear(diff_db)
                final_signal = final_signal * attenuation
                final_adjustment_db = diff_db
                
            # Final stats
            if len(final_signal) < early_samples:
                final_early_check = final_signal
            else:
                final_early_check = final_signal[:early_samples]
            final_rms_db = linear_to_db(calculate_rms(final_early_check))
            final_peak_db = linear_to_db(np.max(np.abs(final_signal)))

            # 6. Save as 32-bit Float
            output_file = output_path / file_path.name
            sf.write(output_file, final_signal, samplerate, subtype='FLOAT')
            
            action = "CLIPPED" if clipped else "CLEAN"
            if final_adjustment_db < 0:
                action += "+ATTEN"
            
            print(f"[{action}] {file_path.name}: In {current_rms_db:.1f}dB -> ClipRMS {post_clip_rms_db:.1f}dB -> Final {final_rms_db:.1f}dB / Peak {final_peak_db:.2f}dB")

        except Exception as e:
            print(f"ERROR processing {file_path.name}: {e}")

    print("-" * 40)
    print("Batch processing complete.")

if __name__ == "__main__":
    INPUT_DIR = "NeuralAmpModeler/resources/models/IRs"
    OUTPUT_DIR = os.path.join(INPUT_DIR, "normalized")
    
    # Target -27dB RMS for proper loudness matching across all IRs
    process_irs(INPUT_DIR, OUTPUT_DIR, target_rms_db=-27.0)

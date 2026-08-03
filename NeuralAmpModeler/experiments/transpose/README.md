# DessMetal transpose-engine evaluation

Status: **production implementation selected and integrated**

Evaluation date: 2026-08-03

Branch: `codex/transpose-engine-spike`

Base commit: `176ed27a77a8073ff4cde1c55bef4ad6ad5332a5`

## Outcome

DessMetal now has a self-contained, real-time polyphonic transpose processor
covering every integer semitone from `-12` through `+12`. It is implemented in
[`TransposeProcessor.h`](../../TransposeProcessor.h), has no runtime dependency
on a proprietary SDK, allocates no memory in `Process()`, adds no buffered
look-ahead, and returns the exact dry samples after the transition back to zero.

The selected design is an ERB-spaced analytic filter bank derived from Steven
Schulteis' MIT-licensed Terrarium Poly Pitch work. DessMetal adds a second,
wider-band transient path, onset-controlled blending, phase-continuous pitch
automation, finite-value recovery, and an 8 ms wet/dry transition. The original
license is preserved in `licenses/terrarium-poly-octave-MIT.txt` and the
attribution is listed in `THIRD_PARTY_NOTICES.md`.

This is not a copy of Neural DSP code. Neural DSP was used only as a black-box
product reference for range, interaction, and rendered A/B evaluation.

## Why this engine

The first spike tested common permissive and platform alternatives. They can
produce good offline or high-latency pitch shifting, but their live modes did
not meet this project's combination of octave range, polyphonic guitar input,
and low latency. The filter-bank approach was the strongest open foundation and
was small enough to harden directly inside DessMetal.

| Candidate | Result in this evaluation | Decision |
|---|---|---|
| DessMetal hybrid ERB processor | Zero buffered latency; exact target carrier in the production sine gates; allocation-free and block-invariant render path; onset gate below 10 ms | **Selected** |
| Signalsmith Stretch 1.3.2 (MIT) | Stable and allocation-free at a 1024-sample setup, but low-string octave tests produced dominant sidebands or mistuning | Rejected; not retained in the production tree |
| Bungee Basic 2.4.24 (MPL-2.0) | Supported configurations were roughly 93–235 ms in the spike; unsupported low-granularity settings reduced latency but degraded quality | Rejected |
| Apple NewTimePitch / AUPitch | Correct pitch but measured roughly 85 ms / 128 ms and would be macOS-only | Rejected |
| Rubber Band LiveShifter / SoundTouch | Published live/typical delays are outside the target for direct guitar monitoring | Rejected |

Primary open references:

- [Terrarium Poly Pitch arbitrary-shift branch](https://github.com/schult/terrarium-poly-octave/tree/arbitrary-shift)
- [Low-Latency Polyphonic Pitch-Shifting of Guitar Signals (Aalto thesis)](https://aaltodoc.aalto.fi/items/6fd2cfc9-db61-4b0a-b11f-ea8c2925509f)
- [Latenzoptimierte Tonhoehenverschiebung (HAW Hamburg thesis)](https://reposit.haw-hamburg.de/bitstream/20.500.12738/15713/1/MA_Latenzoptimierte%20Tonh%C3%B6henverschiebung.pdf)
- [IDMT-SMT-Guitar direct-input dataset](https://zenodo.org/records/4988354)

## Production signal path

The processor runs after input calibration, sanitization, and mono collapse,
before the drive, gate detector, NAM amp, tone stack, and cabinet IR. This lets
the virtual amp react to the retuned guitar rather than shifting the already
distorted output.

`Transpose` is parameter ID 20, appended after the historical 20 parameters so
all earlier IDs remain unchanged. Version `0.2.x` state contains 21 parameters;
all accepted `0.1.x` and legacy layouts migrate with `Transpose = 0`. The fifth
historical hidden amp state remains untouched for host compatibility.

The UI exposes a large `minus | value | plus` control. It uses integer steps,
shows signed values, and resets to zero on double-click. Host automation glides
the pitch ratio and wet mix over 8 ms to avoid abrupt phase or sample jumps.

## Reproducible gates

Run the production processor tests:

```sh
bash NeuralAmpModeler/tests/run_transpose_processor_tests.sh
```

The suite covers:

- bit-exact zero bypass and return-to-zero settling;
- target-carrier accuracy at every integer shift at 48 kHz;
- finite, non-silent, bounded output at every shift at 44.1, 48, 88.2, and 96 kHz;
- standard guitar open strings at both octave extremes;
- fixed and irregular host block equivalence;
- sub-10 ms half-level onset gates at the four frequency/shift extremes;
- bounded automation discontinuity, silence, and NaN/Inf recovery; and
- zero tracked allocations across repeated render blocks and automation.

Run state-layout compatibility tests:

```sh
bash NeuralAmpModeler/tests/run_unserialization_layout_tests.sh
```

Run the retained ERB research benchmark:

```sh
bash NeuralAmpModeler/experiments/transpose/run_erb_benchmark.sh
```

The baseline ERB benchmark on the development M1 Max reported 83 bands, zero
buffered samples, about 5.6% of one real-time core, exact single-sine octave
carriers, and chord target SNR from about 9.0 to 15.4 dB. Those screening
numbers are useful for regression, but they are not a listening-quality claim.
The production processor uses two such banks and adds transient blending, so
the plug-in and host tests are the authoritative gates.

## Remaining human gate

Automated tests can establish pitch, stability, latency behavior, state
compatibility, and finite/non-silent output. They cannot certify feel or tone.
Before merge, audition the exact staged AU in the disposable Logic demo project
on clean DI single notes, palm-muted chugs, power/open chords, bends, slides,
pick transients, and decays. Test every integer shift, with special attention to
`-12`, `-7`, `-5`, `+5`, `+7`, and `+12`, and compare at matched loudness.

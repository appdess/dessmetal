# Contributing to DessMetal

Coordinate with the project owner before starting a large feature. Current collaborators can use the private project
tracker; a public contribution tracker will be linked when the repository is published. Keep changes focused and do
not add model, IR, font, or artwork files without documenting their origin, copyright owner, and redistribution
license.

Before proposing a change, run:

```bash
cd NeuralAmpModelerCore
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target run_tests -j 8
./build/tools/run_tests

cd ..
bash AudioDSPTools/tests/run_wav_tests.sh
```

For macOS plug-in changes, also build APP/AU/VST3, run `auval -v aufx 1YEo AdMs`, and state which real hosts were
tested. Logic Pro validates AU only; use a VST3 validator or VST3 host for that format. Do not report audio quality as
verified from numerical render tests alone.

C++ follows LLVM-style formatting via `bash format.bash`. Preserve unrelated working-tree changes, avoid committing
generated build outputs, and never place signing credentials or notary secrets in the repository.

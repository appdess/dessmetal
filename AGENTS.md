# DessMetal repository guidance

DessMetal is a macOS guitar amp simulator built on Neural Amp Modeler and iPlug2. The current deliverables are a
standalone app, an Audio Unit v2, and a VST3 plug-in.

Use [README.md](README.md) for the evergreen build and test commands. Use [RELEASE.md](RELEASE.md) for dated
validation evidence, remaining owner gates, and the release procedure. Keep changing release results out of this
file.

## Worktree safety

- Work in the existing checkout and preserve unrelated or user-authored changes. Never reset, clean, or broadly
  delete a dirty worktree.
- Do not stage, commit, tag, push, upload, publish, notarize, accept legal terms, or change credentials unless the
  user explicitly authorizes that exact action.
- Treat model, IR, image, font, and icon provenance separately from the root source-code license. Do not infer asset
  redistribution rights or change embedded asset metadata without an explicit owner declaration.

## Build and package

- `./package_mac.sh --unsigned` is the credential-free release-pipeline gate. It builds and verifies universal
  APP/AU/VST3 bundles, component packages, the distribution package, DMG, checksums, manifests, and dSYMs.
- `./package_mac.sh` requires the configured Developer ID Application and Installer identities. It signs locally but
  does not notarize, upload, publish, or install anything.
- Fresh bundles are staged under `NeuralAmpModeler/build-mac/release/products/`. Only a completely successful run may
  promote its six-file artifact set to `NeuralAmpModeler/build-mac/out/`.
- Never share artifacts whose names contain `UNSIGNED-VALIDATION-ONLY`.

## Validation and freshness

- Validate the exact current-source universal APP, AU, and VST3, not an older installed copy or a previous package.
- Run the source/core, WAV parser, and state-layout suites before packaging. Run strict `auval`, AU stress/offline
  rendering, and the official VST3 validator against the freshly staged binaries when release freshness changes.
- A view-only AU UI host proves layout and parameter interaction only. It does not prove standalone CoreAudio startup,
  real-time playback, Logic Pro discovery, save/reopen persistence, or audible behavior.
- For Logic validation, first make a recoverable backup of any installed component, install the exact staged AU into
  the user Audio Units directory, use a disposable project copy, and record objective finite/non-silent active versus
  bypass evidence. Automated tests cannot judge tone or feel.
- Preserve the five-state host amp enumeration and existing parameter identifiers/order. Four amp models are visible;
  the historical hidden alias remains for saved-session compatibility.

## Training documentation

Parametric capture/training guidance belongs in [trainer_source/README.md](trainer_source/README.md), with cloud
execution details in [trainer_source/cloud/README.md](trainer_source/cloud/README.md). Verify capture paths,
calibration levels, and model hashes before replacing any shipped model.

## External approval boundaries

Stop and request confirmation at the moment Keychain authorization, credentials, notarization, upload, tag creation,
GitHub/GitLab publication, or legal acceptance is required. A successful signed local build is not a notarized or
public release.

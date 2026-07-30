# DessMetal repository guidance

DessMetal is a macOS guitar amp simulator built on Neural Amp Modeler and iPlug2. The current deliverables are a
standalone app, an Audio Unit v2, and a VST3 plug-in.

Use [README.md](README.md) for the evergreen build and test commands. Use [RELEASE.md](RELEASE.md) for dated
validation evidence, remaining owner gates, and the release procedure. Keep changing release results out of this
file. Keep the README welcoming, personal, and product-focused; put exact hashes, run IDs, proof tables,
release-status claims, and AI/process narration in RELEASE.md or generated release manifests instead.

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
  does not notarize, upload, publish, or install anything. Run a release build only from the clean, exact commit that
  will be reviewed and tagged.
- Fresh bundles are staged under `NeuralAmpModeler/build-mac/release/products/`. Only a completely successful run may
  promote its six-file artifact set to `NeuralAmpModeler/build-mac/out/`.
- Never share artifacts whose names contain `UNSIGNED-VALIDATION-ONLY`.

## Validation and freshness

- Validate the exact current-source universal APP, AU, and VST3, not an older installed copy or a previous package.
- Run the source/core, WAV parser, and state-layout suites before packaging. Run strict `auval`, AU stress/offline
  rendering, and the official VST3 validator against the freshly staged binaries when release freshness changes.
- Before accepting a signed installer, expand all three component packages and require their exact fixed destinations,
  `@relocatable=false`, zero `relocate/bundle` entries, and exact strict bundle identifiers, then install adversarially
  with a same-bundle-ID app copy elsewhere and confirm the app lands in `/Applications` with no relocation in
  `/var/log/install.log`.
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

## Notarization and GitHub release

- Never describe an older candidate as belonging to a newer documentation or source commit. Release artifacts,
  source-archive hashes, validation runs, tags, and manifests must all resolve to the same exact commit.
- After the owner approves the signed DMG SHA-256, run
  `DESSMETAL_NOTARY_PROFILE=<existing-profile> ./notarize_mac.sh <approved-dmg-sha256>`. It submits only that existing
  signed six-file set to Apple, staples the result, and re-verifies it. This command contacts Apple; do not run it on
  inferred approval or create credentials for it.
- [`.github/workflows/macos-ci.yml`](.github/workflows/macos-ci.yml) is the credential-free push, pull-request, and
  manual validation gate. For release signing, require a successful push or manual run on `main` for the exact
  release commit; a pull-request run alone does not satisfy the release gate.
- [`.github/workflows/codex-security-pr.yml`](.github/workflows/codex-security-pr.yml) is the trusted PR dispatcher.
  It runs from protected `main`, executes no PR-owned code, rejects fork heads, binds the GitHub merge commit and
  source-archive hash, and dispatches the protected scanner. Findings are uploaded as SARIF against the PR merge ref;
  the repository's native code-scanning rule requires the `Codex Security` tool result and blocks every in-diff
  finding at the configured thresholds. The separate release deep scan covers the whole source tree.
  GitHub authenticates that rule by SARIF tool name, not workflow path, so same-repository writers are part of the CI
  trust boundary. The dispatcher rejects forks; establish a dedicated GitHub App or equivalent trusted producer
  before granting another account direct write access.
- [`.github/workflows/codex-security.yml`](.github/workflows/codex-security.yml) is the protected source-scan gate.
  It uses GitHub OIDC and OpenAI workload identity federation; do not add a reusable OpenAI API key or expose its
  short-lived token outside the scanner step. PR scans must enter through the trusted dispatcher and retain the
  `workflow_dispatch`/`main` WIF boundary. A release requires a successful `deep` attestation for the exact tagged
  commit and source-archive hash.
- [`.github/workflows/macos-release.yml`](.github/workflows/macos-release.yml) is the authoritative manual GitHub
  chain: `sign` -> `notarize` -> `draft`. Its inputs bind an existing version tag to an exact commit, source-archive
  hash, successful CI and Codex Security runs or a prior release-workflow run, approved artifact hashes, and the
  protected `macos-release` environment.
  Follow the live workflow inputs and [RELEASE.md](RELEASE.md); do not copy changing hashes, run IDs, or confirmation
  phrases into this file.
- Keep the Codex Security package coordinate, tarball hash, committed npm lockfile, protected-environment lock hash,
  scan attestation, and release attestation verifier synchronized. The scanner must install with `npm ci
  --ignore-scripts`; do not replace the full locked graph with a top-level-only npm pin.
- The `draft` operation creates a GitHub draft release only. Publishing that draft, changing repository visibility,
  or uploading release artifacts through any other route requires separate explicit owner approval after the
  remaining release gates are closed.

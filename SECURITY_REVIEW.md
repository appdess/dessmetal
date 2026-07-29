# DessMetal 0.1.x source security review

Review updated through 2026-07-29. Scope: audio/model parsers, preset state, render-thread ownership, bundle entitlements,
packaging/signing scripts, and CI release boundaries. This is a source review plus targeted dynamic validation, not
a formal penetration test.

## Resolved findings

- The WAV reader no longer trusts signed chunk sizes, truncated reads, padded JUNK offsets, extensible channel masks,
  sample-count arithmetic, implausible sample rates, or inconsistent byte rates. Mono IR data is capped at 16 MiB,
  every 24-bit sample requires an exact three-byte read, and non-finite float samples are rejected. Cubic resampling
  validates its rates and is capped at the 8192 taps the convolution engine can consume, preventing hostile ratios
  from creating an unbounded intermediate vector. A malformed corpus runs under ASan, UBSan, and libc++ debug
  hardening.
- Preset strings use a length-bounded reader before any pointer arithmetic. Truncated, oversized, and non-finite
  state values fail closed. DessMetal 0.1.0 emitted an original 14-parameter and a later 20-parameter positional
  layout under the same version. Version 0.1.1 establishes the current 20-parameter layout while accepting both exact
  0.1.0 shapes. Plain/AU state and the optional four-byte VST3 wrapper bypass suffix are distinguished by exact
  remaining byte count, and the VST3 bypass value must be exactly 0 or 1. AU reports rejected chunks to the host and
  restores the complete prior state after the wrapper's preset reset; VST3 rejects parse failures before seeking.
  The original built-in DessBlock value migrates to its stable current host index, missing
  drive controls default bypassed, and AMP defaults enabled. Legacy 14-value custom-amp indices 2/3 and the recognized
  unsafe 0.1.0 18/19-parameter WIP shapes are rejected before configuration is applied. Current parameter IDs, names,
  and order remain unchanged.
- WaveNet bounds global conditioning to 64 values, checks condition-DSP dimension addition for overflow, validates
  combined condition sizes, and rejects wrong-size or non-finite conditioning vectors.
- Amp/boost/IR selection no longer loads from `OnParamChange`. Requests carry selection, generation, and prepared
  audio-configuration metadata and are coalesced for a serialized service path. Producers publish only complete
  prepared objects through atomic pointers; real-time `ProcessBlock` only adopts matching objects at a block
  boundary; immutable model metadata is atomically release-published by request token; displaced model, boost, and
  IR objects are destroyed only by the mutex-serialized non-real-time service. Raw non-render snapshots use that
  same lifetime mutex. Offline rendering invokes the same service synchronously instead of depending on a UI timer.
  Failed or stale requests cannot replace a newer selection and terminal failure disables only the affected module.
- Parametric prewarm uses the current Gain condition rather than an unconditional midpoint, avoiding a mismatched
  initial recurrent state.
- Release entitlements contain hardened runtime audio-input access only. App Sandbox, network client/server, and
  debugger entitlements are absent.
- macOS wrapper classes, AU entry/factory/view symbols, and the SWELL prefix are DessMetal-specific. This prevents
  Objective-C runtime collisions when a host loads DessMetal beside the upstream NeuralAmpModeler plug-in.
- Ordinary GitHub and GitLab validation imports no signing/notarization credentials and publishes no release.
  GitHub CI retains no product artifacts; configured GitLab signing and artifact retention are tag-only manual
  actions. Both signing paths isolate temporary credentials in private keychains. Signed local packaging requires
  the same clean commit and source-archive hash before the build and immediately before promotion.
- Notarization is a separate script that refuses to upload when the approved SHA-256 does not match the exact
  six-file set. It re-verifies the Installer/Application identities, team, timestamps, hardened runtime, universal
  payloads, DMG contents, and nested PKG; staples only a working copy; then assembles and verifies a complete sibling
  set before rollback-capable promotion. An interrupted operation cannot expose a mixed checksum/artifact set.
- Packaging refuses to clean the generated release directory when the checkout, project, build, or release path is
  symlinked or resolves outside the expected checkout. Its retention self-test confirms the external target remains
  untouched when a symlinked build path is injected.
- Packaging now builds each APP/AU/VST3 component from an analyzed root with relocation disabled, a strict bundle
  identifier, version checking, and upgrade-only overwrite behavior. Packaging and notarization both fail closed
  unless expanded `PackageInfo` records the exact destination, `@relocatable=false`, zero `relocate/bundle` entries,
  and the expected strict identifier. Signed runtime verification of this correction passed as recorded below.

## Verification evidence

- The 0.1.1 state-layout regression passes, and the exact current source compiles for both `arm64` and `x86_64`.
  The real installed-AU migration harness passes the original 14-value 0.1.0, later 20-value 0.1.0, and current
  20-value 0.1.1 cases.
- NeuralAmpModelerCore tests pass, including all four bundled parametric amp models, all four fixed drive models,
  Gain response, combined condition-DSP/global conditioning, invalid dimensions, and non-finite defaults.
- The hardened WAV regression corpus passes under AddressSanitizer and UndefinedBehaviorSanitizer with Apple Command
  Line Tools clang 21, plus libc++ debug bounds hardening. The Xcode toolchain's separate pre-`main()` sanitizer
  hang also reproduces in a hello-world program and is not attributed to DessMetal.
- Universal APP/AU/VST3 release compilation passes. The exact current AU editor opens at its native 980 × 410 size,
  and all amp tabs, drive choices, bypass/disabled states, gate, cabinet, EQ, and settings controls were exercised.
- A clean install of the exact staged AU passes strict `auval`, a 15-second stress render, and a real-AU 44.1 kHz
  offline regression that exercises default, amp-bypass, IR-bypass, and re-enable transitions with finite non-silent
  output and asserts the resampler's 27 samples of host-visible latency.
- The earlier signed, notarized, and stapled candidate was withdrawn after Installer relocated `DessMetal.app` to a
  same-bundle-ID repository staging copy instead of `/Applications`. Commit `a075999` introduced the correction;
  GitHub Actions run `30451268583` passed arm64 and x86_64 for that commit. The replacement Developer ID
  APP/AU/VST3/PKG passed identity, signature, and fixed component-policy checks. Apple submission
  `de6a28a0-7ae7-4534-9804-e64e0690a65c` returned `Accepted`; the candidate was stapled, and Gatekeeper accepted the
  DMG and nested PKG.
- At 14:53:36 Europe/Berlin on 2026-07-29, the signed installer ran while a same-bundle-ID staging app existed outside
  `/Applications`. It placed the app in `/Applications`, produced no DessMetal relocation line in the new
  `/var/log/install.log` interval, and installed APP/AU/VST3 bundles that byte-matched the package payload. The app
  launched and its native UI was visible. Existing AU/VST3 bundles and receipts made this an Upgrade, not a pristine
  uninstall/reinstall.
- Steinberg's official VST3 3.8.0 validator passes the exact staged universal plug-in on both architectures (47/47
  standard and 537/537 extensive tests on each of arm64 and x86_64). An independent probe also enumerates all 21
  parameters, round-trips component state, and renders finite non-silent audio.
- Logic Pro 11.2.2 validation with the exact current AU proves discovery, instantiation, native UI opening, SickDess
  selection, internal AMP active/bypass interaction, finite non-silent playback with clipping off, and saved-state
  persistence after quit/reopen. Meter readings came from different playhead segments and are not represented as a
  controlled numerical A/B.
- After competing audio clients were stopped and CoreAudio restarted, the exact staged standalone launched normally,
  remained responsive, and introduced no new HAL property timeout. The earlier timeout was reproduced across
  old/current DessMetal, Logic, and multiple devices and is not attributed to current plug-in source.
- Exact native UI and untouched Logic evidence captures, with SHA-256 hashes, are recorded in `RELEASE.md`. Exact
  final envelope hashes and the detailed Apple/install evidence belong in the matching GitHub release body and
  generated release manifest so packaged documentation does not self-reference mutable post-staple artifacts.

## Residual limits and release gates

- Automated execution cannot judge tone, feel, aliasing, cabinet realism, or noise. A controlled listening test
  remains required.
- The current product loads only its fixed bundled NAM allowlist. Arbitrary custom-NAM import must not be restored
  until JSON bytes/depth/dimensions, convolution groups, and expected weight counts are bounded and malformed model
  inputs are fuzzed.
- GitHub Actions run `30451268583` passed arm64 and x86_64 for fix-introducing commit `a075999`; final run
  `30454055926` passed both architectures for tagged release commit `a3d57e6`. The release body identifies that run,
  final artifact hashes, and Apple submission `f65db216-98ac-433c-b389-5a42edb033d5`.
- The owner has declared all NAM and IR captures first-party, approved their inclusion, and on 2026-07-29 explicitly
  authorized public distribution of the shipped v0.1.1 asset set through this repository and release. This does not
  relicense the captures or artwork under MIT. The owner also identified
  the former OD808/SD1 `modeled_by: jpisoutoftune` and placeholder `gear_type`/`gear_make`/`gear_model` fields as
  incorrect; only those optional values were cleared to `null`, and byte-level verification confirms unchanged model
  weights and unrelated data. The released artwork is covered by the owner's explicit v0.1.1 distribution direction
  above, while a file-level author/provenance record remains a post-release documentation follow-up.
- Capture-lineage claims are intentionally limited: DessBlock Green is the owner's Peavey 5150 green-channel
  capture; SickDess is the owner's Krank Revolution capture; DessTortion Blue/Red are current voicings in the
  historically EVH 5150 Stealth-based family but are not separately attributed to exact hardware variants.
- The replacement passed the expanded component-policy, adversarial same-ID Upgrade, notarization, staple, and
  Gatekeeper gates. Because AU/VST3 bundles and receipts already existed, this does not prove a pristine
  uninstall/reinstall; perform that stronger owner-machine test if it remains a release requirement.
- This checkout has a configured public GitHub `origin`, and CI passed for the exact tagged release commit.
  Ordinary CI imports no signing/notarization credentials; the separate release workflow targets an environment that
  the repository owner must configure with required reviewers and requires explicit sign, notarize, or verified-draft
  dispatch plus a separate operation-specific artifact-transfer phrase. The final exact tagged commit, CI result,
  envelope hashes, and transfer evidence are recorded in the GitHub release body and release manifest.
- The owner explicitly authorized and completed public visibility for the v0.1.1 repository, tag, assets, and release.
  File-level UI/app-icon provenance, a private security-reporting route, controlled listening notes, and every future
  tag, artifact upload, release, or visibility change remain separate follow-ups requiring their own evidence or
  approval.

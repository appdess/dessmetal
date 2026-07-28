# DessMetal macOS release guide

## Local release candidate verified through 2026-07-28

The results below apply to the current 0.1.1 source and the exact locally staged bundles produced from it. Detailed
UI/control evidence came from the exact AU in a view-only validation host; audio-device startup and real-host
load/playback/save-reopen evidence came separately from the exact standalone and Logic Pro AU. No 0.1.0 result is
substituted for an exact-current gate. A fresh Developer ID Application/Installer candidate was built from the clean
source snapshot. Notarization and the private GitHub review upload are separate operations; their results and exact
post-staple hashes must be recorded in the release verification manifest rather than inferred from this source file.

| Area | Status | Current evidence |
| --- | --- | --- |
| Core and model tests | Pass | All four parametric amps, Gain response at 0.2/0.8, four fixed drives, conditioning bounds, finite/non-silent output |
| WAV parsing | Pass | Malicious/truncated corpus under AddressSanitizer and UndefinedBehaviorSanitizer using Apple CLT clang 21; libc++ debug bounds coverage |
| State compatibility | Pass | Exact 14-value and 20-value 0.1.0 layouts plus current 20-value 0.1.1; plain/AU and optional VST3 bypass suffix |
| Real-time ownership | Pass | Atomic request/publish/adopt flow; model, drive, and IR I/O and destruction remain off the render thread |
| Universal APP/AU/VST3 | Pass | Exact staged executables contain `arm64` and `x86_64` |
| Audio Unit | Pass | Strict `auval`, 15-second stress render, 44.1 kHz offline render, and real installed-AU state migration |
| VST3 | Pass | Steinberg 3.8 validator on both architectures: 47/47 standard and 537/537 extensive; independent parameter/state/audio probe |
| Native UI | Pass | Exact 980 × 410 AU editor: all four amp tabs, all four drive choices, bypass/disabled states, gate, cabinet, EQ, and settings |
| Standalone | Pass | Exact staged app launched through CoreAudio, remained responsive, and introduced no new HAL timeout after the audio-service reset |
| Logic Pro 11.2.2 | Pass | Exact AU discovery, instantiation, native UI, finite/non-silent playback, active/bypass interaction, save/reopen persistence |
| Unsigned package | Pass | Exact validation-only six-file PKG/DMG/checksum/manifest/dSYM set, payload/resource/license checks, and DMG integrity |
| Developer ID package | Pass locally | Fresh exact-source APP/AU/VST3 and PKG/DMG set; Developer ID chains/timestamps, hardened runtime, exact payload identity, dSYM UUIDs, checksums, licenses, and mounted contents verified |
| GitHub Actions | Unrun remotely | Workflow syntax and static checks pass locally; no GitHub repository run is claimed |
| Apple notarization and private review upload | Pending external operation | No external result is claimed here; verify the exact release manifest |

## Compatibility and state

DessMetal 0.1.0 wrote two positional state layouts under the same version: the original 14 parameters and the later
20 parameters. Version 0.1.1 establishes the 20-parameter layout unambiguously while accepting both exact 0.1.0
layouts.

- Plain/AU state and the optional four-byte VST3 wrapper bypass suffix are distinguished by exact byte count.
- The wrapper bypass value is accepted only as 0 or 1.
- The original built-in DessBlock value migrates to its stable current host index.
- Missing drive controls receive deterministic bypassed defaults and AMP defaults enabled.
- Legacy 14-value custom-amp indices 2/3 and recognized unsafe 18/19-value work-in-progress layouts fail before any
  state is applied.
- AU rejection restores the full previous state even after the wrapper's preset reset.
- The current 20 parameter IDs, names, order, and the five-state host amp enumeration remain unchanged.

Four amp models are visible. The hidden fifth amp value is the historical, unshipped DessBlock Red compatibility
slot; it resolves to the established red sound for saved sessions. VST3 tools enumerate 21 parameters because the
wrapper contributes its own bypass parameter beyond the 20 persisted DessMetal parameters.

## Real-time and audio validation

Model, drive, and IR changes are serviced outside the audio callback. Requests carry a generation and prepared audio
configuration; complete objects are atomically published and adopted only at a render-block boundary. Displaced
objects are reclaimed by the serialized non-real-time service. Offline rendering uses the same service synchronously
instead of depending on a UI timer.

The exact installed AU passed:

- `auval -strict -v aufx 1YEo AdMs`, including class-state restoration, automation, mono/stereo operation, slicing,
  and sample rates from 11.025 kHz through 192 kHz;
- `auval -strict -stress 15 -v aufx 1YEo AdMs`;
- the 44.1 kHz real-AU offline regression across default, AMP bypass, IR bypass, and re-enable, with finite,
  non-silent output and 27 samples of reported host-visible latency; and
- the original 14-value, later 20-value 0.1.0, and current 20-value 0.1.1 installed-AU migration cases.

Steinberg's official VST3 3.8 validator passed the exact staged universal plug-in on both architectures: 47/47
standard and 537/537 extensive tests on each. An independent probe loaded both slices, enumerated all 21 parameters,
round-tripped component/controller state, and rendered finite, non-silent audio.

After the user stopped competing audio clients and restarted the audio service, the exact staged standalone opened
normally and remained responsive. No new `MACH_RCV_TIMED_OUT` or CoreAudio property timeout appeared during the
fresh run. The earlier timeout was reproduced across old and current DessMetal, Logic, and multiple devices and is
not attributed to the current plug-in source.

Logic Pro 11.2.2 loaded the exact current AU in a disposable capture project. DessMetal produced finite, non-silent
playback with Logic's clipping indicator off, responded to AMP active/bypass changes, and retained SickDess plus Amp
Enabled after save, quit, and reopen. Meter observations came from different playhead segments, so they are not
presented as a controlled numerical A/B. Earlier controlled bounces remain useful listening material but are not
substituted for this exact-current host evidence.

Automated audio checks cannot judge tone, feel, pick response, aliasing, cabinet realism, or noise. The measured red
model ESR of 0.01575 is a training metric, not a sound-quality verdict. A controlled owner listening decision remains
required.

## UI and host evidence

| File | Role | SHA-256 |
| --- | --- | --- |
| `docs/images/dessmetal-0.1.1-ui.jpg` | Exact 980 × 442 host-window capture of the native 980 × 410 editor | `ff23d3f084ec35040c4ba40780f84a7a8deab4c3ae3462ed48e2eb3411f76a30` |
| `docs/images/dessmetal-0.1.1-logic.png` | Untouched Logic Pro window evidence | `dd1d58d1825fd40217929afdcc06a52014511454d01b7fd5827aeaeb34783164` |
| `docs/images/dessmetal-0.1.1-built-with-codex.png` | AI-generated presentation-only composite; not test evidence | `7932d7a53ee14dba75c273433034bcffd52555bc1a8c1e018b0270b33936af90` |

OpenAI Codex assisted with source review, implementation, build and test orchestration, native UI inspection, Logic
host validation, security review, and packaging hardening. The validation claims above remain tied to the recorded
source, artifacts, host behavior, and independent validators. The composite's “end-to-end” wording describes the
completed local app/plug-in pass, not the separately recorded signing, notarization, asset-term, or publication
gates.

## Asset declarations and lineage

The release inventory contains four visible parametric amp NAMs, four fixed drive NAMs, and 16 cabinet IRs. The owner
stated on 2026-07-27 that every shipped NAM and IR was created from the owner's hardware and approved for inclusion
in DessMetal.

- DessBlock Green was captured from the green channel of the owner's Peavey 5150 head.
- SickDess was captured from the owner's Krank Revolution.
- DessTortion Blue and Red are current voicings in the historically EVH 5150 Stealth-based DessTortion family. The
  preserved evidence does not support attributing each variant to separate exact hardware.

The former OD808/SD1 `modeled_by: jpisoutoftune` and placeholder gear fields were owner-identified mistakes. Only
those optional values were cleared to null; trained weights, model configuration, paths, and host order are
unchanged.

The declaration approving inclusion is recorded separately from the root MIT source-code license. Exact public
redistribution terms for NAM/IR binaries, file-level UI/app-icon artwork ownership and terms, and an approved
exact-hash asset manifest remain owner gates. Capture descriptions are descriptive only; trademarks belong to their
respective owners, and no affiliation or endorsement is claimed.

## CI and release boundaries

The local GitHub workflows implement:

- pull-request, main, and manual validation that imports no signing/notarization credentials;
- a manually dispatched Developer ID signing operation;
- a separately approved notarization operation that reuses an exact prior signed candidate; and
- preparation of a verified draft from an exact approved notarized run.

Ordinary CI retains no product artifacts. The release jobs target a `macos-release` environment that the repository
owner must configure with required reviewers; workflow text cannot prove that repository setting. Each dispatch must
also type the exact transfer authorization for its existing tag:

- `UPLOAD SIGNED CANDIDATE vMAJOR.MINOR.PATCH`
- `UPLOAD NOTARIZED CANDIDATE vMAJOR.MINOR.PATCH`
- `UPLOAD DRAFT ASSETS vMAJOR.MINOR.PATCH`

The protected environment supplies these secrets only to the matching release job:

- `DESSMETAL_SIGNING_CERTIFICATES_P12_BASE64` and `DESSMETAL_SIGNING_CERTIFICATES_P12_PASSWORD` for Developer ID
  Application/Installer signing;
- `DESSMETAL_NOTARY_KEY_P8_BASE64` and `DESSMETAL_NOTARY_KEY_ID` for Apple notarization; and
- `DESSMETAL_NOTARY_ISSUER_ID` for a Team API key. Leave the issuer secret unset for an Individual API key.

Validation runs natively on both GitHub's `macos-15` arm64 runner and `macos-15-intel` x86_64 runner. Each runner
executes strict/stress AU checks plus the Steinberg validator's standard and extensive suites. Notarization also
assesses the exact inner PKG; a real privileged clean installation remains an external owner-machine gate.

Publication remains a distinct manual GitHub action. The workflows pass local syntax/static validation but have not
run in a GitHub repository. The current checkout has no configured remote; no GitHub repository creation or upload
is claimed. GitLab validation imports no signing/notarization credentials, while its signed release job is manual
and tag-only.

The local Xcode toolchain's AddressSanitizer runtime was independently reproduced hanging before `main()` even for a
hello-world binary. The complete WAV corpus passed AddressSanitizer plus UndefinedBehaviorSanitizer with the installed
Apple Command Line Tools clang 21. A real GitHub macOS 15 workflow run remains useful CI evidence, but is no longer
the only sanitizer coverage.

## Remaining release gates

1. Approve exact public redistribution terms for every shipped NAM and IR, plus file-level UI/app-icon artwork
   provenance and terms; promote the owner review sheet into the release manifest.
2. Complete the owner's controlled listening decision, including the red-model ESR 0.01575 checkbox.
3. Configure a working public support route and a private vulnerability-reporting route, then replace the currently
   unresolved plug-in/installer contact metadata before distribution.
4. Review and explicitly approve the exact signed DMG SHA-256 before any Apple notarization submission.
5. After notarization, validate the staple, Gatekeeper app/install assessment, and a clean-account or clean-machine
   installation.
6. If GitHub automation is desired, configure required reviewers on the `macos-release` environment, review each
   exact dispatch input and transfer-confirmation phrase, then run the workflows in the real repository.
7. Ask separately before creating a tag, preparing a draft, or publishing a release. A transfer-confirmation phrase
   authorizes only the named workflow artifact upload, not tag creation or publication.

An unnotarized Developer ID candidate is for owner review, not a finished shareable macOS release.

## Release procedure

1. Review the intended source/docs diff and confirm the version in `NeuralAmpModeler/config.h`.
2. Run the core, parser, and state-layout commands in [README.md](README.md).
3. Run `./package_mac.sh --unsigned` and verify the promoted validation-only six-file set.
4. Clean-install the exact staged AU; rerun state migration, strict/stress `auval`, and offline validation whenever
   the binary changes.
5. Run `./package_mac.sh`, approving Developer ID Keychain access only if macOS prompts.
6. Verify `BUILD-INFO.txt`, identities, hardened runtime, certificate chains, trusted timestamps, PKG payload byte
   identity, both-architecture dSYM UUIDs, exact six-file contents, checksums, and mounted DMG contents.
7. Complete the asset and controlled-listening owner gates.
8. Ask for approval of the exact signed DMG hash before submitting it to Apple.
9. Run `DESSMETAL_NOTARY_PROFILE=existing-profile ./notarize_mac.sh <approved-dmg-sha256>`, then record the
   post-staple DMG hash and repeat Gatekeeper and clean-install checks.
10. Ask separately before uploading artifacts, creating a tag, preparing a draft, or publishing.

No release script accepts legal terms, creates credentials, modifies a Developer account, or publishes a release.

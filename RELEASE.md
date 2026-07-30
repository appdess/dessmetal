# DessMetal macOS release guide

## Local validation and verified replacement status through 2026-07-29

The results below apply to the 0.1.1 product source and the locally staged bundles from the fix-introducing build
described below. Detailed UI/control evidence came from the exact AU in a view-only validation host; audio-device
startup and real-host load/playback/save-reopen evidence came separately from the exact standalone and Logic Pro AU.
No 0.1.0 result is substituted for an exact-current gate. The non-relocatable installer correction was introduced by
commit `a075999`, and the source is available in the public GitHub repository. The final exact tagged commit
and its CI run, envelope hashes, and full Apple/install evidence records belong in the matching GitHub release body
and generated release manifest rather than inside the documentation packaged by the artifact itself.

### Withdrawn installer candidate and verified replacement

The previously signed, notarized, and stapled 0.1.1 candidate was withdrawn on 2026-07-29 after a successful Installer
run failed to place `DessMetal.app` in `/Applications`. `/var/log/install.log` showed that PackageKit relocated the app
to a same-bundle-ID copy under the repository's generated staging tree. The AU and VST3 reached their intended system
plug-in directories, but that does not make the distribution package acceptable. The prior candidate, its hashes,
and its Apple result must not be reused as release evidence.

The replacement packaging path explicitly disables component relocation. Every release must expand the final
distribution package and verify all three component `PackageInfo` files:

- APP: `@install-location=/Applications`, `@relocatable=false`, zero `relocate/bundle` entries, and one strict
  identifier for `com.AlexanderDess.app.DessMetal`;
- AU: `@install-location=/Library/Audio/Plug-Ins/Components`, `@relocatable=false`, zero `relocate/bundle` entries,
  and one strict identifier for `com.AlexanderDess.audiounit.DessMetal`; and
- VST3: `@install-location=/Library/Audio/Plug-Ins/VST3`, `@relocatable=false`, zero `relocate/bundle` entries, and
  one strict identifier for `com.AlexanderDess.vst3.DessMetal`.

GitHub Actions run `30451268583` passed on both arm64 and x86_64 for the fix-introducing commit `a075999`. The
replacement Developer ID APP, AU, VST3, and PKG passed identity, signature, and fixed component-policy checks. Apple
submission `de6a28a0-7ae7-4534-9804-e64e0690a65c` returned `Accepted`; the replacement was stapled, and Gatekeeper
accepted the DMG and its nested PKG.

The signed install regression was deliberately adversarial. At 14:53:36 Europe/Berlin on 2026-07-29, a
same-bundle-ID staging app existed outside `/Applications` while the exact signed installer ran. The app landed at
`/Applications/DessMetal.app`, the new `/var/log/install.log` interval contained no DessMetal relocation, and the
installed APP/AU/VST3 bundles byte-matched the package payload. The installed app launched and its native UI was
visible. This was an Installer **Upgrade**, because AU/VST3 bundles and package receipts already existed; it was not
a pristine uninstall/reinstall test.

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
| Standalone | Pass | Exact installed replacement app launched and showed its native UI; prior exact staged app also passed CoreAudio startup without a new HAL timeout after the audio-service reset |
| Logic Pro 11.2.2 | Pass | Exact AU discovery, instantiation, native UI, finite/non-silent playback, active/bypass interaction, save/reopen persistence |
| Package pipeline | Pass | Replacement APP/AU/VST3/PKG passed Developer ID identity/signature and fixed-destination/non-relocation checks; installed bundles later byte-matched the package payload |
| GitHub Actions | Final release commit passed | Run `30454055926` passed arm64 and x86_64 for tagged commit `a3d57e6` |
| Signed install | Pass as Upgrade | Same-ID staging app remained present; Installer placed the app in `/Applications` without a relocation log entry, installed bundles byte-matched payload, and the app launched with native UI visible |
| Apple notarization | Pass | Final submission `f65db216-98ac-433c-b389-5a42edb033d5` was accepted; staple and Gatekeeper DMG/nested-PKG assessments passed |
| Public v0.1.1 release | Pass | Tag `v0.1.1` and its signed, notarized six-file macOS set are published from the exact release commit; the release body records final CI, hashes, Apple submission, and install evidence |

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

The declaration approving inclusion is recorded separately from the root MIT source-code license. On 2026-07-29,
the owner explicitly authorized public distribution of the shipped v0.1.1 asset set through this repository and its
GitHub release. That authorization does not place NAM/IR binaries or artwork under MIT, establish third-party rights,
or replace the need to document the source and redistribution terms of future asset changes. Capture descriptions
are descriptive only; trademarks belong to their respective owners, and no affiliation or endorsement is claimed.

## CI and release boundaries

The local GitHub workflows implement:

- pull-request, main, and manual validation that imports no signing/notarization credentials;
- a trusted same-repository PR dispatcher that binds the GitHub merge commit, queues the protected Codex Security
  scan, and uploads SARIF to the PR merge ref for enforcement by GitHub's native code-scanning rule;
- a protected manual or PR-dispatched Codex Security scan that uses GitHub OIDC and OpenAI workload identity
  federation instead of a stored OpenAI API key;
- a manually dispatched Developer ID signing operation;
- a separately approved notarization operation that reuses an exact prior signed candidate; and
- preparation of a verified draft from an exact approved notarized run.

Ordinary CI retains no product artifacts. The release jobs target a main-only `macos-release` environment with a
required owner review. That repository setting was verified on 2026-07-29 but remains mutable external state. Each
dispatch must also type the exact transfer authorization for its existing tag:

- `UPLOAD SIGNED CANDIDATE vMAJOR.MINOR.PATCH`
- `UPLOAD NOTARIZED CANDIDATE vMAJOR.MINOR.PATCH`
- `UPLOAD DRAFT ASSETS vMAJOR.MINOR.PATCH`

The `codex-security-release` environment is a separate, main-only trust boundary. It has a required owner review and
contains only non-secret configuration variables: the OpenAI WIF audience, identity-provider ID, service-account ID,
the exact `@openai/codex-security@0.1.1` package coordinate, its approved npm tarball SHA-256, and the approved
committed npm lockfile SHA-256. The lockfile resolves the complete 93-entry package graph with registry URLs and
SHA-512 integrity values; CI verifies the top-level tarball against both hashes and installs the graph with
`npm ci --ignore-scripts`; npm lifecycle scripts are also disabled while fetching the top-level tarball. The
workflow pins Node.js 22.23.1 with npm 10.9.8. The environment contains no GitHub Actions secret. The corresponding
OpenAI mapping is restricted to `appdess/dessmetal`, `refs/heads/main`, the
exact `.github/workflows/codex-security.yml` path, `workflow_dispatch`, and the `codex-security-release` environment;
the service account can only read model metadata and make model requests.

The PR dispatcher uses `pull_request_target` only as a trusted control plane: it checks out the generated merge commit
as data, executes no repository-owned script, rejects fork heads, and dispatches the scanner on `main`. This preserves
the existing WIF mapping's exact `refs/heads/main`, workflow path, `workflow_dispatch`, and protected-environment
claims. The scanner uploads SARIF against the exact generated PR merge commit at `refs/pull/<number>/merge`. The
protected `main` ruleset requires a result from the `Codex Security` code-scanning tool and is configured to block
every in-diff finding, including notes. GitHub merge protection does not block a PR on unrelated baseline lines, so
the release deep scan remains the whole-tree gate. Missing or invalid results also leave the merge blocked.

GitHub's native code-scanning rule binds the declared SARIF tool name, not the producing workflow path or analysis
category. The dispatcher rejects fork heads, fork workflow tokens are read-only, and `appdess` was the only direct
collaborator when this gate was enabled on 2026-07-29. Consequently the current single-owner setup is protected from
fork spoofing, but every future same-repository writer must be treated as part of the CI trust boundary. Before
granting another account write access, move the final merge signal to a dedicated GitHub App or equivalent external
producer whose identity cannot be minted by pull-request-owned Actions.

The scan exchanges GitHub's signed job identity for a short-lived OpenAI token, verifies the expected OIDC claims,
and gives each token only to Codex's guarded provider-auth process. The scanner's mandatory API-key login bootstrap
receives a fixed non-secret marker, never the WIF token; `CODEX_HOME` and credential-bearing environment names are
excluded from model tool processes, and the helper refuses callers whose direct parent is not the pinned Codex
native binary at its exact locked-package path. Before any repository checkout, the workflow also runs the pinned
Codex sandbox directly and fails unless
the host PID namespace, GitHub identity environment, `CODEX_HOME`, and direct network are unavailable inside the
sandbox. A host-owned command helper refreshes the token during longer scans. Raw tokens and raw scanner output remain
ephemeral in named runner-temporary paths, including the scanner state database and stderr, and the unconditional
cleanup removes them. A completed, sealed scan retains only a strict sanitized attestation
and sends SARIF directly to GitHub Code Scanning for native ruleset enforcement. Incomplete or invalid scan output
is rejected and cannot satisfy the rule. The service account's automatically created
bootstrap API key was revoked without being copied or stored. Run the fixed `proof` mode before relying on a new or
changed trust mapping.

As configured on 2026-07-29, the dedicated OpenAI project has a hard monthly limit of USD 25, alerts at USD 20 and
USD 25, and permits only `gpt-5.6-sol` and `gpt-5.6-terra`. Each allowed model is capped at 500,000 tokens/minute and
120 requests/minute for this project. These OpenAI account controls are mutable external state and should be checked
again before a release scan; they are not asserted by the repository workflow.

GitHub secret scanning and push protection are enabled for the public repository. The 2026-07-29 local current-tree
and reachable-history credential-pattern scan found no matching secret paths, and GitHub reported zero secret-scanning
alerts. This is defense in depth, not proof that arbitrary future content is safe; review every source and artifact
diff before pushing.

For a future CI release, the protected environment must supply these secrets only to the matching release job:

- `DESSMETAL_SIGNING_CERTIFICATES_P12_BASE64` and `DESSMETAL_SIGNING_CERTIFICATES_P12_PASSWORD` for Developer ID
  Application/Installer signing;
- `DESSMETAL_NOTARY_KEY_P8_BASE64` and `DESSMETAL_NOTARY_KEY_ID` for Apple notarization; and
- `DESSMETAL_NOTARY_ISSUER_ID` for a Team API key. Leave the issuer secret unset for an Individual API key.

It also needs the non-secret `DESSMETAL_APP_IDENTITY` and `DESSMETAL_INSTALLER_IDENTITY` environment variables. The
environment protection was created without copying or changing any Apple credential; those release values must be
configured separately before the next sign/notarize dispatch.

Validation runs natively on both GitHub's `macos-15` arm64 runner and `macos-15-intel` x86_64 runner. Each runner
executes strict/stress AU checks plus the Steinberg validator's standard and extensive suites. Run `30454055926`
passed both runners for tagged release commit `a3d57e6`; the matching release body records that run. Notarization
assessed the exact inner PKG. The completed privileged installer regression was an Upgrade with existing AU/VST3
receipts, not a pristine uninstall/reinstall.

Publication remains a distinct manual GitHub action. The public repository is
`https://github.com/appdess/dessmetal`, and `origin/main` is the development branch. Release `v0.1.1` is published
from exact commit `a3d57e6`; its release body records the matching CI run, envelope hashes, Apple submission, and
install evidence. Future tags, artifacts, releases, or visibility changes still require explicit owner approval.
GitLab validation imports no signing/notarization credentials, while its signed release job is manual and tag-only.

The local Xcode toolchain's AddressSanitizer runtime was independently reproduced hanging before `main()` even for a
hello-world binary. The complete WAV corpus passed AddressSanitizer plus UndefinedBehaviorSanitizer with the installed
Apple Command Line Tools clang 21. A real GitHub macOS 15 workflow run remains useful CI evidence, but is no longer
the only sanitizer coverage.

## Post-release follow-ups

1. Add a file-level author/provenance record for the shipped UI and app-icon artwork; require exact origin and
   redistribution terms for every future model, IR, image, font, or icon change.
2. Complete the owner's controlled listening notes, including the red-model ESR 0.01575 item; keep the metric
   separate from any subjective sound-quality claim.
3. Add a private vulnerability-reporting route. Public support and collaboration use GitHub Issues.
4. If a pristine first-install result is desired, remove the existing installed bundles and receipts through
   the approved uninstall path, then repeat the exact installer test. The completed adversarial Upgrade proves the
   relocation correction but is not a pristine uninstall/reinstall.
5. Recheck the required reviewer and main-only policy on both protected GitHub environments, then review every
   dispatch input and transfer-confirmation phrase before a later release-workflow operation.

An unnotarized Developer ID candidate is for owner review, not a finished shareable macOS release.

## Release procedure

1. Review the intended source/docs diff and confirm the version in `NeuralAmpModeler/config.h`.
2. Run the core, parser, and state-layout commands in [README.md](README.md).
3. Run `./package_mac.sh --unsigned` and verify the promoted validation-only six-file set.
4. Clean-install the exact staged AU; rerun state migration, strict/stress `auval`, and offline validation whenever
   the binary changes.
5. On the exact tagged `main` commit, manually run `.github/workflows/codex-security.yml` in `deep` mode with the
   approved source-archive SHA-256, tag, explicit confirmation phrase, and an owner-reviewed cost threshold. Require
   complete coverage, no high/critical findings, and retain its run ID. The scanner's `max_cost_usd` input is an
   estimated stop threshold, not an account-level hard spending cap; configure project budget/rate limits in OpenAI
   as the independent backstop.
6. Supply that exact deep-scan run ID together with the matching successful macOS validation run ID to the `sign`
   operation in `.github/workflows/macos-release.yml`. The release gate downloads the sanitized attestation and
   independently checks its schema, run, commit, tag, source hash, scanner pin, coverage, policy, and WIF refresh
   evidence before signing credentials are exposed.
7. Run `./package_mac.sh`, approving Developer ID Keychain access only if macOS prompts.
8. Verify `BUILD-INFO.txt`, identities, hardened runtime, certificate chains, trusted timestamps, PKG payload byte
   identity, both-architecture dSYM UUIDs, exact six-file contents, checksums, and mounted DMG contents. Expand the
   three component packages and require the exact fixed destinations, `@relocatable=false`, zero `relocate/bundle`
   entries, and exact strict bundle identifiers listed above.
9. While a same-bundle-ID app copy exists outside `/Applications`, install the exact signed package and verify
   `/Applications/DessMetal.app`, the two fixed system plug-in destinations, receipts, and the absence of PackageKit
   relocation in the new `/var/log/install.log` interval.
10. For future releases or changed assets, complete the applicable provenance and controlled-listening review.
11. Ask for approval of the exact replacement signed DMG hash before submitting it to Apple.
12. Run `DESSMETAL_NOTARY_PROFILE=existing-profile ./notarize_mac.sh <approved-dmg-sha256>`, then record the
    replacement post-staple DMG hash and repeat Gatekeeper and the adversarial clean-install checks.
13. Publish only after the exact tag, CI run, Codex Security attestation, notarization, asset digests, and release body
    agree. Require separate approval for every future tag, artifact upload, release publication, or visibility change.

No local release script accepts legal terms, creates credentials, modifies a Developer account, or publishes a
release. The manual GitHub workflow can create and upload assets to a draft after its explicit gates; it never
publishes that draft.

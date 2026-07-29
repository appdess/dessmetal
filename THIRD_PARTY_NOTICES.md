# Third-party and asset notices

DessMetal source code is distributed under the root MIT license. The binary also incorporates or links the following
projects; their full license files are retained in the source tree and copied into the release DMG's
`THIRD_PARTY_LICENSES` directory.

| Component | License | Source license file |
| --- | --- | --- |
| NeuralAmpModelerCore | MIT | `NeuralAmpModelerCore/LICENSE` |
| AudioDSPTools | MIT | `AudioDSPTools/LICENSE` |
| iPlug2 and Cockos WDL portions | zlib-style / component notices | `iPlug2/LICENSE.txt` |
| Eigen | MPL-2.0 plus listed BSD/Apache/Minpack portions | `NeuralAmpModelerCore/Dependencies/eigen/COPYING.*` |
| VST3 SDK | MIT | `iPlug2/Dependencies/IPlug/VST3_SDK/LICENSE.txt` |
| NanoVG | zlib | `iPlug2/Dependencies/IGraphics/NanoVG/LICENSE.txt` |
| NanoSVG | zlib | `iPlug2/Dependencies/IGraphics/NanoSVG/LICENSE.txt` |
| MetalNanoVG | MIT | `iPlug2/Dependencies/IGraphics/MetalNanoVG/LICENSE` |
| Yoga | MIT | `iPlug2/Dependencies/IGraphics/yoga/LICENSE` |
| nlohmann/json | MIT | `licenses/nlohmann-json-MIT.txt` |
| RtAudio | MIT-style | `iPlug2/Dependencies/IPlug/RTAudio/doc/doxygen/license.txt` |
| RtMidi | MIT-style | `licenses/RtMidi-MIT.txt` |
| stb_textedit | MIT (selected from dual-license terms) | `licenses/stb-MIT.txt` |
| Roboto Regular | Apache-2.0 | Font data copyright Google 2012; `NeuralAmpModelerCore/Dependencies/eigen/COPYING.APACHE` (renamed in the DMG license set) |
| Michroma Regular | SIL Open Font License 1.1 | Copyright 2011 The Michroma Project Authors; `iPlug2/Dependencies/IGraphics/NanoVG/example/LICENSE_OFL.txt` (license text) |

## Bundled captures, IRs, and artwork

On 2026-07-27, project owner Alexander Dess stated that all NAM models and cabinet IRs shipped with DessMetal were
created from the owner's hardware and approved their inclusion in DessMetal. This declaration is recorded separately
from the root MIT source-code license; it does not automatically place the binary captures or artwork under MIT.
On 2026-07-29, the owner explicitly authorized public distribution of the shipped v0.1.1 asset set through this
repository and its GitHub release. That release authorization does not relicense the captures or artwork under MIT
and does not establish rights in third-party marks.

The shipped capture inventory is four visible parametric amp NAMs, four fixed drive NAMs, and 16 cabinet IRs. The
owner supplied the following hardware lineage:

- DessBlock Green was captured from the green channel of the owner's Peavey 5150 head.
- SickDess was captured from the owner's Krank Revolution.
- DessTortion Blue and Red are current voicings in the historically EVH 5150 Stealth-based DessTortion family. The
  preserved evidence does not support separately attributing the two current variants to exact hardware.

These names are descriptive capture-lineage references, not claims of manufacturer affiliation or endorsement.

The restored OD808 and SD1 drive files previously contained `modeled_by: jpisoutoftune` in their embedded NAM
metadata, together with placeholder `gear_type: amp`, `gear_make: tz-make`, and `gear_model: tz-model` values. On
2026-07-28, the owner identified those values as incorrectly set and directed that they be cleared. Only those four
optional metadata values were changed to `null`; byte-level verification confirms that the trained weights, model
configuration, and all unrelated data are unchanged. TS9 and aesahaettr also have null creator metadata; the owner
declaration above is the recorded provenance statement for all four captures. A separate file-level author/provenance
record for the UI artwork and standalone app icon has not been added; future replacements must document their source
and redistribution terms before inclusion.

Peavey, EVH, Krank, ENGL, Mesa, Tube Screamer, TS9, SD-1, OD808, and other product names or marks are the property of their
respective owners. They are used only for descriptive capture/provenance purposes. DessMetal is not affiliated with,
sponsored by, or endorsed by those owners.

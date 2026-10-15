# Third-party source and license inventory

This source preview contains native receiver source, dependency build overlays and a UxPlay patch. Dependency archives and compiled libraries are not bundled. Their exact download locations and archive hashes are in `dependencies.lock.json`.

| Component | Version / source | License evidence | Role |
| --- | --- | --- | --- |
| aomkoyo/obs-airplay-receiver | `a1b57cd56b276e7e2d381c8083f76cd80e2a2e0b` | Explicit LGPL-2.1-or-later notice, retained in `LICENSES/` | Native Windows receiver base; this snapshot elects GPLv3 under LGPL 2.1 section 3 |
| FDH2/UxPlay | 1.73.6, `21eef8df25d91e12635c36d8176ad192725baca2` | Root GPLv3 and per-file notices | AirPlay/RAOP protocol; Windows and reconnect patch supplied here |
| PlayFair, via UxPlay | Included in the pinned UxPlay source | `lib/playfair/LICENSE.md` is GPLv3 | AirPlay compatibility |
| llhttp, via UxPlay | Included in the pinned UxPlay source | MIT; Fedor Indutny and contributors | HTTP parser; MIT notice retained here |
| libplist | 2.7.0 | LGPL-2.1-or-later and per-file notices | Statically linked plist parser |
| OBS Studio / libobs | API headers 30.0.0; intended runtime 32.2.2 | GPL-2.0-or-later, including `libobs/obs.h` | OBS host interface |
| FFmpeg development SDK | BtbN n8.1.2-46-g139afe709a, LGPL shared build | LGPL build configuration retained in the original receiver inventory | Development imports; FFmpeg runtime is supplied by the separately installed target OBS |
| OpenSSL | 3.5.5, openssl-native NuGet package | Apache-2.0; license retained here | libcrypto development/runtime dependency |

The upstream aomkoyo README incorrectly labels UxPlay as LGPL. This inventory follows the actual pinned UxPlay and PlayFair GPLv3 files instead.

mika314/obs-airplay is a historical upstream reference, not a source directory or object linked by this build. Its repository contains GPLv2 text. We do not assert an additional GPLv3 grant for its code; details and limits of the comparison are recorded in `PROVENANCE.md`.

Upstream UxPlay documents additional FairPlay-related legal uncertainty in its README. This source release preserves that provenance and does not claim to resolve it. Copyright-license compatibility does not itself establish patent or other third-party rights.

Before any binary release, archive the complete corresponding source for the actual build and supply its source location alongside the binary. The current source preview does not claim to be the verified Corresponding Source of a publicly distributed binary.

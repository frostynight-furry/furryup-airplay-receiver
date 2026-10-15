# Source provenance and scope

Audit date: 2026-10-15. This document records observations, not legal clearance or a claim of independent clean-room implementation.

## Native base and dependency pins

- Native base: [aomkoyo/obs-airplay-receiver at a1b57cd](https://github.com/aomkoyo/obs-airplay-receiver/tree/a1b57cd56b276e7e2d381c8083f76cd80e2a2e0b).
- Its [license at that commit](https://github.com/aomkoyo/obs-airplay-receiver/blob/a1b57cd56b276e7e2d381c8083f76cd80e2a2e0b/LICENSE) explicitly permits LGPL 2.1 or later. The original notice is preserved unchanged in this snapshot.
- [Upstream commit 3c924d0](https://github.com/aomkoyo/obs-airplay-receiver/commit/3c924d065ac262806f54105a5d6b26b5e60666a3) records replacement of the custom protocol backend by UxPlay. This build's CMake inputs are the native `src/` files and the libraries listed in the dependency lock.
- UxPlay: `21eef8df25d91e12635c36d8176ad192725baca2`; the root LICENSE and `lib/playfair/LICENSE.md` are GPLv3. Some protocol files preserve LGPL-2.1-or-later notices and llhttp preserves MIT notices.
- The Windows/reconnect patch hash is `11f4b74459a80b707641ba733dc3cec9774c4f8f13e8c08ded523d42ce4a80c3`. The dependency lock records all overlay and download hashes.

## mika314 reference

The aomkoyo README describes the project as a Windows port and cites [mika314/obs-airplay](https://github.com/mika314/obs-airplay) as inspiration/reference. The audited mika314 head is `1197817f670c4dabea073f991331fb3af38feb2c`. Its repository contains a GPLv2 license text; no separate GPL-or-later grant was found in its source files, headers or README. The generic example notice at the end of a GPL license text is not recorded as a project-specific relicense.

The aomkoyo base's four principal C implementation files were compared against the seven principal mika314 C++ implementation/header files at that head and at `9542167385ba1864e1c18b91a4312eb839377232`. No copied implementation block was identified. The largest exact token matches were a UxPlay callback signature, FFmpeg include sequences and a four-byte AAC configuration constant. These observations do not establish absence of all derivative expression or a clean-room history. No mika314 source file is compiled or vendored by this snapshot.

Accordingly, this snapshot uses aomkoyo's actual LGPL-2.1-or-later grant, the conversion allowed by LGPL 2.1 section 3, and the actual GPLv3 UxPlay/PlayFair licenses. It does not infer a new mika314 license grant from aomkoyo's README.

## Product boundary

The native module runs inside OBS and can be created and configured through OBS without FurryUP Studio. FurryUP Studio controls external OBS through OBS WebSocket source settings; its application implementation is not part of this native build. No FurryUP Studio source, other plugin source, server code, account data, signing material, Git history or compiled binary is included in the source snapshot.

GPL scope depends on the actual work and how components interact; a separate repository name alone does not determine it. The source scope here is based on the actual native build and independent OBS-hosted operation. This does not license unrelated FurryUP software.

## Status of the snapshot

The source and patch were located alongside the historical reconnect build and checked against its recorded receiver inventory. Source-only publication does not imply that a fresh build has reproduced that binary or that a future binary's complete Corresponding Source has been assembled. Those checks remain in `RELEASE-GATE.md`.

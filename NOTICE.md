# Copyright and license notices

This repository is a source preview of the standalone AirPlay Live Source native OBS receiver. The snapshot as a whole is distributed under GNU GPL version 3. The complete license is in `LICENSE`.

The native receiver is derived from aomkoyo/obs-airplay-receiver at commit `a1b57cd56b276e7e2d381c8083f76cd80e2a2e0b`. That upstream provides an explicit LGPL-2.1-or-later grant. Its original notice is preserved unchanged in `LICENSES/aomkoyo-LGPL-2.1-or-later-NOTICE.txt`; the complete LGPL text is also retained. This snapshot elects the GPL conversion permitted by LGPL 2.1 section 3. This election does not claim ownership of upstream contributions or remove their original notices.

The receiver links UxPlay/PlayFair protocol code under GPLv3, libplist under LGPL-2.1-or-later, and llhttp under MIT. OBS headers are GPL-2.0-or-later. OpenSSL 3 is Apache-2.0. Per-file notices and the dependency licenses remain applicable. See `THIRD_PARTY_NOTICES.md` and `dependencies.lock.json`.

FurryUP modifications include receiver branding and localization, OBS-native status rendering and frontend controls, stable receiver identity, lifecycle controls, Windows portability fixes, and reconnect cleanup. Their inclusion in this GPLv3 source snapshot applies only to the receiver code present here.

FurryUP Studio, its other plugins, account services and FurryUP Studio-side control implementation are separate software and are not included or licensed by this repository. Names and trademarks are used to identify products and upstream projects; this license does not grant trademark rights.

The upstream README describes mika314/obs-airplay as a Windows-port reference and inspiration. That reference is retained in `PROVENANCE.md`. This snapshot does not claim a new license grant from mika314, a clean-room provenance, or completed legal review of the full dependency chain.

AirPlay is a trademark of Apple Inc. This project is not endorsed by Apple or the OBS Project.

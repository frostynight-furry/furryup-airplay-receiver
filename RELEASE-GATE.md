# Remaining work before a binary release

This repository publishes a native receiver **source preview only**. These are pending binary-release checks, not a claim that source publication requires all historical internal-alpha checks.

1. Build from a fresh copy using the locked dependencies and the supplied Windows patch. Record the compiler, linker, CMake versions, commands and resulting DLL hash. The historical matching DLL is not distributed here.
2. Archive complete Corresponding Source for that exact binary: receiver source, relevant dependency source, the applied patches and overlays, build/install scripts, retained notices, and clear source-download instructions. Verify the dependency source locations remain available. The current dependency lock includes an FFmpeg binary SDK and an OpenSSL binary package; these alone are not a complete source archive for a future binary distribution.
3. Reconcile `scripts/package-binary.ps1`, installer metadata and the final public receiver inventory with the selected release version. The retained scripts describe the historical native-alpha packaging shape and must not be advertised as a validated public release.
4. Verify the receiver's PE dependency closure and loading against the actual supported OBS build. The current target is OBS 32.2.2 and its FFmpeg DLL set; compatibility with other OBS versions must be established separately.
5. Exercise actual iPhone audio/video, rotation, reconnect, receiver lifecycle, OBS minimization and extended operation. The source snapshot does not establish completion of these tests.
6. Preserve upstream provenance and copyright notices. LGPL-2.1-or-later to GPLv3 is a permitted conversion; this repository does not claim a new mika314 grant or a clean-room rewrite. Any later evidence of copied incompatible source must be resolved for the affected files before distributing a combined binary.

The historical blanket requirement for a new upstream relicense or clean-room rewrite is not treated as an established fact. The concrete licensing evidence and comparison limits are in `PROVENANCE.md`.

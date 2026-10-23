# Precompiled releases

Super Metroid v0.3.12 retires the first-run Generate & rebuild system. Each
platform download contains the native game, its runtime libraries, launcher
assets and defaults. The player selects their own verified JU ROM and plays.
Python, Rust, CMake, a compiler, source downloads and a writable build tree
are not needed. Audio is enabled at normal volume in release defaults.

`profiles/release.json` identifies the generation recipe. The CFG roots and
title-specific `force_lle` exclusions remain authoritative. Unproved execution
continues through the portable ROM interpreter; precompiled packaging does not
change that fallback or certify an unassisted full-game playthrough.

## Harvested coverage

`historical_profiles` lists tier-2 coverage captured from gameplay. Each file
seeds analysis roots and conservative interpreter-only exclusions; it never
supplies decoding facts, and each is validated separately against the ROM
(snesrecomp `docs/COVERAGE_FEEDBACK.md`). Captures from different builds are
never merged into one file. `profiles/attract_tier2.json` is the July v1
attract capture; the `coverage-*` files come from the campaign below.

The campaign (`tools/sm_coverage_campaign.py`) drives the memory-assisted
progression harness (`tools/sm_progression_qa.py`) headlessly: attract, the
equipment route and save station, both full room sweeps, Ceres, the three
assisted bosses, Mother Brain, the timed escape, the ending, and a guest SRAM
restart. It needs the TCP debug server, so capture from a
`-DSNESRECOMP_ENABLE_TRACE=ON` build of the current generated program:

```sh
python3 tools/sm_coverage_campaign.py run --exe build-trace/SuperMetroidSNESRecomp   --rom /path/to/owned-rom.sfc --output _coverage/<name>
python3 tools/sm_coverage_campaign.py export-profile _coverage/<name>   --output profiles/coverage-<version>-<date>.json
```

Add the file to `historical_profiles`, regenerate with `--strict-idempotent`,
rebuild, rerun the campaign on the new executable (and with
`--interpreter-only` for the reference) and compare:

```sh
python3 tools/sm_coverage_campaign.py compare _coverage/<before> _coverage/<after>
```

The exported profile keeps identities, discoveries and exclusions only:
instruction costs are per-build measurements, and RAM routine snapshots hold
ROM-derived code bytes that must not be published. A shipping build has no
debug server; `release-attract` measures it from the `execution_share`
section of its exit report instead.

To prepare a release from a clean checkout at the pinned framework revisions:

```sh
python3 tools/regen.py --rom /path/to/owned-rom.sfc --strict-idempotent
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DSNESRECOMP_SETUP_HOST=OFF -DSNESRECOMP_BUILD_VERSION=0.3.12
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
python3 tools/release_payload.py archive --output _release_inputs/generated.tar.gz
python3 tools/upload_release_input.py _release_inputs/generated.tar.gz
```

Run the manual Release workflow immediately, supplying VERSION and the printed
archive SHA-256. First use `publish_release=false` to inspect candidate packages.
The signed URL expires: rerun `upload_release_input.py` before a later workflow
run. It uses the maintainer's existing GitHub CLI login and private build vault;
the login credential is never stored in the public repository or Actions.

CI checks dependency pins, the archive digest, and fingerprints of all game
sources, CFG files, profile and identity inputs before extracting generated C.
The same archive is compiled and tested for Windows x64, Linux x64, macOS arm64
and macOS x64. Generated output remains private; only audited runtime ZIPs are
uploaded as public workflow artifacts. No ROM is uploaded, even to private
storage. Toolchain packs are builder dependencies and stay out of the downloads.

`scripts/package_release.sh BUILD PLATFORM` is the common runtime packager.
Linux ZIPs include `Launch.sh` to locate bundled libraries. Extract the complete
folder and use that script. macOS libraries are relocated beside the executable
and receive an ad-hoc signature after relocation. macOS downloads are not
notarized. Windows runtime DLLs are resolved transitively by the shared helper.

Keep the old folder's settings and guest SRAM when updating. Host snapshots
remain specific to the matching runtime build. Validation launches use separate
directories and muted output; packaged audio defaults stay enabled.

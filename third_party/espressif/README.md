# Espressif hardware support bundled with NOS

<!-- Copyright (C) 2026 NOS contributors -->

This directory contains repository-owned snapshots of ESP-IDF 5.4.2 and
esp-usb, including the initialized submodule contents used by NOS C3/S3 builds.
`manifest.json` records upstream URLs, exact commits, recursive submodule
identities, exclusions and SHA-256 digests. Inventories record every regular
file, executable mode and symbolic link. Archives preserve upstream files and
license notices without local patches. The loader adds the documented
`version.txt` release-archive metadata specified in the manifest, so SDK
version detection cannot inherit the enclosing NOS Git revision. They are ordinary Git files, not LFS
pointers or network submodule references.

`scripts/espressif_sources.py` verifies and extracts them into a content-addressed
build cache under `out/esp32-sdk/sources/`. No external IDF checkout is used.
The cache key covers archive, inventory and generated release metadata.
Only documentation/examples, Git metadata and Python bytecode are excluded;
the firmware dependencies, build scripts, headers and vendor binary libraries
are retained. Uninitialized nested CMock test-only submodules are not needed
by NOS firmware and are recorded as such in the upstream submodule listing.

ESP-IDF's top-level Apache-2.0 license is reproduced in `ESP-IDF-LICENSE`.
Individual packages retain their own notices in the archives, including the
vendor binary licenses. esp_tinyusb's Apache-2.0 license is reproduced in
`ESP-TINYUSB-LICENSE`; TinyUSB retains its own MIT license in its source tree.
This bundling does not relicense vendor binaries or remove their restrictions.

The bundle supplies hardware support, compatible utility/protocol libraries
and build machinery. NOS excludes the vendor scheduler and OS/app startup,
provides its own startup/init, and adapts vendor APIs to NOS tasks and devices.
See [the architecture boundary](../../docs/ESP32_ARCHITECTURE.md).

Cross compilers, Python dependencies, host shared libraries and QEMU are
development tools installed separately by the pinned setup scripts. They are
not embedded firmware dependencies and are not included in these archives.

To reproduce the archives from clean upstream checkouts with the recorded
submodules initialized:

```sh
python3 scripts/package_espressif.py --idf-source /path/to/esp-idf --usb-source /path/to/esp-usb
```

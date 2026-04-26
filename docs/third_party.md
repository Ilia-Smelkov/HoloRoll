# Third-party dependencies

## Required at build time

| Tool | Purpose | Notes |
|---|---|---|
| Visual Studio 2022 | C++ compiler | Desktop C++ workload required |
| CMake 3.24+ | Build configuration | `cmake --preset` syntax used |
| REAPER extension SDK | `reaper_plugin.h` | Vendored under `third_party/reaper-sdk/` |

The REAPER SDK is included in the repository for build determinism. To use a
different copy, set the `REAPER_SDK_DIR` environment variable to a folder
containing `reaper_plugin.h`.

## Fetched automatically

| Library | Source | Purpose |
|---|---|---|
| Dear ImGui | `ocornut/imgui` (tag `v1.91.9b`) | Overlay UI |

ImGui is pulled by CMake's `FetchContent` during configure.

## Required only for distribution builds

| Tool | Purpose |
|---|---|
| Inno Setup 6+ | Builds the `.exe` installer (see `installer/holoroll.iss`) |

If you only build the DLL, you don't need Inno Setup. The installer pipeline
is documented separately in `docs/headless_install.md`.

## Licenses

- HoloRoll itself: [MIT](../LICENSE).
- REAPER SDK: © Cockos Inc. — see headers in `third_party/reaper-sdk/sdk/`.
- Dear ImGui: MIT, © Omar Cornut and contributors.

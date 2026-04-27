# Contributing to HoloRoll

Thanks for your interest. This document is intentionally short — the project
is still small and process is light.

## Reporting bugs

Open a [bug report](https://github.com/Ilia-Smelkov/HoloRoll/issues/new?template=bug_report.yml).
Please include:

- REAPER version (Help → About REAPER)
- HoloRoll version (visible in the overlay or `holoroll_config.ini` comment)
- A short reproduction recipe (folder layout, what you clicked)
- The console output from REAPER's `View → Show Console` if anything was logged

## Suggesting features

Open a [feature request](https://github.com/Ilia-Smelkov/HoloRoll/issues/new?template=feature_request.yml).
Briefly describe the workflow you'd like — explaining the *why* helps more
than describing a solution.

## Building from source

See the [README](README.md#from-source). Short version:

```powershell
git clone https://github.com/Ilia-Smelkov/HoloRoll.git
cd HoloRoll
.\scripts\bootstrap.ps1 -Preset x64-Debug -DeployToReaper -KillReaper -RestartReaper
```

The first invocation prompts PowerShell's execution policy. If it complains:

```powershell
Set-ExecutionPolicy -Scope CurrentUser -ExecutionPolicy RemoteSigned
```

## Submitting a pull request

1. Fork and create a feature branch off `main`.
2. Keep changes focused — one logical change per PR.
3. Match the surrounding code style (Google-ish C++, 2-space indent,
   `snake_case` files, `lowerCamel` private members with trailing `_`,
   `UpperCamel` types and methods).
4. Run a Debug build at least once before pushing.
5. Update `CHANGELOG.md` under `[Unreleased]` with a brief note.

### Commit messages

We follow a relaxed [Conventional Commits](https://www.conventionalcommits.org/)
style. Example:

```
feat(library): vertex-count fallback for OBJ pairing
fix(viewport): RMB drag releasing capture too early
docs: clarify region color matching in README
```

## Project structure

```
src/
  core/         pure data + algorithms (parsers, library, config)
  extension/    REAPER plugin glue, entry point, OS dialogs
  render/       OpenGL viewport + ImGui overlay
third_party/
  reaper-sdk/   vendored REAPER extension SDK headers
docs/           additional documentation
scripts/        build + deployment helpers
installer/      Inno Setup script (for distribution builds)
archive/mvp/    pre-0.1.0 MVP code, kept for reference (not built)
```

## License

By contributing you agree that your changes ship under the
[MIT license](LICENSE) of the project.

# Contributing to Fadix Engine

Thanks for your interest in contributing! Fadix is an early-preview engine, so
a clear, well-tested contribution goes a long way. This guide describes the
workflow for code, bug reports, and documentation.

## Starting point

All development happens on the `dev` branch, which branches from the latest
release (v0.9.134) and is the default target for pull requests. `main` only
receives merges for releases and release-critical fixes.

```powershell
git clone https://github.com/IYanel-DEV/FadixEngine.git
git checkout dev
```

## Workflow

1. Create a feature branch off `dev`:

   ```powershell
   git checkout dev
   git pull
   git checkout -b yourname/short-description
   ```

2. Make focused changes. Prefer several small commits over one large one.
3. Build the editor in Debug locally (see [Building](#building)).
4. Run the smoke test that covers the system you changed.
5. Push the branch and open a pull request into `dev` using the
   [pull request template](.github/PULL_REQUEST_TEMPLATE.md).
6. Address review feedback. Keep the discussion in the pull request so the
   reasoning is recorded.

## Building

From the repository root on Windows:

```powershell
.\build.bat 1
```

This configures CMake in `.build\debug-cmake` (fetching dependencies from
source) and builds `fadix_editor` and `fadix_player` in Debug. See the
[README](README.md#build) for manual CMake and release build commands.

## Coding style

- C++20, built with strict warnings enabled.
- Prefer RAII and `std::filesystem` overloads that take `std::error_code`.
- Use `static_cast` for explicit conversions.
- Keep Windows-only code behind `#ifdef _WIN32`.
- Use PascalCase for types and systems, and camelCase or descriptive names for
  local variables and members.
- Follow existing naming suffixes such as `Component`, `System`, and `Manager`.
- Do not add comments that restate the code; use them to explain why.

## Testing

There is no standalone unit-test framework. Verify changes with the relevant
smoke target in `bin\Debug` (for example `fadix_project_smoke.exe`), and make
sure `fadix_editor` still builds in Debug.

## What not to commit

- Build directories (`.build`, `bin`, `artifacts`) and intermediate files.
- Editor caches, personal projects, and local settings.
- Generated embedded assets under `src/generated/`. Regenerate them through the
  normal build flow (`tools/embed_assets.py` via CMake) instead of editing by
  hand.
- Credentials, tokens, or any secrets.

## Reporting bugs

Use the [bug report template](.github/ISSUE_TEMPLATE/bug_report.yml) and
include reproduction steps, the graphics adapter, Windows version, build
configuration, and any Output-panel or console messages.

## Licensing

By contributing, you agree that your contributions are licensed under the
project's [MIT License](LICENSE).

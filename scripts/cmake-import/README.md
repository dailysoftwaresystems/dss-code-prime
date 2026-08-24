# cmake-import

Convert a CMake project into a DSS **`.dss-project.json`** manifest — the file
the compiler consumes via `dss-code-prime --project <file>`.

## Layout

| File | Role |
|---|---|
| `cmake-import.py`  | **The transform** — the single source of truth. Reads a `compile_commands.json`, aggregates sources/includes/defines, writes the manifest. Standalone (`python3 cmake-import.py --help`). |
| `cmake-import.sh`  | Thin wrapper for bash (Linux / macOS / Git Bash / WSL): parse args, detect the host target, run `cmake`, then call `cmake-import.py`. |
| `cmake-import.ps1` | Thin wrapper for PowerShell 7+ (Windows / cross-platform pwsh): same steps, calls `cmake-import.py`. |

Because **both wrappers run the same `cmake-import.py`**, they emit
**byte-identical** JSON by construction — there is no duplicated transform logic
to drift.

## What it does

1. The wrapper verifies `<root-cmake-dir>/CMakeLists.txt` exists.
2. Configures the project into a throwaway build dir with
   `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON` (the build dir is removed afterward).
3. `cmake-import.py` reads the generated `compile_commands.json` and aggregates
   **across every translation unit** (handling both the `command`-string and
   `arguments`-array entry forms):
   - **`sources`** — the `file` of each entry, emitted **relative to the project
     root** (forward-slash, deduped, sorted) — e.g. `src/main.c`.
   - **`includes`** — every `-I<dir>` / `-I <dir>` / `-isystem <dir>`, emitted
     **relative to the project root when it lives under it** (e.g. `include`);
     a dir **outside** the root (a system/toolchain `-isystem` path) stays
     **absolute** (deduped, sorted).
   - **`defines`** — every `-D<NAME>[=VALUE]` (the `-D` stripped, deduped, sorted).
   - All other compiler flags (`-c`, `-o`, `-O`, `-g`, warnings, `-std=…`) are
     ignored — DSS derives those itself.

   Relative source/include paths keep the manifest **portable** (no
   machine-specific absolute paths). DSS resolves a manifest's relative
   `sources` against the **process working directory**, so **run
   `dss-code-prime --project <manifest>` from the project root** (see the
   example below).
4. Writes the `.dss-project.json` with a stable key order:
   `language`, `artifactProfile`, `targets`, `artifactName`, `sources`,
   `includes`, `defines` (`includes` / `defines` / `artifactName` are omitted
   when empty).

## Usage

```sh
scripts/cmake-import/cmake-import.sh  <root-cmake-dir> <output-project-file> [options]
```
```powershell
scripts/cmake-import/cmake-import.ps1 <root-cmake-dir> <output-project-file> [options]
```

**Positional (both required)**

| Arg | Meaning |
|---|---|
| `<root-cmake-dir>`      | CMake project root — must contain `CMakeLists.txt`. |
| `<output-project-file>` | Path of the `.dss-project.json` to write. |

**Options**

| Option | Default | Meaning |
|---|---|---|
| `--target <spec>`       | host-native spec (below) | DSS `"<targetName>:<formatName>"`. **Repeatable** — one artifact per target. |
| `--language <name>`     | `c` | DSS language. |
| `--profile <name>`      | `cli`      | DSS `artifactProfile`. |
| `--artifact-name <name>`| root dir's basename (sanitized) | Binary base name — a bare name, no path separators. |
| `-h`, `--help`          |            | Show help and exit. |

Value flags also accept the `--flag=value` form.

### Host-native target defaults

When no `--target` is given, the host is detected and mapped to the matching
shipped `(targetName:formatName)` spec (target names from
`src/dss-config/targets/*.target.json`; format names are the
`src/dss-config/object-formats/*.format.json` stems):

| Host | Default target spec |
|---|---|
| Linux x86-64   | `x86_64:elf64-x86_64-linux-exec` |
| Linux arm64    | `arm64:elf64-aarch64-linux-exec` |
| Windows x64    | `x86_64:pe64-x86_64-windows-exec` |
| macOS arm64    | `arm64:macho64-arm64-darwin-exec` |
| macOS x86-64   | `x86_64:macho64-x86_64-darwin-exec` |

## Requirements

- **python3** — **both** wrappers run the shared `cmake-import.py`, so python3
  (or `python`) must be on `PATH`. They fail loud with a clear message if it is
  absent. `cmake-import.py` can also be run directly.
- **CMake**, and a generator that emits `compile_commands.json`. **Ninja** and
  **Unix Makefiles** do; **Visual Studio** and **Xcode** do **not**. The wrapper
  tries the default generator first, then falls back to `-G Ninja`, then
  `-G "Unix Makefiles"`; if none produces `compile_commands.json` it fails loud,
  telling you to select a Ninja/Makefiles generator.

## Limitation: link libraries are not captured

`compile_commands.json` is **compile-only** — it records how each `.c` file is
compiled, not how the program is linked, so it carries **no link libraries**.
The tool therefore never emits `resolveLibraries`. If your project links
external libraries whose symbols DSS must resolve, add a `resolveLibraries`
array to the generated manifest by hand:

```jsonc
"resolveLibraries": ["dist/libfoo.so"]
```

## Example

A runnable sample lives in [`example/`](./example): a tiny C executable
(`src/main.c` calls `scaled_square` in `src/util.c`, both include
`include/mathlib.h`), built with `-DSCALE=2 -DDEMO_BUILD` and an include dir.

```sh
# import → manifest (write it into the project root)
scripts/cmake-import/cmake-import.sh scripts/cmake-import/example \
    scripts/cmake-import/example/demo.dss-project.json

# build with DSS — run FROM the project root so the relative sources resolve
cd scripts/cmake-import/example
dss-code-prime --project demo.dss-project.json --output /tmp/demo-out
```

The generated `demo.dss-project.json` (paths relative to the project root):

```json
{
  "language": "c",
  "artifactProfile": "cli",
  "targets": [
    "x86_64:elf64-x86_64-linux-exec"
  ],
  "artifactName": "example",
  "sources": [
    "src/main.c",
    "src/util.c"
  ],
  "includes": [
    "include"
  ],
  "defines": [
    "DEMO_BUILD",
    "SCALE=2"
  ]
}
```

(The `targets` line shows the host-native default — `pe64-…` on Windows,
`macho64-…` on macOS.) The produced binary returns `scaled_square(7) = 7*7*2 =
98`, proving the imported `-DSCALE=2` flowed through the whole
CMake → import → `--project` → codegen chain. Because the `sources` are
relative, `dss-code-prime --project` must run from the project root (the
working directory DSS resolves them against); running it from a different
directory fails with `D_FileNotFound` for `src/main.c`.

## Notes

- `cmake-import.py` emits the manifest with `json.dumps(indent=2,
  ensure_ascii=False)` (LF line endings, forward slashes, minimal escaping) and
  sorts arrays with Python's ordinal `sorted()`. Both shells call it, so the
  bytes are identical regardless of host.
- Running a wrapper invokes `cmake-import.py` with
  `--compile-commands <build>/compile_commands.json --output <file>
  --target <spec>… --language <name> --profile <name>
  --default-artifact-name <root-basename> --relative-to <root>
  [--artifact-name <name>]`. `--relative-to` (the project root, in OS-native
  form so it matches cmake's compile_commands paths) drives the relative-path
  emission; it is supplied automatically by the wrappers, not a user flag.
- The transform understands CMake/Ninja shell-escaping of string-valued defines
  (`-DVER=\"1.2.3\"` → `VER="1.2.3"`) while leaving lone backslashes literal, so
  Windows paths survive.

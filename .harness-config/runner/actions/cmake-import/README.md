# cmake-import

Convert a CMake project into a DSS **`.dss-project.json`** manifest — the file
the compiler consumes via `dsscp --project <file>`.

## Layout

| File | Role |
|---|---|
| `cmake-import.py`  | **The program** — one Python file that runs on every host. It parses the command line, picks the host target, runs `cmake` to get a `compile_commands.json`, aggregates it into the manifest, and removes its throwaway build directory on every way out. `--self-test` proves its refusals, its path base and its generator ladder; `--prove-any-cwd <dsscp>` proves, with a real compiler, that a manifest it writes builds the same files from any working directory. |
| `cmake-import.yml` | The action: how a runner starts the program on the bundled example, and what witnesses that it ran. |
| `example/`         | A runnable CMake project the action converts (see [Example](#example)). |

There is no shell or PowerShell wrapper: the `.sh` and `.ps1` wrappers that once
drove the transform were folded into `cmake-import.py`, so one program behaves the
same on Windows, Linux and macOS.

## What it does

1. Verifies `<root-cmake-dir>/CMakeLists.txt` exists and that `cmake` is on `PATH`.
2. Configures the project into a throwaway build directory inside the root
   (`<root>/.dss-cmake-import-build.<random>/`, unique per run and removed
   afterward) with `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON`. It tries the default
   generator first, then `-G Ninja`, then `-G "Unix Makefiles"`, each in a fresh
   `attemptN` directory; an attempt succeeds only if cmake exits 0 **and** wrote
   `compile_commands.json`.
3. Reads the `compile_commands.json` and aggregates **across every translation
   unit** (handling both the `command`-string and `arguments`-array entry forms):
   - **`sources`** — the `file` of each entry (forward-slash, deduped, sorted).
   - **`includes`** — every `-I<dir>` / `-I <dir>` / `-isystem <dir>` (deduped,
     sorted).
   - **`defines`** — every `-D<NAME>[=VALUE]` (the `-D` stripped, deduped, sorted).
   - All other compiler flags (`-c`, `-o`, `-O`, `-g`, warnings, `-std=…`) are
     ignored — DSS derives those itself.

   **Every path is written relative to the MANIFEST's own directory when it lies
   under that directory, and absolute otherwise** — never with `..`. DSS resolves
   a manifest's relative paths against the directory the manifest is in, not the
   working directory `dsscp` runs in, so the manifest names the same files from
   anywhere. Write it **at the project root** (the example below does) and every
   project path is relative, so the manifest stays relocatable with the project;
   write it anywhere else — this action writes one into its step's build
   directory — and its project paths are absolute. A system/toolchain `-isystem`
   directory outside the manifest's directory is absolute either way.
4. Writes the `.dss-project.json` with a stable key order:
   `language`, `artifactProfile`, `targets`, `artifactName`, `sources`,
   `includes`, `defines` (`includes` / `defines` / `artifactName` are omitted
   when empty), and prints one summary line:
   `cmake-import: <n> sources, <n> includes, <n> defines -> <file>`.

## Usage

```sh
python3 .harness-config/runner/actions/cmake-import/cmake-import.py <root-cmake-dir> <output-project-file> [options]
python3 .harness-config/runner/actions/cmake-import/cmake-import.py --self-test
python3 .harness-config/runner/actions/cmake-import/cmake-import.py --prove-any-cwd <dsscp>
```

On Windows `python` works the same way; the program needs nothing from a shell.

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
| `--compile-commands <path>` |        | Transform an **existing** `compile_commands.json` instead of configuring. The root still names the project (its basename is the default artifact name); it needs no `CMakeLists.txt` and no `cmake`. |
| `--self-test`           |            | Run the program's self-test arms against synthesized inputs (a stand-in `cmake` drives the generator ladder); exit 0 only if every arm passes. |
| `--prove-any-cwd <dsscp>` |          | Import the bundled example into a manifest **outside** its root and build it with `<dsscp>` from the root, from a directory of `#error` look-alikes and from an empty directory; exit 0 only if all three build, a manifest naming the look-alikes fails on them (the trap works), and the root-relative manifest this tool wrote before 2026-09-21 fails from the last two. Run by ctest as `harness/cmake_import_any_cwd`. Takes no other argument. |
| `-h`, `--help`          |            | Show help and exit. |

Value flags also accept the `--flag=value` form, and `--` ends the options.
Flags are case-sensitive and never abbreviated: `--lang` and `--Target` are
refused as unknown flags, as is a lone `-`.

**Exit codes:** `0` the manifest was written · `1` refused (a bad argument, a
missing input, no `compile_commands.json`, a transform error) or a self-test arm
failed · `130` interrupted (Ctrl-C) · `143` terminated (SIGTERM, or SIGHUP where
the host has it). The build directory is removed on every one of these exits; if
it cannot be removed, its path is printed on stderr.

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

The architecture is the **operating system's**, not the interpreter's: on Windows
`PROCESSOR_ARCHITEW6432` wins over `PROCESSOR_ARCHITECTURE` (a 32-bit or emulated
Python still sees the system's), and on macOS a Rosetta-translated Python is seen
through. Any other host is refused by name — pass `--target` there.

## Requirements

- **Python 3.9 or newer** (standard library only).
- **CMake**, and a generator that emits `compile_commands.json`. **Ninja** and
  **Unix Makefiles** do; **Visual Studio** and **Xcode** do **not**. The program
  tries the default generator first, then `-G Ninja`, then `-G "Unix Makefiles"`;
  if none produces `compile_commands.json` it fails loud, showing the last
  attempt's CMake output and telling you to select a Ninja/Makefiles generator.

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
# import → manifest (written into the project root, so its paths are relative)
python3 .harness-config/runner/actions/cmake-import/cmake-import.py .harness-config/runner/actions/cmake-import/example \
    .harness-config/runner/actions/cmake-import/example/demo.dss-project.json

# build with DSS — from any working directory: the paths resolve beside the manifest
dsscp --project .harness-config/runner/actions/cmake-import/example/demo.dss-project.json --output /tmp/demo-out
```

The generated `demo.dss-project.json` (paths relative to its own directory, the
project root):

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
CMake → import → `--project` → codegen chain. Written anywhere else — say
`/tmp/demo.dss-project.json` — the same manifest names
`<repo>/.harness-config/runner/actions/cmake-import/example/src/main.c` and the
rest by their absolute paths, and builds the same files from any directory;
`--prove-any-cwd` checks exactly that, from the root, from a directory holding
look-alikes of every relative name, and from an empty one.

## Notes

- The manifest is written with `json.dumps(indent=2, ensure_ascii=False)` (UTF-8,
  LF line endings, one trailing newline, forward slashes, minimal escaping) and
  its arrays are sorted with Python's ordinal `sorted()`, so the bytes are the
  same on every host for the same project. They are also the bytes the retired
  wrappers produced: the transform functions were carried over unchanged.
- The root and the manifest's directory are used as typed, except that on a
  case-insensitive Windows filesystem one typed in the wrong case (`c:\source\…`)
  is respelled in the case the filesystem stores — the spelling CMake writes into
  `compile_commands.json`. Without that, no path would match the manifest's
  directory and every path would come out absolute. A path under EITHER spelling
  of the manifest's directory is written relative. A symlinked root is never
  resolved: it keeps its own spelling and its own name as the default artifact.
- Until 2026-09-21 the paths were made relative to the PROJECT ROOT, which is right
  only for a manifest written at the root: one written elsewhere (this action's own
  runner writes into its step's build directory) named files that resolved to
  nothing — or, from a directory holding look-alikes, to the wrong files.
- The transform understands CMake/Ninja shell-escaping of string-valued defines
  (`-DVER=\"1.2.3\"` → `VER="1.2.3"`) while leaving lone backslashes literal, so
  Windows paths survive.
- The throwaway build directory lives inside the project root and is deleted when
  the run ends, so an include directory that the configure step itself generates
  inside that build tree (a `configure_file` output directory, say) is emitted as a
  path that no longer exists afterward — point such an include at a real directory
  by hand.

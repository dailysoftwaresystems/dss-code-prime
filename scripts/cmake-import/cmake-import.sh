#!/usr/bin/env bash
# PURPOSE: convert a CMake project into a DSS `.dss-project.json` manifest.
# cmake-import — convert a CMake project into a DSS `.dss-project.json` manifest.
#
# Thin wrapper: it runs `cmake` with CMAKE_EXPORT_COMPILE_COMMANDS=ON, then
# hands the resulting compile_commands.json to the shared transform
# `cmake-import.py` (the single source of truth), which aggregates the per-TU
# sources / includes / defines and writes the `.dss-project.json` the compiler
# consumes via `dss-code-prime --project`.
#
# Usage:
#   scripts/cmake-import/cmake-import.sh <root-cmake-dir> <output-project-file> [options]
#
# Positional (both required):
#   <root-cmake-dir>       CMake project root (must contain CMakeLists.txt)
#   <output-project-file>  path of the .dss-project.json to write
#
# Options:
#   --target <spec>        DSS "<targetName>:<formatName>" (repeatable).
#                          Default = the host-native spec.
#   --language <name>      DSS language name.        Default: c
#   --profile <name>       DSS artifactProfile.      Default: cli
#   --artifact-name <name> binary base name (no path separators).
#                          Default: the root dir's basename (sanitized).
#   -h, --help             show this help and exit.
#
# Requirements: cmake (with a Ninja or Unix Makefiles generator — Visual Studio
# and Xcode do not emit compile_commands.json) AND python3 (runs the shared
# cmake-import.py transform).
#
# NOTE: compile_commands.json is COMPILE-only — it carries no link libraries,
# so `resolveLibraries` is never emitted. If your project links external
# libraries, add a `resolveLibraries` array to the manifest by hand.
#
# Companion of cmake-import.ps1; both call the same cmake-import.py, so they
# emit BYTE-IDENTICAL JSON by construction.

set -euo pipefail

# ─────────────────────────────────────────────────────────────────────────────
# fail-loud helpers
# ─────────────────────────────────────────────────────────────────────────────
prog="$(basename "$0")"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PYFILE="$SCRIPT_DIR/cmake-import.py"

die() {
    printf '%s: error: %s\n' "$prog" "$*" >&2
    exit 1
}

usage() {
    # Print the contiguous leading comment block (after the shebang) as help,
    # stopping at the first non-comment line (robust to line-number drift).
    awk 'NR==1 { next }
         /^#/  { sub(/^# ?/, ""); print; next }
         { exit }' "$0"
}

# ─────────────────────────────────────────────────────────────────────────────
# host-native target spec detection
#
# Format names are the shipped src/dss-config/object-formats/*.format.json
# stems; target names are the shipped targets/*.target.json `target.name`
# values (x86_64 / arm64). Confirmed against the config tree.
# ─────────────────────────────────────────────────────────────────────────────
detect_host_spec() {
    local os arch
    os="$(uname -s 2>/dev/null || echo unknown)"
    arch="$(uname -m 2>/dev/null || echo unknown)"
    case "$os" in
        Linux)
            case "$arch" in
                x86_64|amd64)   echo "x86_64:elf64-x86_64-linux-exec" ;;
                aarch64|arm64)  echo "arm64:elf64-aarch64-linux-exec" ;;
                *) die "unsupported Linux architecture '$arch' — pass --target explicitly" ;;
            esac ;;
        Darwin)
            case "$arch" in
                arm64|aarch64)  echo "arm64:macho64-arm64-darwin-exec" ;;
                x86_64|amd64)   echo "x86_64:macho64-x86_64-darwin-exec" ;;
                *) die "unsupported macOS architecture '$arch' — pass --target explicitly" ;;
            esac ;;
        MINGW*|MSYS*|CYGWIN*|Windows_NT)
            case "$arch" in
                x86_64|amd64)   echo "x86_64:pe64-x86_64-windows-exec" ;;
                *) die "unsupported Windows architecture '$arch' — pass --target explicitly" ;;
            esac ;;
        *) die "unrecognized host OS '$os' — pass --target explicitly" ;;
    esac
}

# ─────────────────────────────────────────────────────────────────────────────
# argument parsing
# ─────────────────────────────────────────────────────────────────────────────
LANGUAGE="c"
PROFILE="cli"
ARTIFACT=""              # explicit --artifact-name; empty => derive from basename
TARGETS=()
positionals=()

while [ $# -gt 0 ]; do
    case "$1" in
        -h|--help)        usage; exit 0 ;;
        --target)         shift; [ $# -gt 0 ] || die "--target requires a value"; TARGETS+=("$1") ;;
        --target=*)       TARGETS+=("${1#*=}") ;;
        --language)       shift; [ $# -gt 0 ] || die "--language requires a value"; LANGUAGE="$1" ;;
        --language=*)     LANGUAGE="${1#*=}" ;;
        --profile)        shift; [ $# -gt 0 ] || die "--profile requires a value"; PROFILE="$1" ;;
        --profile=*)      PROFILE="${1#*=}" ;;
        --artifact-name)  shift; [ $# -gt 0 ] || die "--artifact-name requires a value"; ARTIFACT="$1" ;;
        --artifact-name=*) ARTIFACT="${1#*=}" ;;
        --)               shift; while [ $# -gt 0 ]; do positionals+=("$1"); shift; done; break ;;
        -*)               die "unknown flag '$1' (see --help)" ;;
        *)                positionals+=("$1") ;;
    esac
    shift
done

if [ "${#positionals[@]}" -lt 2 ]; then
    die "expected 2 positional arguments <root-cmake-dir> <output-project-file>; got ${#positionals[@]} (see --help)"
fi
if [ "${#positionals[@]}" -gt 2 ]; then
    die "too many positional arguments (${#positionals[@]}); expected exactly <root-cmake-dir> <output-project-file> (see --help)"
fi
ROOT_DIR="${positionals[0]}"
OUT_FILE="${positionals[1]}"

[ -n "$LANGUAGE" ] || die "--language must be non-empty"
[ -n "$PROFILE" ]  || die "--profile must be non-empty"
case "$ARTIFACT" in
    */*|*\\*) die "--artifact-name must be a bare file name (no '/' or '\\'): '$ARTIFACT'" ;;
esac

# ─────────────────────────────────────────────────────────────────────────────
# validate inputs / tooling
# ─────────────────────────────────────────────────────────────────────────────
[ -d "$ROOT_DIR" ]                 || die "root directory not found: '$ROOT_DIR'"
[ -f "$ROOT_DIR/CMakeLists.txt" ]  || die "no CMakeLists.txt in root directory: '$ROOT_DIR'"
[ -f "$PYFILE" ]                   || die "shared transform not found next to this script: '$PYFILE'"

command -v cmake >/dev/null 2>&1   || die "cmake not found on PATH — install CMake and retry"

PY=python3
if ! command -v "$PY" >/dev/null 2>&1; then PY=python; fi
command -v "$PY" >/dev/null 2>&1   || die "python3 (or python) not found on PATH — required to run cmake-import.py"

# Resolve the root to an absolute path and take its basename for the default
# artifact name. Resolving first makes `.` / trailing-slash inputs yield the
# real directory name (matches the .ps1 Split-Path -Leaf on Resolve-Path).
ROOT_ABS="$(cd "$ROOT_DIR" && pwd)"
ROOT_BASENAME="$(basename "$ROOT_ABS")"

# The relativize base for `--relative-to` must be in the SAME form as the
# compile_commands.json paths, which cmake (a native tool) emits in OS form
# (C:/... on Windows). Under MSYS/Cygwin `pwd` is a POSIX path (/c/...), so
# convert with cygpath -m; elsewhere the POSIX absolute path already matches.
if command -v cygpath >/dev/null 2>&1; then
    REL_BASE="$(cygpath -m -a "$ROOT_DIR")"
else
    REL_BASE="$ROOT_ABS"
fi

# Default target = host-native spec.
if [ "${#TARGETS[@]}" -eq 0 ]; then
    TARGETS=("$(detect_host_spec)")
fi

# ─────────────────────────────────────────────────────────────────────────────
# CMake configure — export compile_commands.json
#
# The default generator does not always emit compile_commands.json (Visual
# Studio / Xcode do not; Ninja + Unix Makefiles do). Try the default first,
# then fall back to Ninja, then Unix Makefiles. A fresh build dir per attempt
# (generators can't be switched in place). The scratch root is removed on exit.
#
# The scratch root MUST be unique per run — do NOT "simplify" it back to a
# constant name. Two concurrent imports of the SAME project (a CI matrix, a
# parallel test suite, two people on one host) would otherwise share one
# directory, and each run's cleanup would delete the other's in-flight CMake
# output. `mktemp -d` CREATES the directory atomically and prints its name, so
# no two runs can ever agree on one; a pid suffix alone would NOT be enough,
# because pids recycle and a killed run leaves its directory behind.
#
# Portability: a template argument ending in X's is honoured by both BSD/macOS
# and GNU/Linux mktemp. GNU-only long options (--tmpdir, --directory) are not,
# and must not be introduced here.
# ─────────────────────────────────────────────────────────────────────────────
SCRATCH_ROOT="$(mktemp -d "$ROOT_ABS/.dss-cmake-import-build.XXXXXXXXXX")" \
    || die "could not create a scratch build directory under '$ROOT_ABS'"
LOG="$SCRATCH_ROOT/.cmake-import.log"

# Log the chosen root once, so a run that fails or is interrupted is still
# debuggable even though the directory name is different every time.
printf '%s: scratch build dir: %s\n' "$prog" "$SCRATCH_ROOT" >&2

cleanup() { rm -rf "$SCRATCH_ROOT" 2>/dev/null || true; }
# EXIT covers normal and error exits; INT/TERM/HUP make an interrupted run
# clean up too, instead of leaking the directory into the user's project.
trap cleanup EXIT
trap 'cleanup; exit 130' INT
trap 'cleanup; exit 143' TERM HUP

gen_attempt=0
try_generator() {
    # $1 = generator name, or "" for the default generator.
    # Each attempt gets its OWN fresh sub-directory, because CMake cannot
    # switch generators inside an existing build tree. A never-reused name
    # means nothing has to be deleted here: the per-run scratch root is
    # created only by mktemp, which is exactly what keeps it ours alone.
    gen_attempt=$((gen_attempt + 1))
    BUILD_DIR="$SCRATCH_ROOT/attempt$gen_attempt"
    mkdir -p "$BUILD_DIR" || return 1
    if [ -z "$1" ]; then
        cmake -S "$ROOT_ABS" -B "$BUILD_DIR" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON >"$LOG" 2>&1
    else
        cmake -S "$ROOT_ABS" -B "$BUILD_DIR" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -G "$1" >"$LOG" 2>&1
    fi
}

CCJSON=""
for gen in "" "Ninja" "Unix Makefiles"; do
    if try_generator "$gen" && [ -f "$BUILD_DIR/compile_commands.json" ]; then
        CCJSON="$BUILD_DIR/compile_commands.json"
        break
    fi
done

if [ -z "$CCJSON" ]; then
    {
        echo "$prog: error: CMake did not produce compile_commands.json."
        echo "Tried the default generator, Ninja, and Unix Makefiles."
        echo "Use a generator that supports CMAKE_EXPORT_COMPILE_COMMANDS"
        echo "(Ninja or Unix Makefiles). Last CMake output:"
        echo "----------------------------------------------------------------"
        [ -f "$LOG" ] && cat "$LOG"
        echo "----------------------------------------------------------------"
    } >&2
    exit 1
fi

# ─────────────────────────────────────────────────────────────────────────────
# invoke the shared transform (single source of truth)
# ─────────────────────────────────────────────────────────────────────────────
pyargs=(--compile-commands "$CCJSON" --output "$OUT_FILE"
        --language "$LANGUAGE" --profile "$PROFILE"
        --default-artifact-name "$ROOT_BASENAME"
        --relative-to "$REL_BASE")
if [ -n "$ARTIFACT" ]; then
    pyargs+=(--artifact-name "$ARTIFACT")
fi
for t in "${TARGETS[@]}"; do
    pyargs+=(--target "$t")
done

"$PY" "$PYFILE" "${pyargs[@]}"

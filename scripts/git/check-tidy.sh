#!/usr/bin/env bash
set -euo pipefail

CLANG_TIDY="${CLANG_TIDY:-clang-tidy}"
BUILD_DIR="${BUILD_DIR:-build}"

ROOT="$(git rev-parse --show-toplevel)"
CONFIG="$ROOT/.clang-tidy"
HEADER_FILTER="^$ROOT/(include|src|tests)/"

files="$(
    git ls-files -- \
        'src/*.c' 'src/*.cc' 'src/*.cpp' 'src/*.cxx' \
        'examples/*.c' 'examples/*.cc' 'examples/*.cpp' 'examples/*.cxx'
)"

if [[ -z "$files" ]]; then
    exit 0
fi

if [[ ! -f "$BUILD_DIR/compile_commands.json" ]]; then
    echo "Error: $BUILD_DIR/compile_commands.json not found."
    echo "Configure CMake with:"
    echo "  cmake -S . -B $BUILD_DIR -DCMAKE_EXPORT_COMPILE_COMMANDS=ON"
    exit 1
fi

if [[ ! -f "$CONFIG" ]]; then
    echo "Error: $CONFIG not found."
    exit 1
fi

failed=0

while IFS= read -r file; do
    echo "clang-tidy: $file"

    args=(
        -p "$BUILD_DIR"
        --config-file="$CONFIG"
        --header-filter="$HEADER_FILTER"
    )

    if [[ "$file" == tests/* ]]; then
        args+=(
            --checks="-misc-use-internal-linkage,-readability-identifier-naming"
        )
    fi

    if ! "$CLANG_TIDY" "${args[@]}" "$file"; then
        failed=1
    fi
done <<< "$files"

if (( failed )); then
    echo
    echo "clang-tidy check failed."
    exit 1
fi

echo "clang-tidy check passed."

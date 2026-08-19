#!/usr/bin/env bash
set -euo pipefail

CLANG_FORMAT="${CLANG_FORMAT:-clang-format}"

files="$(
    git ls-files -- \
        'include/*.c' 'include/*.cc' 'include/*.cpp' 'include/*.cxx' \
        'include/*.h' 'include/*.hh' 'include/*.hpp' 'include/*.hxx' \
        'src/*.c' 'src/*.cc' 'src/*.cpp' 'src/*.cxx' \
        'src/*.h' 'src/*.hh' 'src/*.hpp' 'src/*.hxx' \
        'tests/*.c' 'tests/*.cc' 'tests/*.cpp' 'tests/*.cxx' \
        'tests/*.h' 'tests/*.hh' 'tests/*.hpp' 'tests/*.hxx'
)"

if [[ -z "$files" ]]; then
    exit 0
fi

failed=0

while IFS= read -r file; do
    if ! "$CLANG_FORMAT" --dry-run --Werror "$file"; then
        failed=1
    fi
done <<< "$files"

if (( failed )); then
    echo
    echo "Formatting check failed."
    echo "Run: ./scripts/format.sh"
    exit 1
fi

echo "clang-format check passed."

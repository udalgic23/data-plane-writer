#!/usr/bin/env bash
set -euo pipefail

CLANG_FORMAT="${CLANG_FORMAT:-clang-format}"

git ls-files -- \
    'include/*.c' 'include/*.cc' 'include/*.cpp' 'include/*.cxx' \
    'include/*.h' 'include/*.hh' 'include/*.hpp' 'include/*.hxx' \
    'src/*.c' 'src/*.cc' 'src/*.cpp' 'src/*.cxx' \
    'src/*.h' 'src/*.hh' 'src/*.hpp' 'src/*.hxx' \
    'tests/*.c' 'tests/*.cc' 'tests/*.cpp' 'tests/*.cxx' \
    'tests/*.h' 'tests/*.hh' 'tests/*.hpp' 'tests/*.hxx' |
while IFS= read -r file; do
    "$CLANG_FORMAT" -i "$file"
done

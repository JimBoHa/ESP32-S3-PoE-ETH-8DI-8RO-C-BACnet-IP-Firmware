#!/bin/sh
set -eu

repo_root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
test_tmp=$(mktemp -d)
trap 'rm -rf "$test_tmp"' EXIT HUP INT TERM

cc -std=c11 -Wall -Wextra -Werror \
    -I"$repo_root/main" \
    "$repo_root/main/config_model.c" \
    "$repo_root/tests/test_config_model.c" \
    -o "$test_tmp/test_config_model"
"$test_tmp/test_config_model"

cd "$repo_root"
python3 -m unittest discover -s tests -p 'test_*.py'

#!/usr/bin/env bash
# Host tests for the removal of the built-in third-party Pak Store: no tracked
# reference to its download, no vendored staging in the root makefile, no
# special-casing in the launcher, and a real `make package` over a synthetic
# build tree that downloads nothing and ships no .pakz. Runs entirely inside a
# throwaway directory: no device, no real SD card, no release feed, no key.
set -euo pipefail

cd "$(dirname "$0")"
repo_root="$(cd .. && pwd)"

python3 pak_store_preinstall_test.py "$repo_root"

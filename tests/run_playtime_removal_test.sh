#!/usr/bin/env bash
# Host tests for the removal of the game play-time statistics service: no
# tracked reference to it, no source tree or skeleton pak left, no build or
# package step for it, and a real `zip -r` over synthetic package trees that
# warns about no missing name. Runs entirely inside a throwaway directory: no
# device, no real SD card, no release feed, no key.
set -euo pipefail

cd "$(dirname "$0")"
repo_root="$(cd .. && pwd)"

python3 playtime_removal_test.py "$repo_root"

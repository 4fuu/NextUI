#!/usr/bin/env bash
# Host isolation tests for the old boot install-path convergence: the arbitrary
# *.pakz install backdoor is closed, the tg5040 Bluetooth upgrade and the
# MinUI.zip first install are preserved, and no pak installs on tg5050. Static
# checks plus real runs of the boot scripts against throwaway roots and a python
# unzip shim. Runs entirely inside temporary directories: no device, no real SD
# card, no network, no key.
set -euo pipefail

cd "$(dirname "$0")"
repo_root="$(cd .. && pwd)"

python3 boot_install_path_test.py "$repo_root"

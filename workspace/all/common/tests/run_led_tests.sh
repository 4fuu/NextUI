#!/usr/bin/env bash
# Runs the three host-side LED lease checks.
set -euo pipefail

cd "$(dirname "$0")"

./run_led_lease_test.sh
./run_led_backend_test.sh
./run_led_profile_test.sh

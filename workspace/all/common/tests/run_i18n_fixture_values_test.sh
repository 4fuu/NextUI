#!/usr/bin/env bash
# Host test that the settings i18n table and its charset fixture hold the same
# values.
#
# Companion to run_i18n_test.sh (table shape) and to the font verifier
# (character coverage). The verifier compares character sets, so a value that
# disappears from the fixture is invisible while its characters still appear
# elsewhere in it - which is how the {"Pair", "配对"} regression shipped. This
# one compares the values themselves, as multisets on both sides.
#
# No fontTools and no SDL are needed: the check reads two text files.
#
# Usage (from the repository root):
#   workspace/all/common/tests/run_i18n_fixture_values_test.sh [table [fixture]]

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
cd "$ROOT"

PYTHON="${I18N_FIXTURE_PYTHON:-}"
if [[ -z "$PYTHON" ]]; then
    for candidate in python3 /tmp/nextui-font-venv/bin/python; do
        if "$candidate" -c 'import ast, re' >/dev/null 2>&1; then
            PYTHON="$candidate"
            break
        fi
    done
fi
if [[ -z "$PYTHON" ]]; then
    echo "FAIL: no python3 interpreter found" >&2
    exit 2
fi

exec "$PYTHON" workspace/all/common/tests/i18n_fixture_values_test.py "$@"

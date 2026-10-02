#!/usr/bin/env bash
set -euo pipefail

PYTHONDONTWRITEBYTECODE=1 python3 \
  "${TEST_SRCDIR}/${TEST_WORKSPACE}/scripts/build_pages_test.py"

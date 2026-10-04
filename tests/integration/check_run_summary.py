#!/usr/bin/env python3
"""CI check: `decomp runs show --json` equals the run's summary.json and reports the expected matches."""
import json
import sys

shown_path, summary_path, expected_matches = sys.argv[1], sys.argv[2], int(sys.argv[3])
with open(shown_path) as f:
    shown = json.load(f)
with open(summary_path) as f:
    summary = json.load(f)
if shown != summary:
    sys.exit("runs show differs from summary.json")
if shown["functions_matched"] != expected_matches:
    sys.exit(f"expected {expected_matches} matched functions, got {shown['functions_matched']}")
print(f"run {shown['run']}: {shown['status']}, {shown['functions_matched']} matched, summary.json agrees")

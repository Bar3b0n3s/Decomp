#!/usr/bin/env python3
"""Helpers for the flag search checks in CI.

  flag_search.py set-flags <decomp.json> <flag>...   sets a project's compiler flags
  flag_search.py check <result.json> <flag>...       checks a `decomp --json search flags` result: every
      function byte-exact, and the given flags (the target's build flags) among the equally good ones in
      every group: the alternative of each group they select scores as well as the one chosen.
"""
import json
import sys


def set_flags(path, flags):
    with open(path, encoding="utf-8") as f:
        config = json.load(f)
    config["flags"] = flags
    with open(path, "w", encoding="utf-8") as f:
        json.dump(config, f, indent=2)
        f.write("\n")


def selected(alternatives, flags):
    """The alternative the flags select: the one with the most flags all given, else the empty one."""
    best = None
    for i, alt in enumerate(alternatives):
        parts = [] if alt == "none" else alt.split()
        if parts and all(p in flags for p in parts) and (best is None or len(parts) > best[1]):
            best = (i, len(parts))
    if best is not None:
        return best[0]
    return alternatives.index("none") if "none" in alternatives else 0


def check(path, flags):
    with open(path, encoding="utf-8") as f:
        result = json.load(f)
    score = result["score"]
    assert score["functions"] > 0 and score["exact"] == score["functions"], f"not every function is byte-exact: {score}"
    for group in result["groups"]:
        alternatives = group["alternatives"]
        target = selected(alternatives, flags)
        assert target in group["equivalent"], (
            f"group {group['name']}: the target's '{alternatives[target]}' does not do as well as the chosen "
            f"'{alternatives[group['chosen']]}' (equally good: {[alternatives[i] for i in group['equivalent']]})")
    print(f"flag search: {score['exact']}/{score['functions']} byte-exact with {' '.join(result['flags'])}; "
          f"the target's flags are among the best in all {len(result['groups'])} groups")


if __name__ == "__main__":
    if len(sys.argv) < 3 or sys.argv[1] not in ("set-flags", "check"):
        sys.exit(__doc__)
    if sys.argv[1] == "set-flags":
        set_flags(sys.argv[2], sys.argv[3:])
    else:
        check(sys.argv[2], sys.argv[3:])

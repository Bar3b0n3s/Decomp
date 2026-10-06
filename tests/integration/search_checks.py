#!/usr/bin/env python3
"""Helpers for the search checks in CI.

  search_checks.py set-flags <decomp.json> <flag>...  sets a project's compiler flags
  search_checks.py flags <result.json> <flag>...      checks a `decomp --json search flags` result: every
      function byte-exact, and the given flags (the target's build flags) among the equally good ones in
      every group: the alternative of each group they select scores as well as the one chosen.
  search_checks.py identify <result.json> <toolchain> checks a `decomp --json search identify` result: the
      toolchain ranks first, ahead of the others, with every function byte-exact.
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


def check_identify(path, toolchain):
    with open(path, encoding="utf-8") as f:
        result = json.load(f)
    ranking = result["ranking"]
    table = "; ".join(f"{r['toolchain']}: {r['score']['exact']}/{r['score']['functions']} distance {r['score']['distance']}"
                      + (f" ({r['error'][:80]})" if r.get("error") else "") for r in ranking)
    assert ranking and ranking[0]["toolchain"] == toolchain, f"{toolchain} does not rank first: {table}"
    assert result["decided"], f"{toolchain} is not ahead of the others: {table}"
    score = ranking[0]["score"]
    assert score["functions"] > 0 and score["exact"] == score["functions"], f"{toolchain} does not make every function byte-exact: {table}"
    print(f"identify: {toolchain} first with {' '.join(ranking[0]['flags'])} ({table})")


if __name__ == "__main__":
    commands = {"set-flags": lambda a: set_flags(a[0], a[1:]), "flags": lambda a: check(a[0], a[1:]), "identify": lambda a: check_identify(a[0], a[1])}
    if len(sys.argv) < 3 or sys.argv[1] not in commands:
        sys.exit(__doc__)
    commands[sys.argv[1]](sys.argv[2:])

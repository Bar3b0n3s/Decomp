#!/usr/bin/env python3
"""Writes tests/replay/run/: scripted API responses for a whole `decomp run` over the x86 fixture.

Each matching script compiles the function's real source (tests/fixtures/src) and submits it; every
other function gets default.jsonl, which looks around and gives up. The run finds a function's script
by its safe name without the address (e.g. Player__Hit.jsonl). Regenerate with
`python3 tests/replay/make_run_scripts.py`.
"""
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "run")

SOURCES = {
    "add": "extern int g_counter;\n__declspec(noinline) int add(int a, int b) { return a + b + g_counter; }\n",
    "read_counter": "extern int g_counter;\n__declspec(noinline) int read_counter() { return g_counter; }\n",
    "sum_array": (
        "__declspec(noinline) int sum_array(const int* p, int n) {\n"
        "    int s = 0;\n"
        "    for (int i = 0; i < n; ++i) s += p[i];\n"
        "    return s;\n"
        "}\n"
    ),
    "message": "__declspec(noinline) const char* message() { return \"hello world\"; }\n",
    "scale": "__declspec(noinline) float scale(float x) { return x * 1.5f + 0.25f; }\n",
    "mix": "__declspec(noinline) double mix(double a, double b) { return a * 0.75 + b * 0.25; }\n",
    "Player__Hit": (
        "struct Player {\n"
        "    int hp;\n"
        "    float speed;\n"
        "    __declspec(noinline) void Hit(int dmg);\n"
        "    __declspec(noinline) int Score() const;\n"
        "};\n"
        "\n"
        "void Player::Hit(int dmg) {\n"
        "    hp -= dmg;\n"
        "    if (hp < 0) hp = 0;\n"
        "    speed *= 0.5f;\n"
        "}\n"
    ),
    "other_value": (
        "extern int g_table[8];\n"
        "static const char kName[] = \"other\";\n"
        "\n"
        "__declspec(noinline) int other_value(int x) { return g_table[x & 7] * 2 + kName[x & 3]; }\n"
    ),
}


def pieces(text, n=3):
    size = max(1, (len(text) + n - 1) // n)
    return [text[i:i + size] for i in range(0, len(text), size)] or [""]


def message(msg_id, blocks, stop_reason, usage):
    """A streamed 200 response: message_start, the blocks with their deltas, message_delta, message_stop."""
    events = [
        {"event": "message_start", "data": {"type": "message_start", "message": {
            "id": msg_id, "type": "message", "role": "assistant", "model": "claude-opus-5-5", "content": [],
            "stop_reason": None, "stop_sequence": None,
            "usage": {"input_tokens": usage["input_tokens"], "cache_creation_input_tokens": usage["cache_creation_input_tokens"],
                      "cache_read_input_tokens": usage["cache_read_input_tokens"], "output_tokens": 1}}}},
        {"event": "ping", "data": {"type": "ping"}},
    ]
    for i, block in enumerate(blocks):
        kind = block["type"]
        if kind == "thinking":
            events.append({"event": "content_block_start", "data": {"type": "content_block_start", "index": i,
                                                                    "content_block": {"type": "thinking", "thinking": "", "signature": ""}}})
            for p in pieces(block["thinking"]):
                events.append({"event": "content_block_delta", "data": {"type": "content_block_delta", "index": i,
                                                                        "delta": {"type": "thinking_delta", "thinking": p}}})
            events.append({"event": "content_block_delta", "data": {"type": "content_block_delta", "index": i,
                                                                    "delta": {"type": "signature_delta", "signature": "replay-signature-" + msg_id}}})
        elif kind == "text":
            events.append({"event": "content_block_start", "data": {"type": "content_block_start", "index": i,
                                                                    "content_block": {"type": "text", "text": ""}}})
            for p in pieces(block["text"]):
                events.append({"event": "content_block_delta", "data": {"type": "content_block_delta", "index": i,
                                                                        "delta": {"type": "text_delta", "text": p}}})
        else:  # tool_use
            start = {"type": "tool_use", "id": block["id"], "name": block["name"], "input": {}}
            events.append({"event": "content_block_start", "data": {"type": "content_block_start", "index": i, "content_block": start}})
            for p in pieces(json.dumps(block["input"], separators=(",", ":"))):
                events.append({"event": "content_block_delta", "data": {"type": "content_block_delta", "index": i,
                                                                        "delta": {"type": "input_json_delta", "partial_json": p}}})
        events.append({"event": "content_block_stop", "data": {"type": "content_block_stop", "index": i}})
    events.append({"event": "message_delta", "data": {"type": "message_delta", "delta": {"stop_reason": stop_reason, "stop_sequence": None},
                                                      "usage": {"output_tokens": usage["output_tokens"]}}})
    events.append({"event": "message_stop", "data": {"type": "message_stop"}})
    return {"status": 200, "headers": {"request-id": "req_" + msg_id}, "events": events}


def usage(first_turn, output):
    # The first turn writes the shared prompt prefix to the cache; later turns read it.
    if first_turn:
        return {"input_tokens": 1500, "cache_creation_input_tokens": 6100, "cache_read_input_tokens": 0, "output_tokens": output}
    return {"input_tokens": 900, "cache_creation_input_tokens": 0, "cache_read_input_tokens": 6100, "output_tokens": output}


def tool(tool_id, name, payload):
    return {"type": "tool_use", "id": tool_id, "name": name, "input": payload}


def matching_script(name, source):
    p = name.lower()
    return [
        message(f"msg_{p}_1", [
            {"type": "thinking", "thinking": f"The listing of {name} is short; writing it the way the compiler saw it should be enough."},
            {"type": "text", "text": "Compiling a first version."},
            tool(f"toolu_{p}_1", "compile_and_diff", {"source": source}),
        ], "tool_use", usage(True, 220)),
        message(f"msg_{p}_2", [
            {"type": "text", "text": "Byte-exact; submitting."},
            tool(f"toolu_{p}_2", "submit_result", {"outcome": "matched", "source": source, "reason": ""}),
        ], "tool_use", usage(False, 160)),
    ]


def default_script():
    return [
        message("msg_default_1", [
            {"type": "thinking", "thinking": "This scripted session only looks around before giving up."},
            tool("toolu_default_1", "lookup_symbol", {"query": "g_table"}),
        ], "tool_use", usage(True, 120)),
        message("msg_default_2", [
            tool("toolu_default_2", "record_note", {"text": "Scripted session: no attempt was made."}),
            tool("toolu_default_3", "submit_result", {"outcome": "give_up", "source": "", "reason": "scripted give-up"}),
        ], "tool_use", usage(False, 90)),
    ]


def write(name, responses, comment):
    with open(os.path.join(OUT, name + ".jsonl"), "w", newline="\n") as f:
        f.write("# " + comment + "\n")
        for r in responses:
            f.write(json.dumps(r, separators=(",", ":"), sort_keys=True) + "\n")


def main():
    os.makedirs(OUT, exist_ok=True)
    for name, source in SOURCES.items():
        write(name, matching_script(name, source), f"Scripted session for `decomp run --replay-dir`: matches {name} (compile, then submit).")
    write("default", default_script(), "Scripted session for every other function: looks up a symbol, records a note, gives up.")
    print(f"wrote {len(SOURCES) + 1} scripts to {OUT}")


if __name__ == "__main__":
    main()

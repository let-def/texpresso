#!/usr/bin/env bash

# Exercise the headless QOI protocol and, deliberately, pass -tmpdir as a
# relative path. TeXpresso changes to the document directory during startup,
# so this also guards the launch-directory path resolution contract.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
SANDBOX=$(mktemp -d "${TMPDIR:-/tmp}/texpresso-webview-test.XXXXXX")
trap 'rm -rf "$SANDBOX"' EXIT

mkdir -p "$SANDBOX/output" "$SANDBOX/cache"

python3 - "$ROOT" "$SANDBOX" <<'PY'
import json
import os
from pathlib import Path
import subprocess
import sys

root = Path(sys.argv[1]).resolve()
sandbox = Path(sys.argv[2]).resolve()
stdout_path = sandbox / "stdout.log"
stderr_path = sandbox / "stderr.log"

env = os.environ.copy()
env["SDL_VIDEODRIVER"] = "dummy"
env["XDG_CACHE_HOME"] = str(sandbox / "cache")

command = [
    str(root / "build" / "texpresso"),
    "-texlive",
    "-json",
    "-webview",
    "-tmpdir",
    "output",
    "-test-initialize",
    str(root / "test" / "simple.tex"),
]

# A truncated mkstemp template could redirect output to an unintended path.
# Reject oversized values while parsing the CLI, before starting the engine.
oversized = subprocess.run(
    [
        str(root / "build" / "texpresso"),
        "-webview",
        "-tmpdir",
        "x" * 8192,
        str(root / "test" / "simple.tex"),
    ],
    cwd=sandbox,
    env=env,
    stdin=subprocess.DEVNULL,
    stdout=subprocess.PIPE,
    stderr=subprocess.PIPE,
    text=True,
    timeout=10,
    check=False,
)
if oversized.returncode == 0 or "-tmpdir path is too long" not in oversized.stderr:
    print("FAIL: oversized -tmpdir was not rejected", file=sys.stderr)
    print(oversized.stderr[-2000:], file=sys.stderr)
    raise SystemExit(1)

invalid_resolution = subprocess.run(
    [
        str(root / "build" / "texpresso"),
        "-webview",
        "-resolution",
        "nan",
        str(root / "test" / "simple.tex"),
    ],
    cwd=sandbox,
    env=env,
    stdin=subprocess.DEVNULL,
    stdout=subprocess.PIPE,
    stderr=subprocess.PIPE,
    text=True,
    timeout=10,
    check=False,
)
if (invalid_resolution.returncode == 0 or
        "-resolution expects a positive finite number" not in invalid_resolution.stderr):
    print("FAIL: invalid -resolution was not rejected", file=sys.stderr)
    print(invalid_resolution.stderr[-2000:], file=sys.stderr)
    raise SystemExit(1)

clamped_resolution = subprocess.run(
    [
        str(root / "build" / "texpresso"),
        "-webview",
        "-resolution",
        "100",
        str(root / "test" / "simple.tex"),
    ],
    cwd=sandbox,
    env=env,
    stdin=subprocess.DEVNULL,
    stdout=subprocess.PIPE,
    stderr=subprocess.PIPE,
    text=True,
    timeout=10,
    check=False,
)
if "-resolution clamped from 100 to 8.0" not in clamped_resolution.stderr:
    print("FAIL: oversized -resolution was not clamped", file=sys.stderr)
    print(clamped_resolution.stderr[-2000:], file=sys.stderr)
    raise SystemExit(1)

missing_tmpdir = subprocess.run(
    [
        str(root / "build" / "texpresso"),
        "-json",
        "-webview",
        "-tmpdir",
        "does-not-exist",
        str(root / "test" / "simple.tex"),
    ],
    cwd=sandbox,
    env=env,
    stdin=subprocess.DEVNULL,
    stdout=subprocess.PIPE,
    stderr=subprocess.PIPE,
    text=True,
    timeout=10,
    check=False,
)
if (missing_tmpdir.returncode == 0 or
        "not an existing directory" not in missing_tmpdir.stderr):
    print("FAIL: nonexistent -tmpdir was not rejected", file=sys.stderr)
    print(missing_tmpdir.stderr[-2000:], file=sys.stderr)
    raise SystemExit(1)

requires_json = subprocess.run(
    [
        str(root / "build" / "texpresso"),
        "-webview",
        str(root / "test" / "simple.tex"),
    ],
    cwd=sandbox,
    env=env,
    stdin=subprocess.DEVNULL,
    stdout=subprocess.PIPE,
    stderr=subprocess.PIPE,
    text=True,
    timeout=10,
    check=False,
)
if requires_json.returncode == 0 or "-webview requires -json" not in requires_json.stderr:
    print("FAIL: -webview without -json was not rejected", file=sys.stderr)
    print(requires_json.stderr[-2000:], file=sys.stderr)
    raise SystemExit(1)

protocol_input = "\n".join([
    '["move-window",0,0,100,100]',
    '["map-window",0,0,100,100]',
    '["unmap-window"]',
    '["stay-on-top",true]',
    '["set-output-size",320,240]',
    '["set-trim-factor",0.49]',
]) + "\n"

try:
    result = subprocess.run(
        command,
        cwd=sandbox,
        env=env,
        input=protocol_input,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        timeout=60,
        check=False,
    )
except subprocess.TimeoutExpired as error:
    print("FAIL: webview initialize timed out after 60 seconds", file=sys.stderr)
    if error.stderr:
        print(error.stderr[-4000:], file=sys.stderr)
    raise SystemExit(1)

stdout_path.write_text(result.stdout)
stderr_path.write_text(result.stderr)

if result.returncode != 0:
    print(f"FAIL: TeXpresso exited with status {result.returncode}", file=sys.stderr)
    print(result.stderr[-4000:], file=sys.stderr)
    raise SystemExit(1)

messages = []
for line_number, line in enumerate(result.stdout.splitlines(), start=1):
    if not line:
        continue
    try:
        messages.append(json.loads(line))
    except json.JSONDecodeError as error:
        print(f"FAIL: stdout line {line_number} is not JSON: {error}", file=sys.stderr)
        raise SystemExit(1)

pages = [message for message in messages if message and message[0] == "page"]
if not pages:
    print("FAIL: webview initialize emitted no page message", file=sys.stderr)
    print(result.stdout[-4000:], file=sys.stderr)
    raise SystemExit(1)

page = pages[0]
if len(page) != 8:
    print(f"FAIL: unexpected page message shape: {page!r}", file=sys.stderr)
    raise SystemExit(1)

page_index, page_count, image_name = page[1:4]
dimensions = page[4:8]
if not (isinstance(page_index, int) and isinstance(page_count, int)
        and 0 <= page_index < page_count):
    print(f"FAIL: invalid page indices: {page!r}", file=sys.stderr)
    raise SystemExit(1)
if not all(isinstance(value, int) and value > 0 for value in dimensions):
    print(f"FAIL: invalid page dimensions: {page!r}", file=sys.stderr)
    raise SystemExit(1)
if dimensions[:2] != [320, 240]:
    print(f"FAIL: set-output-size was not applied: {page!r}", file=sys.stderr)
    raise SystemExit(1)

image_path = Path(image_name)
expected_dir = (sandbox / "output").resolve()
if not image_path.is_absolute() or image_path.resolve().parent != expected_dir:
    print(f"FAIL: QOI path did not resolve against launch cwd: {image_path}", file=sys.stderr)
    raise SystemExit(1)
if not image_path.is_file():
    print(f"FAIL: QOI output does not exist: {image_path}", file=sys.stderr)
    raise SystemExit(1)
if image_path.read_bytes()[:4] != b"qoif":
    print(f"FAIL: output does not have a QOI header: {image_path}", file=sys.stderr)
    raise SystemExit(1)

if "[info] Initialize mode: terminating engine process" not in result.stderr:
    print("FAIL: initialize did not report page-ready termination", file=sys.stderr)
    raise SystemExit(1)
if "[info] Initialize mode: test completed" not in result.stderr:
    print("FAIL: initialize did not complete", file=sys.stderr)
    raise SystemExit(1)
if "set-trim-factor clamped from 0.49 to 0.4" not in result.stderr:
    print("FAIL: oversized trim factor was not clamped", file=sys.stderr)
    raise SystemExit(1)

print(
    "PASS: webview emitted QOI page "
    f"{page_index + 1}/{page_count} at {dimensions[0]}x{dimensions[1]}"
)
PY

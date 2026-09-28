#!/usr/bin/env python3
"""Check fixture playback and final HTML against a fresh md4c parse."""

import argparse
import json
import subprocess
from pathlib import Path


FIXTURE_DIR = Path(__file__).resolve().parent


def validate(md2html: Path) -> None:
    # The adversarial set shares the schema and oracle.
    for directory in (FIXTURE_DIR, FIXTURE_DIR / "adversarial"):
        validate_directory(md2html, directory)


def validate_directory(md2html: Path, directory: Path) -> None:
    manifest = json.loads((directory / "manifest.json").read_text(encoding="utf-8"))
    if manifest["schemaVersion"] != 1:
        raise ValueError("unsupported manifest schema")
    if manifest["oracle"]["flags"] != ["--gfm"]:
        raise ValueError("unexpected oracle flags")

    version = subprocess.run(
        [str(md2html), "--version"], capture_output=True, check=True, text=True
    ).stdout.strip()
    if version != manifest["oracle"]["version"]:
        raise ValueError(f"md2html version {version!r} does not match manifest")

    seen = set()
    for fixture in manifest["fixtures"]:
        fixture_id = fixture["id"]
        if fixture_id in seen:
            raise ValueError(f"duplicate fixture ID: {fixture_id}")
        seen.add(fixture_id)

        source = (directory / fixture["source"]).read_bytes()
        chunks = fixture["chunks"]
        if not chunks or any(not isinstance(chunk, str) or not chunk for chunk in chunks):
            raise ValueError(f"{fixture_id}: chunks must be nonempty strings")
        if "".join(chunks).encode("utf-8") != source:
            raise ValueError(f"{fixture_id}: chunks do not reproduce source bytes")

        checkpoints = fixture["checkpoints"]
        positions = [checkpoint["afterChunk"] for checkpoint in checkpoints]
        if not positions or positions != sorted(set(positions)) or positions[-1] != len(chunks):
            raise ValueError(f"{fixture_id}: checkpoints must be sorted and end at completion")
        for checkpoint in checkpoints:
            position = checkpoint["afterChunk"]
            if position < 1 or position > len(chunks):
                raise ValueError(f"{fixture_id}: checkpoint outside chunk range")
            if checkpoint["prefix"] != "".join(chunks[:position]):
                raise ValueError(f"{fixture_id}: checkpoint prefix does not match chunks")
            if not checkpoint.get("expectedPreview"):
                raise ValueError(f"{fixture_id}: checkpoint has no preview expectation")

        result = subprocess.run(
            [str(md2html), "--gfm"], input=source, capture_output=True, check=True
        )
        expected = (directory / fixture["expectedHtml"]).read_bytes()
        if result.stdout != expected:
            raise ValueError(f"{fixture_id}: final HTML differs from fresh md4c parse")
        print(f"ok {fixture_id}: {len(chunks)} chunks, {len(checkpoints)} checkpoints")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--md2html", required=True, type=Path, help="md4c 0.6.0 md2html binary")
    args = parser.parse_args()
    validate(args.md2html.resolve())

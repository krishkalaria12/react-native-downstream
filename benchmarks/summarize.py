#!/usr/bin/env python3
"""Summarize app traces without treating JavaScript callbacks as native paint."""

import argparse
from collections import defaultdict
import json
import math
from pathlib import Path


SAMPLE_EVENTS = {
    "arrival-to-frame-sample": ("chunkToFrameEstimateMs", "value"),
    "js-frame-interval": ("jsFrameIntervalMs", "value"),
    "batch-delivery": ("deliveryLatenessMs", "latenessMs"),
}
IDENTITY_FIELDS = (
    "fixture",
    "sourceHash",
    "workloadSize",
    "workloadShape",
    "schedule",
    "seed",
    "streams",
)


def percentile(values: list[float], fraction: float) -> float | None:
    if not values:
        return None
    ordered = sorted(values)
    position = (len(ordered) - 1) * fraction
    low = math.floor(position)
    high = math.ceil(position)
    return round(ordered[low] + (ordered[high] - ordered[low]) * (position - low), 3)


def distribution(values: list[float]) -> dict:
    return {
        "samples": len(values),
        "p50": percentile(values, 0.50),
        "p95": percentile(values, 0.95),
        "p99": percentile(values, 0.99),
        "min": round(min(values), 3) if values else None,
        "max": round(max(values), 3) if values else None,
    }


def numbers(events: list[dict], event_type: str, field: str = "value") -> list[float]:
    return [
        event[field]
        for event in events
        if event.get("type") == event_type
        and isinstance(event.get(field), (int, float))
        and not isinstance(event.get(field), bool)
        and math.isfinite(event[field])
    ]


def batch_keys(events: list[dict], event_type: str) -> set[tuple[int, int]]:
    return {
        (event["stream"], event["batch"])
        for event in events
        if event.get("type") == event_type
        and isinstance(event.get("stream"), int)
        and isinstance(event.get("batch"), int)
    }


def summarize_run(run: dict) -> dict:
    events = run.get("events", [])
    summary = {
        key: run.get(key)
        for key in (
            "run",
            "startedAt",
            "fixture",
            "sourceBytes",
            "sourceHash",
            "workloadSize",
            "workloadShape",
            "schedule",
            "seed",
            "streams",
            "traceEnabled",
            "sampleStride",
            "batchWindowMs",
            "droppedEvents",
            "completionStatus",
            "runDurationMs",
            "wallDurationMs",
        )
    }
    for event_type, (name, field) in SAMPLE_EVENTS.items():
        summary[name] = distribution(numbers(events, event_type, field))

    flushed = batch_keys(events, "batch-flush")
    rendered = batch_keys(events, "renderer-update")
    observed = batch_keys(events, "first-visible-frame-estimate")
    superseded = batch_keys(events, "superseded-before-frame")
    summary["sampledBatches"] = {
        "flushed": len(flushed),
        "rendererUpdated": len(rendered),
        "frameEstimated": len(observed),
        "supersededBeforeFrame": len(superseded),
        "unobservedAfterFlush": len(flushed - observed - superseded),
        "supersededRate": round(len(superseded) / len(rendered), 4)
        if rendered
        else None,
    }
    summary["batchSizeChunks"] = distribution(numbers(events, "batch-flush"))
    return summary


def group_key(environment: dict, summary: dict) -> str:
    # Run the tool on exports from one physical device at a time. Screen metadata
    # keeps obvious mixed-device inputs apart, but it is not a device identifier.
    return json.dumps(
        {
            "platform": environment.get("platform"),
            "platformVersion": environment.get("platformVersion"),
            "screen": environment.get("screen"),
            **{field: summary[field] for field in IDENTITY_FIELDS},
        },
        sort_keys=True,
    )


def run_key(environment: dict, summary: dict) -> tuple[str, str | None, int | None]:
    return (group_key(environment, summary), summary["startedAt"], summary["run"])


def summarize_group(group: list[dict]) -> dict:
    completed = [run for run in group if run["completionStatus"] == "complete"]
    traced = [run for run in completed if run["traceEnabled"] is True]
    untraced = [run for run in completed if run["traceEnabled"] is False]
    result = {
        "runs": len(group),
        "completedRuns": len(completed),
        "traceOnRuns": len(traced),
        "traceOffRuns": len(untraced),
        "repeatedRunVariation": {},
    }
    for metric in ("chunkToFrameEstimateMs", "jsFrameIntervalMs", "deliveryLatenessMs"):
        for statistic in ("p50", "p95", "p99"):
            values = [
                run[metric][statistic]
                for run in traced
                if run[metric][statistic] is not None
            ]
            result["repeatedRunVariation"][f"{metric}.{statistic}"] = distribution(values)

    result["runDurationMs"] = {
        "traceOn": distribution(
            [run["runDurationMs"] for run in traced if run["runDurationMs"] is not None]
        ),
        "traceOff": distribution(
            [run["runDurationMs"] for run in untraced if run["runDurationMs"] is not None]
        ),
    }
    on = result["runDurationMs"]["traceOn"]["p50"]
    off = result["runDurationMs"]["traceOff"]["p50"]
    result["traceOnMinusOffMedianRunDurationMs"] = (
        round(on - off, 3) if on is not None and off is not None else None
    )
    return result


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("traces", nargs="+", type=Path)
    args = parser.parse_args()
    groups = defaultdict(list)
    seen = set()
    for path in args.traces:
        export = json.loads(path.read_text(encoding="utf-8"))
        if export.get("schemaVersion") != 1 or not isinstance(export.get("runs"), list):
            raise ValueError(f"{path}: unsupported app trace export")
        environment = export.get("environment", {})
        for run in export["runs"]:
            summary = summarize_run(run)
            identity = run_key(environment, summary)
            if identity in seen:
                continue
            seen.add(identity)
            print(json.dumps({"kind": "run", "file": str(path), **summary}, sort_keys=True))
            groups[group_key(environment, summary)].append(summary)
    for key, group in sorted(groups.items()):
        print(
            json.dumps(
                {"kind": "group", "identity": json.loads(key), **summarize_group(group)},
                sort_keys=True,
            )
        )


if __name__ == "__main__":
    main()

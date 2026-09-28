import unittest
from unittest.mock import patch
from contextlib import redirect_stdout
import io
import json
from pathlib import Path
import tempfile

from summarize import main, summarize_group, summarize_run


def run(trace_enabled=True, status="complete", duration=100.0, events=None):
    return {
        "run": 1,
        "startedAt": "2026-09-28T00:00:00.000Z",
        "fixture": "paragraph",
        "sourceHash": "abc",
        "workloadSize": "fixture",
        "workloadShape": "repeat",
        "schedule": "bursty",
        "seed": 7,
        "streams": 1,
        "traceEnabled": trace_enabled,
        "completionStatus": status,
        "runDurationMs": duration,
        "wallDurationMs": duration,
        "events": events or [],
    }


def event(kind, batch, **details):
    return {"type": kind, "stream": 0, "batch": batch, **details}


class SummarizeTest(unittest.TestCase):
    def test_superseded_batches_are_counted_outside_latency_samples(self):
        summary = summarize_run(
            run(
                events=[
                    event("batch-flush", 1, value=2),
                    event("renderer-update", 1),
                    event("superseded-before-frame", 1),
                    event("batch-flush", 2, value=1),
                    event("renderer-update", 2),
                    event("first-visible-frame-estimate", 2),
                    event("arrival-to-frame-sample", 2, value=12.5),
                    event("batch-delivery", 2, latenessMs=3.0),
                ]
            )
        )
        self.assertEqual(summary["chunkToFrameEstimateMs"]["samples"], 1)
        self.assertEqual(summary["sampledBatches"]["supersededBeforeFrame"], 1)
        self.assertEqual(summary["sampledBatches"]["supersededRate"], 0.5)
        self.assertEqual(summary["deliveryLatenessMs"]["p50"], 3.0)

    def test_group_excludes_cancelled_runs_and_compares_trace_toggles(self):
        summaries = [
            summarize_run(run(duration=100)),
            summarize_run(run(duration=120)),
            summarize_run(run(trace_enabled=False, duration=80)),
            summarize_run(run(status="cancelled", duration=1000)),
        ]
        group = summarize_group(summaries)
        self.assertEqual(group["completedRuns"], 3)
        self.assertEqual(group["runDurationMs"]["traceOn"]["p50"], 110)
        self.assertEqual(group["traceOnMinusOffMedianRunDurationMs"], 30)

    def test_overlapping_exports_count_each_run_once(self):
        with tempfile.TemporaryDirectory() as directory:
            paths = [Path(directory) / name for name in ("first.json", "second.json")]
            first = run()
            second = {**run(duration=120), "run": 2, "startedAt": "2026-09-28T00:01:00.000Z"}
            for path, runs in zip(paths, ([first], [first, second])):
                path.write_text(
                    json.dumps({"schemaVersion": 1, "environment": {}, "runs": runs}),
                    encoding="utf-8",
                )
            output = io.StringIO()
            with patch("sys.argv", ["summarize.py", *(str(path) for path in paths)]):
                with redirect_stdout(output):
                    main()
        rows = [json.loads(line) for line in output.getvalue().splitlines()]
        self.assertEqual([row["run"] for row in rows if row["kind"] == "run"], [1, 2])
        self.assertEqual([row["runs"] for row in rows if row["kind"] == "group"], [2])


if __name__ == "__main__":
    unittest.main()

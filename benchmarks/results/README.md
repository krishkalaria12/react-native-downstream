# Device baseline results

Physical-device benchmarking is deferred until the implementation work is complete. No physical-device baseline has been recorded in this repository yet. Do not treat Expo export, prebuild, or simulator output as a latency result.

Store one JSON export from the example per run, with matching native trace and screen recording when available. Use the directory layout and metadata in [the measurement method](../../docs/performance-methodology.md). Keep failed runs with their failure reason. A summary should report fixture hash, exact versions, device details, run order, trace overhead, p50/p95/p99, variation, and the observed cost ranking. The renderer ownership decision belongs in that summary after both physical platforms have measurements.

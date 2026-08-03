# Benchmark methodology

> **Status:** Draft
> **Owner:** Benchmarking
> **Last reviewed:** 2026-08-02
> **Related:** ADR-0012, PERFORMANCE-PHILOSOPHY.md, C15

## The primary metric

```
goodput = verified original file payload bits / total transfer time
```

`total transfer time` is wall clock from user-visible start to user-visible completion,
**including** acquisition, capability announcement, the fountain-decode tail and final hash
verification. Not the steady-state streaming window.

A run that streams fast but takes three seconds to acquire lock has that three seconds in
its denominator. That is the honest number and it is the one users experience.

## Required reported metrics

Every benchmark run reports all of these. A run missing any is incomplete.

### Throughput and correctness
| Metric | Notes |
|---|---|
| **Verified payload goodput** | KB/s. The metric. Numerator and denominator reported separately so it can be re-derived |
| Raw optical bit rate | Labelled as raw. Never compared to another system's goodput |
| Corrected decoder bit rate | Intermediate |
| Symbol error rate | Pre-FEC |
| Frame error rate | Post-FEC |
| Header failure rate | Measures `H` |
| Erasure rate | Feeds the fountain layer |
| FEC overhead | Actual, not nominal |
| Fountain overhead | Actual reception overhead |
| Transfer success | Boolean — hash verified or not |

### Timing and system
| Metric | Notes |
|---|---|
| Acquisition time | Start to first decoded frame. Dead time in the denominator |
| Decode latency | Per stage and end-to-end |
| Camera frame drop rate | Directly reduces `Pc` |
| Display pacing errors | Missed/duplicated presented frames |
| CPU utilisation | Per core, and total |
| GPU utilisation | |
| Memory | Peak RSS and peak native allocation |
| Battery drain | mAh or % over the run |
| Thermal status | Sampled throughout, not just at the end |

### Conditions (all mandatory)
Distance · angle · motion condition · ambient brightness (lux) · screen brightness ·
device pairing (both models and OS builds) · display mode · camera configuration ·
modulation profile · grid and layout · file size and type · software commit.

## Reporting rules

1. **Never report a best run as system performance.** Report median and range across a
   stated number of runs. A minimum of 5 runs per configuration; more where variance is high.
2. **Always name the metric.** Unlabelled numbers are rejected in review.
3. **Never compare our goodput to another system's raw rate**, or vice versa. If a cited
   paper's metric is ambiguous, say so.
4. **Report failures.** A configuration that fails to complete is a result — record the
   failure rate, do not filter to successful runs.
5. **Preserve raw data.** `data/experiments/<ID>/raw/` is append-only.
6. **Compression gains are reported separately** and never folded into channel throughput
   (NG8).

## Benchmark categories

### 1. Controlled rigid mount
Both devices fixed on a rig at a set distance and angle, controlled lighting.
**Purpose.** Upper-bound measurement and regression detection with minimal variance.
**Reported as.** "Controlled stationary" — this is the category milestone 5 refers to.

### 2. Typical indoor handheld alignment
Receiver handheld, transmitter propped, typical indoor lighting, a user making a genuine
attempt to hold steady.
**Purpose.** The realistic number. Most likely to differ sharply from category 1 —
and the gap between them is itself an important result.

### 3. Stress conditions
Extreme distance and angle, poor lighting, direct glare, deliberate motion, partial
occlusion.
**Purpose.** Find the failure envelope and verify graceful degradation. A system that
fails safely (no output) is acceptable; one that delivers corrupt output is not.

### 4. Cross-device matrix
Every reference device as transmitter × every reference device as receiver.
**Purpose.** Detect device-specific assumptions early (RISK-017). Run from Phase 4, not
deferred to Phase 10 — the whole point is early warning.

### 5. Thermal endurance
Sustained transfer at maximum brightness and full rate until thermal steady state.
**Purpose.** Characterise the degradation curve (RISK-010). **Sustained goodput, not peak,
is what the milestones require** — a system hitting 250 KB/s for 20 seconds and settling at
120 KB/s has not met milestone 4.

### 6. Large-file transfer
100 MB transfers end to end.
**Purpose.** Validate memory bounds, fountain behaviour at scale, thermal behaviour, and
that goodput does not degrade with transfer length.

## Test files

| File | Purpose |
|---|---|
| Deterministic pseudorandom, 1 MB | Fast iteration; incompressible so channel-limited |
| Deterministic pseudorandom, 10 MB | Standard benchmark |
| Deterministic pseudorandom, 100 MB | Large-file, thermal, memory-bound |
| Small files: 1 KB, 10 KB, 100 KB | **Overhead-dominated regime.** Acquisition and session setup dominate; goodput will be much lower and that is the honest result |
| Highly compressible (e.g. zeros, repetitive text), 10 MB | **Reported separately.** Never mixed into channel throughput figures |

All pseudorandom files are generated from a fixed seed so they are reproducible without
storing them.

**On small files:** it is tempting to exclude them because they make the numbers look bad.
They should be reported precisely because they show where the overhead lies, and because
transferring a small file quickly is a real use case.

## Run procedure

1. Both devices at a defined battery level and thermal state (cooled to ambient between
   runs in categories 1 and 5 — thermal history contaminates results).
2. Record full environment metadata automatically, not by hand.
3. Verify display mode settled before starting (TX-04).
4. Verify camera manual settings applied — check returned metadata, not the request.
5. Run N ≥ 5 repetitions.
6. Record raw per-frame telemetry for at least one run per configuration.
7. Compute goodput with numerator and denominator recorded separately.
8. Store everything under `data/experiments/<ID>/raw/`.

## Reproducibility

Every reported figure must be re-derivable from stored raw data by a committed script.
A number that cannot be regenerated is not a result.

```bash
tools/bench/report.py data/experiments/EXP-0NN/raw/ --format markdown
```

## The observer effect

Telemetry must not perturb what it measures. Per-frame recording uses pre-allocated
lock-free ring buffers with no allocation or I/O on the hot path. **The overhead is itself
measured** — benchmark with telemetry on and off, and if the difference is material, the
telemetry is wrong and must be fixed before its numbers are trusted.

## Milestone acceptance

A milestone is met when:

- The target goodput is achieved as a **median** across ≥5 runs
- In the **stated benchmark category** (milestone 4: category 2 handheld; milestone 5:
  category 1 rigid mount)
- **Sustained** through a 10 MB transfer at minimum, with thermal endurance data supporting
  that it holds
- With **verified** output — hash match on every counted run
- Reproduced on **at least two device pairings**
- With raw data preserved and the computation reproducible

Anything less is reported as progress toward the milestone, not achievement of it.

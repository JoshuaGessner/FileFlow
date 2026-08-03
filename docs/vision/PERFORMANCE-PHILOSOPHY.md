# Performance philosophy

> **Status:** Draft
> **Owner:** Project lead
> **Last reviewed:** 2026-08-02
> **Related:** ADR-0012, docs/testing/BENCHMARK-METHODOLOGY.md

## The definition

```
goodput = verified original file payload bits / total transfer time
```

Three words carry the weight:

- **verified** — the reconstructed file's hash matches the source. Bits that arrive but
  cannot be shown correct do not count.
- **original** — pre-compression, pre-encoding payload. If we compress, the compression
  gain is reported separately and never folded into channel performance.
- **total transfer time** — wall clock from user-visible start to user-visible
  completion. Includes acquisition, capability negotiation, the tail where the fountain
  decoder is waiting for its last few symbols, and final verification. Not just the
  steady-state streaming window.

That last point is where optimistic numbers usually hide. A system that streams at
300 KB/s for four seconds and then spends three seconds acquiring lock and closing out
the fountain tail has a goodput of about 170 KB/s, not 300 KB/s.

## Why this is stated so aggressively

The screen-camera literature is full of headline numbers that are not comparable to each
other. A concrete, citable example: ChromaCode (MobiCom 2018) reports **raw throughput
777 kbps and data goodput 120 kbps** — a **6.5× gap** between the two figures for the
same system in the same experiment. The authors themselves note that the ultimate
goodput is much smaller than the raw throughput. `[LIT]`

If we quote "777 kbps" as that system's performance, we overstate it by 6.5×. If we then
compare our own goodput against it, we produce a meaningless result and steer the project
toward optimising the wrong quantity.

## The six-rate discipline

Every performance statement, in every document, log line, commit message, chart axis and
verbal update, must identify which of these it refers to:

1. Display refresh rate (Hz)
2. Display state rate `Fd` (distinct presented optical frames per second)
3. Camera capture rate (fps)
4. Raw optical symbol rate (symbols/s)
5. Raw encoded bit rate (bit/s)
6. Corrected decoder bit rate (bit/s)
7. **Payload goodput (KB/s)** — the objective

An unlabelled number is a documentation defect and should be treated like a failing test.

## Things that are not goodput improvements

| Tempting claim | Why it is not goodput |
|---|---|
| "We doubled bits per cell" | Higher-order symbols reduce noise margin; `Rfec` and `Pc` fall. Net effect can be negative. This is why M2/M3 are gated on measurement. |
| "We doubled the refresh rate" | If the camera cannot capture distinct states, `Pc` collapses. `Fd` is bounded by the receiver, not the transmitter. |
| "We increased the grid density" | Beyond the optical resolution limit, crosstalk raises the symbol error rate faster than cell count raises capacity. |
| "We reduced FEC overhead" | Until a burst of errors becomes uncorrectable and the frame is lost entirely. |
| "The file compressed well" | That is a property of the file, not of the optical channel. Report separately (NG8). |
| "Raw bit rate is up 40%" | Irrelevant if error correction and frame loss consume the gain. |

The last row is the project's core discipline: **raw bit rate is irrelevant when error
correction and frame losses eliminate its advantage.**

## Coupled variables

The first-order model multiplies its variables as though they were independent. They are
not. The known couplings, all `[HYP]` until measured:

- `B ↑` → `Rfec ↓`, `Pc ↓` (less margin per symbol)
- `Fd ↑` → `Pc ↓` (more display states per camera frame → more mixing)
- `N ↑` → `Pc ↓` (smaller cells → more crosstalk, more blur sensitivity)
- `Rfec ↑` → frame error rate ↑ (weaker code, more total frame losses)
- distance ↑ → effective `N` limit ↓ and `Pc` ↓ together

Because of these couplings, **every optimisation must be validated by an end-to-end
goodput measurement**, not by improving a component metric. A change that improves symbol
error rate while reducing state rate may well be a net loss.

## Reporting rules

A performance report is only acceptable if it states:

- The metric (from the list above)
- The benchmark category (rigid mount / handheld / stress / cross-device / thermal / large file)
- Device pair, display mode, camera configuration
- Distance, angle, ambient and screen brightness
- File size and file type (and whether compressible)
- Software commit
- Number of runs and the spread, not just the best run

**Best-run numbers are never reported as system performance.** Report the median and the
range. A single lucky run is an anecdote.

## Targets versus results

The milestones (200 KB/s, 500 KB/s, 1 MB/s) are **targets**. Until a benchmark run under
the documented methodology produces them, they are written as targets everywhere,
including informally. Model outputs are labelled as model outputs. This rule has no
exceptions and applies to every document in this repository.

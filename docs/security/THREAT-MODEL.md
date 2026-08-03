# Threat model

> **Status:** Draft
> **Owner:** Security
> **Last reviewed:** 2026-08-02
> **Related:** INPUT-VALIDATION.md, RISK-009, PROTOCOL-SPEC.md

## What we are protecting

1. **The receiving device** from a malicious transmitter. This is the primary concern: the
   receiver parses attacker-controlled data in C++.
2. **The integrity of transferred files** — the user must never receive silently corrupted
   data believing it correct.
3. **User privacy** — camera use, and the inherent broadcast nature of the medium.

## What we are explicitly *not* protecting (initial system)

**Confidentiality is not provided by default.** Anyone with line of sight to the
transmitting screen and an adequate camera receives the same data. This is a property of
the medium, not a defect, and it **must be surfaced to the user** (SEC-07) rather than
left implicit. Optional authenticated encryption is designed for but deferred (SEC-08).

## Trust boundaries

```
┌──────────────────────────────────────────────────┐
│ TRANSMITTER — trusted by its own user only       │
└───────────────────────┬──────────────────────────┘
                        │ light — a fully untrusted channel
                        │ any observer can read; any emitter can inject
┌───────────────────────▼──────────────────────────┐
│ RECEIVER                                         │
│ ┌──────────────────────────────────────────────┐ │
│ │ UNTRUSTED ZONE                               │ │
│ │ camera frames → symbols → packets → blocks   │ │
│ │ Everything here is attacker-controlled       │ │
│ └───────────────────┬──────────────────────────┘ │
│                     │ hash verification gate     │
│ ┌───────────────────▼──────────────────────────┐ │
│ │ TRUSTED ZONE — verified file delivered       │ │
│ └──────────────────────────────────────────────┘ │
└──────────────────────────────────────────────────┘
```

The hash-verification gate is the only boundary that matters. **Nothing crosses it
unverified.**

## Threat actors

| Actor | Capability | Concern |
|---|---|---|
| **Malicious transmitter** | Full control of every transmitted bit; can craft arbitrary headers, lengths, FEC and fountain metadata | **Primary.** Memory corruption, resource exhaustion, path traversal |
| **Passive observer** | Line of sight to the screen | Reads the entire transfer. Inherent to the medium |
| **Active interferer** | Can shine light, obstruct, or display competing patterns | Denial of service; possibly frame injection |
| **Malicious app on the receiver** | Local | Standard Android app isolation applies; out of scope beyond not weakening it |

## Threats and mitigations

### T1 — Memory corruption via crafted protocol fields
**Vector.** Oversized lengths, integer overflow in size arithmetic, negative or wrapped
counts, mismatched declared versus actual lengths.
**Impact.** High — remote code execution in a C++ process.
**Mitigations.** Every field bounds-checked **before** use in allocation or loop bounds;
checked arithmetic for all size computations; no variable-length stack allocation;
fuzzing from Phase 4; ASan/UBSan in CI.
**See.** [INPUT-VALIDATION.md](INPUT-VALIDATION.md)

### T2 — Resource exhaustion
**Vector.** Enormous declared file size, huge block counts, tiny symbol sizes producing
enormous symbol counts, unbounded session duration, forcing unbounded fountain decoder
state.
**Impact.** Medium-High — OOM, storage exhaustion, battery drain, device unusability.
**Mitigations.** Hard caps on file size, block count, block size, symbol size and session
duration; declared size checked against free storage *before* starting; fountain decoder
memory derived from validated parameters and capped; **explicit back-pressure with
deliberate frame dropping rather than unbounded queueing** (RX-05).

### T3 — Path traversal and unsafe file writing
**Vector.** Filenames containing `../`, absolute paths, path separators, null bytes,
control characters, or platform-special names.
**Impact.** High — writing outside the app's storage.
**Mitigations.** Filenames are **sanitised, not merely validated** — the basename is
extracted, separators and control characters stripped, length bounded, and a safe fallback
name used if nothing usable remains. Writes go to an app-private directory via a
randomised temporary name created with `O_EXCL`.

### T4 — Malicious FEC or fountain metadata
**Vector.** Parameters causing pathological decoder behaviour: degenerate matrices,
enormous iteration counts, self-referential block structures.
**Impact.** Medium — CPU exhaustion, hangs.
**Mitigations.** Parameter whitelists rather than range checks where the valid set is
small; hard iteration limits on all decoders; wall-clock budget per frame; fuzzing of the
FEC and fountain parameter surface specifically.

### T5 — Silent corruption delivered as valid
**Vector.** FEC miscorrection producing a valid-looking codeword; hash collision (not
realistic with SHA-256); a bug in reassembly.
**Impact.** **High** — violates the core guarantee G3.
**Mitigations.** CRC above the FEC layer to catch miscorrection; end-to-end SHA-256 as the
final gate; output moved into place only after verification; **hash mismatch is a hard
failure that delivers nothing**.

### T6 — Decompression bomb
**Vector.** If compression is supported, a small compressed payload expanding enormously.
**Impact.** Medium.
**Mitigations.** Compression is deferred (FIL-05). When implemented: declared uncompressed
size validated against the cap before decompression, expansion ratio bounded, and output
size enforced during decompression, not merely checked afterward.

### T7 — Replay
**Vector.** Recording a transmission and replaying it later.
**Impact.** Low for the initial system — the file is not secret to anyone with line of
sight anyway, and there is no authentication to bypass.
**Mitigations.** Session IDs distinguish sessions. Replay becomes meaningful only if
authentication is added (SEC-09), at which point freshness would need designing in.
Documented now so it is not forgotten then.

### T8 — Session confusion
**Vector.** Two transmitters visible simultaneously; a receiver mixing packets from both.
**Impact.** Medium — corruption, or a denial of service.
**Mitigations.** Random 32-bit session ID in every frame header; packets from a
non-matching session are discarded; the receiver locks to one session for the duration.

### T9 — Camera privacy
**Vector.** The receiver's camera is active and pointed at whatever is in front of it.
**Impact.** Medium — privacy expectation.
**Mitigations.** Clear in-app indication when the camera is active (SEC-06); **no frame
retention beyond the session** unless the user explicitly enables capture recording for
debugging; no network transmission of anything, ever (NG7 helps here).

### T10 — Optical eavesdropping
**Vector.** Anyone with line of sight receives the data.
**Impact.** Depends entirely on the file.
**Mitigations.** **Disclosure** (SEC-07) — the user must understand this before
transferring something sensitive. Optional authenticated encryption (SEC-08) is the real
fix and is deferred; the disclosure is not a substitute for it, but it is honest.

## Security requirements summary

| # | Requirement |
|---|---|
| SR-1 | No allocation whose size derives from unvalidated input |
| SR-2 | All size arithmetic uses checked operations |
| SR-3 | Every protocol field has a documented bound, enforced before use |
| SR-4 | No output delivered without hash verification |
| SR-5 | Filenames sanitised, not merely validated |
| SR-6 | Temporary files: app-private, randomised name, `O_EXCL`, cleaned up on all paths |
| SR-7 | All decoders have hard iteration and time limits |
| SR-8 | Protocol parsers fuzzed continuously from Phase 4 |
| SR-9 | ASan and UBSan builds in CI |
| SR-10 | Camera activity clearly indicated; no frame retention by default |
| SR-11 | Broadcast nature of the medium disclosed to the user |

## Review schedule

- After the protocol spec stabilises (Phase 4)
- Before any public release
- Whenever a new parser or externally-influenced allocation is added

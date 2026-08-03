# Project vision

> **Status:** Draft
> **Owner:** Project lead
> **Last reviewed:** 2026-08-02
> **Related:** ADR-0001, ADR-0006, ADR-0012

## The problem

Two people stand next to each other holding smartphones. They want to move a file
between them. Every available path has a cost: Wi-Fi Direct pairing is unreliable and
opaque, Bluetooth is slow, AirDrop-equivalents are platform-locked, NFC is limited to
handshakes, cloud transfer requires connectivity and consent to upload, and cables
require an accessory that neither person has.

There is one channel that is always present, needs no pairing, no radio, no account and
no infrastructure: **one phone's screen pointed at the other phone's camera.**

Today the standard way to use that channel is a stream of QR codes. It works, and it is
extremely slow — QR was designed for one-shot scanning of small payloads by cheap
hardware under adversarial conditions, and every design choice reflects that. Detection
is re-run from scratch on every frame, most of the screen area is spent on quiet zones
and format redundancy, error correction is fixed and generous, and there is no notion of
a persistent link.

## The thesis

A screen-to-camera link is not a barcode-scanning problem. It is a **communications
problem** over a spatial optical channel with tens of thousands of parallel subchannels,
and it should be engineered like one.

Concretely, the bet is that treating the display as a **dense, persistent, tracked,
full-screen symbol matrix** — with pilots, differential modulation, soft decisions,
intra-frame FEC and a cross-frame fountain code — will outperform repeated independent
QR detection by a wide margin.

The specific structural advantages we intend to exploit:

- **Spatial parallelism.** A 144×240 grid is 34,560 simultaneous subchannels. `[HYP]`
- **Persistence.** Once the screen is located, tracking is cheap; re-detection is not. `[HYP]`
- **Soft information.** QR throws away confidence at the threshold step. FEC wants it. `[HYP]`
- **Rateless coding.** Frame loss stops being a retransmission problem. `[HYP]`
- **Differential modulation.** Encoding *changes* rejects fixed channel distortion. `[HYP]`

Each of these is a hypothesis with a linked experiment. None is established.

## What success looks like

A user opens FileFlow on two Android phones, picks a file on one, points the other's
camera at the first phone's screen, props both against something stable, and a
100 MB file arrives verified and intact — with the transfer running fast enough that
the interaction feels like a transfer, not a stunt.

The quantitative form of that: **sustained verified payload goodput above 200 KB/s**
on reference hardware, above **500 KB/s** in a controlled stationary setup, with
correctness guaranteed by an end-to-end cryptographic hash.

## What this project is *not*

It is not a faster QR library. It is not an imperceptible watermarking system. It is not
a browser demo. It is not a machine-learning decoder. Those are documented as prior art
and as possible later experiments, not as the architecture. See
[GOALS-AND-NON-GOALS.md](GOALS-AND-NON-GOALS.md).

## Why the target is plausible — and where it is strained

The strongest published *imperceptible* screen-camera systems operate around
**120 kbps goodput** (ChromaCode, MobiCom 2018 — raw throughput 777 kbps, goodput
120 kbps, BER 0.05) `[LIT]`. That is roughly **15 KB/s**. Our milestone-4 target of
200 KB/s is about **13× that goodput**, and about **2× ChromaCode's raw rate**.

That gap is not closed by cleverness alone. It is closed primarily by **abandoning
imperceptibility**, which is the single largest constraint in that literature. Those
systems spend nearly all of their channel budget hiding the signal inside cover video.
FileFlow spends none: the screen is a dedicated transmitter and is allowed to look like
whatever decodes best.

Even so, the first-order model says the target is tight. Binary luminance at 60 display
states per second does not reach 200 KB/s on any candidate grid — the model puts the
optimistic binary case at roughly 145 KB/s. Reaching milestone 4 requires **either**
four-level modulation **or** more than 60 distinct display states per second. See
[PERFORMANCE-MODEL.md](../specifications/PERFORMANCE-MODEL.md). `[HYP]`

And there is a hard platform constraint on the "more states per second" path: Android's
constrained high-speed capture sessions (the ≥120 fps path) **cannot deliver frames to
an ImageReader at all**. `[FACT]` — see
[android-camera-pipeline.md](../research/android-camera-pipeline.md). That does not kill
the approach, but it forces the high-frame-rate receiver onto a GPU texture path rather
than a CPU YUV path, and it changes what "low-copy YUV processing" means in practice.

Stating this plainly at the start is deliberate. The project's credibility depends on
the difference between a target and a result never blurring.

## Principles

1. **Goodput is the metric.** Not symbol rate. Not refresh rate. Not raw bits.
2. **Measure before optimising.** The simulator and the channel characterisation phases
   exist so that optimisation targets are chosen from data.
3. **Model, then verify, then replace the model.** The first-order model is scaffolding
   with non-independent variables. Measured channel data replaces it.
4. **Reference devices first.** Breadth of device support is a later problem; it trades
   directly against the depth needed to learn anything.
5. **Negative results are results.** The fastest way to a real 200 KB/s is to find out
   quickly which of our hypotheses are wrong.

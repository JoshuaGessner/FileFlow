# Input validation rules

> **Status:** Draft
> **Owner:** Security
> **Last reviewed:** 2026-08-02
> **Related:** THREAT-MODEL.md, PROTOCOL-SPEC.md, RISK-009

**Every byte arriving from the camera is attacker-controlled.** Read this before writing
any parser.

## The core rule

> **Validate before you allocate. Validate before you loop. Validate before you index.**

A bound checked *after* the allocation is not a bound. A length used to size a buffer must
be validated in the same expression that produces it, or earlier.

## Bounds table

Every externally-influenced quantity has a hard bound, enforced at parse time. These are
provisional values to be refined, but **no field may exist without a bound**.

| Field | Bound | Rationale |
|---|---|---|
| File size | ≤ 4 GiB, **and** ≤ (free storage − margin) | Prevents storage exhaustion; checked before transfer starts |
| File name length | ≤ 255 bytes | Filesystem limit |
| File name content | Sanitised — see below | Path traversal |
| Block count | ≤ 2²⁴ | Bounds decoder bookkeeping |
| Block size | 1 KiB … 1 MiB | Bounds decoder working set |
| Symbol size | 64 B … 64 KiB | Bounds per-symbol handling |
| block_count × block_size | Checked for overflow, ≤ file size bound | The classic multiplication overflow |
| Payload length per frame | ≤ cells available under the active grid | Cannot exceed physical capacity |
| Extension field length | ≤ 4 KiB each; ≤ 16 KiB total | Bounds TLV parsing |
| Extension count | ≤ 64 | Prevents pathological TLV chains |
| Session duration | ≤ 30 minutes | Bounds resource use |
| Frame count per session | ≤ 2³² | Sequence-number space |
| FEC iterations | ≤ 50 (code-dependent) | Prevents CPU exhaustion |
| Fountain symbols ingested | ≤ 3 × block symbol count | Prevents unbounded repair ingestion |
| Decoder wall-clock per frame | ≤ frame period × 2 | Hard timeout |

## Integer arithmetic

**All size and offset arithmetic uses checked operations.** No exceptions.

```cpp
// WRONG — overflows silently, allocation is tiny, writes are not
size_t total = block_count * block_size;
auto buf = allocate(total);

// RIGHT — validate inputs, then use checked multiplication
if (block_count > kMaxBlockCount) return Err::BlockCountOutOfRange;
if (block_size < kMinBlockSize || block_size > kMaxBlockSize)
    return Err::BlockSizeOutOfRange;

size_t total;
if (__builtin_mul_overflow(block_count, block_size, &total))
    return Err::SizeOverflow;
if (total > kMaxTotalBytes) return Err::TotalTooLarge;

auto buf = allocate(total);
```

Rules:
- Use `__builtin_*_overflow` or an equivalent checked-arithmetic wrapper.
- **Never** cast a signed value to unsigned without a range check first — a negative
  becoming a huge unsigned is a classic path to a heap overflow.
- Prefer `size_t` for sizes, but validate that a wire value fits before converting.
- No variable-length arrays and no `alloca` on any path reachable from parsed input.

## Filename handling

Filenames are **sanitised, not validated**. Validation rejects; sanitisation produces
something safe. Rejecting is fine too, but we must never pass through.

```
1. Take the basename only — discard everything up to and including the last separator.
2. Reject or strip: path separators ('/', '\'), null bytes, control characters (< 0x20),
   leading dots, and any sequence of dots that is the entire name.
3. Reject platform-reserved names.
4. Truncate to the length bound, preserving the extension if possible.
5. If nothing usable remains, use a generated safe name.
6. NEVER use the received name to construct a path by concatenation. Open relative to a
   directory file descriptor for the app-private destination.
```

The received filename is a **hint for display**, not a path. Treat it accordingly.

## Temporary files

```
1. App-private directory only. Never a shared or world-writable location.
2. Randomised name from a CSPRNG.
3. Created with O_CREAT | O_EXCL — never open an existing path.
4. Never follow symlinks (O_NOFOLLOW where applicable).
5. Cleaned up on every exit path: success, failure, cancellation, exception, crash
   recovery on next launch.
6. The final file is moved into place ONLY after hash verification succeeds.
```

## Parser discipline

| Rule | Why |
|---|---|
| Parse into a validated struct in one place, then use only the struct | Prevents re-parsing raw bytes at scattered call sites with inconsistent checks |
| Never trust a length field against the actual buffer — check both | Declared and actual length disagreeing is a classic vector |
| Bounds-check every index derived from parsed data | |
| Unknown extension types are **skipped by length**, not rejected | Forward compatibility (PROTOCOL-SPEC) — but the length itself is still bounds-checked first |
| Reserved fields transmitted as zero, **not validated as zero** on receive | Otherwise future use breaks old receivers |
| Fail closed | An unparseable frame is discarded, never partially applied |
| No partial state mutation before full validation | Prevents a rejected frame leaving inconsistent state |

## Decoder hardening

FEC and fountain decoders are attacker-influenced through their parameters:

- Hard iteration limits, always. An iterative decoder with attacker-influenced input needs
  a ceiling.
- Wall-clock budget per frame; exceeding it discards the frame.
- Parameter **whitelists** rather than range checks where the valid set is small — a
  whitelist of supported code rates is safer than a range.
- Fountain decoder memory computed from already-validated block parameters, capped
  independently as a second line of defence.
- Degenerate inputs (zero symbols, duplicate symbol IDs, self-referential structures) must
  be handled, and are prime fuzzing targets.

## Testing requirements

| Requirement | Detail |
|---|---|
| Malformed-input vectors | Committed alongside valid vectors (C18); every bound has a vector that violates it |
| Continuous fuzzing | From Phase 4, on every parser: frame header, session header, TLV extensions, FEC parameters, fountain metadata |
| Sanitiser builds in CI | ASan and UBSan, on the desktop build — which exists precisely because ADR-0010 requires it |
| Corruption detection test | Injected corruption must **always** be caught by the hash gate. This test asserts G3 |
| Resource-limit tests | Each cap has a test that hits it and confirms clean rejection rather than degradation |

The desktop build (ADR-0010) is what makes fuzzing and sanitiser coverage practical — a
useful second benefit of a decision made for testing reasons.

## Review checklist

Before merging any code that touches parsed input:

- [ ] Every field bounds-checked before use
- [ ] All size arithmetic uses checked operations
- [ ] No allocation sized by unvalidated input
- [ ] No signed/unsigned conversion without a range check
- [ ] No VLA or `alloca` reachable from parsed input
- [ ] Failure paths leave no partial state
- [ ] Malformed-input vectors added for the new fields
- [ ] Fuzz target updated to cover the new surface

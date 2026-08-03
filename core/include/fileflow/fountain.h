// LT fountain code — the cross-frame erasure layer (ADR-0009).
//
// Why rateless: frame loss is a NORMAL event on this channel, and there is no reverse
// channel (NG9) to request retransmission. A fountain code makes loss a non-event -- the
// transmitter emits repair symbols indefinitely and the receiver signals completion by
// having completed.
//
// Why LT and not RaptorQ: RaptorQ (RFC 6330) has materially better reception overhead, but
// Qualcomm's IETF IPR declaration #1958 offers non-assert only for devices that do NOT
// implement a wireless wide-area standard -- and we target smartphones, which do. That is
// unresolved (RISK-016, OQ-010, RT-07) and RaptorQ must not become a dependency before
// legal review. LT is the fallback with a known cost, measured by EXP-012.
//
// SYSTEMATIC: source symbols are emitted first, so a clean channel decodes with almost no
// fountain work at all. Only a degraded channel pays the peeling cost.
#pragma once

#include <fileflow/result.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace fileflow {

// Deterministic PRNG. The decoder must regenerate each symbol's neighbour set from its
// encoding symbol id alone, so this must produce identical output on every platform --
// which rules out std::mt19937 seeded per-symbol (slow) and anything libstdc++/libc++
// might implement differently. splitmix64 is fixed by its constants.
class SplitMix64 {
  public:
    explicit SplitMix64(std::uint64_t seed) noexcept : s_(seed) {}

    std::uint64_t Next() noexcept {
        s_ += 0x9E3779B97F4A7C15ULL;
        std::uint64_t z = s_;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }

    // Uniform in [0, n). Rejection-sampled to avoid modulo bias.
    std::uint32_t Below(std::uint32_t n) noexcept {
        if (n <= 1) return 0;
        const std::uint64_t limit = UINT64_MAX - (UINT64_MAX % n);
        std::uint64_t r;
        do {
            r = Next();
        } while (r >= limit);
        return static_cast<std::uint32_t>(r % n);
    }

    double NextDouble() noexcept {
        return static_cast<double>(Next() >> 11) * (1.0 / 9007199254740992.0);
    }

  private:
    std::uint64_t s_;
};

struct FountainParams {
    std::uint32_t source_symbols = 0;  // k
    std::uint32_t symbol_size = 0;     // bytes per symbol
    std::uint32_t session_seed = 0;    // mixes into every ESI so sessions differ

    // Robust soliton tuning. c ~ 0.03..0.2, delta = failure probability target.
    double c = 0.05;
    double delta = 0.05;

    // Hard bounds enforced before ANY allocation (docs/security/INPUT-VALIDATION.md).
    static constexpr std::uint32_t kMaxSourceSymbols = 1u << 20;
    static constexpr std::uint32_t kMaxSymbolSize = 64u * 1024u;

    [[nodiscard]] Status Validate() const noexcept;
};

// The degree distribution and neighbour selection are shared by encoder and decoder --
// they MUST agree exactly or nothing decodes. Kept in one place for that reason.
class LtDegreeTable {
  public:
    explicit LtDegreeTable(const FountainParams& p);

    // Neighbour set for an encoding symbol id, written into `out` (cleared first).
    void Neighbours(std::uint32_t esi, std::vector<std::uint32_t>* out) const;

  private:
    [[nodiscard]] std::uint32_t SampleDegree(SplitMix64* rng) const;

    FountainParams p_;
    std::vector<double> cdf_;  // cumulative robust soliton distribution
};

class FountainEncoder {
  public:
    // `source` is padded to k * symbol_size internally; the caller keeps ownership of
    // nothing -- symbols are copied into the encoder.
    static Result<FountainEncoder> Create(const FountainParams& p,
                                          std::span<const std::uint8_t> source);

    // Symbols 0..k-1 are the source symbols verbatim (systematic). Beyond that, repair.
    [[nodiscard]] std::vector<std::uint8_t> Symbol(std::uint32_t esi) const;

    [[nodiscard]] const FountainParams& params() const noexcept { return p_; }

  private:
    FountainEncoder(const FountainParams& p, std::vector<std::uint8_t> src)
        : p_(p), table_(p), src_(std::move(src)) {}

    FountainParams p_;
    LtDegreeTable table_;
    std::vector<std::uint8_t> src_;  // k * symbol_size, zero-padded
};

class FountainDecoder {
  public:
    static Result<FountainDecoder> Create(const FountainParams& p);

    // Ingest one received symbol. Duplicates and out-of-order arrivals are tolerated by
    // construction (PROTOCOL-SPEC). Returns true once the block is fully recovered.
    bool Ingest(std::uint32_t esi, std::span<const std::uint8_t> data);

    [[nodiscard]] bool complete() const noexcept { return recovered_ == p_.source_symbols; }
    [[nodiscard]] std::uint32_t recovered() const noexcept { return recovered_; }
    [[nodiscard]] std::uint32_t ingested() const noexcept { return ingested_; }

    // Reception overhead: symbols consumed beyond k, as a fraction of k. This is the
    // Rfountain term in the goodput model, and EXP-012 measures it.
    [[nodiscard]] double overhead() const noexcept;

    // Valid only when complete(). Returns k * symbol_size bytes (still padded).
    [[nodiscard]] Result<std::vector<std::uint8_t>> Take();

  private:
    struct Pending {
        std::vector<std::uint32_t> unknown;  // source indices not yet resolved
        std::vector<std::uint8_t> data;      // XOR of all resolved neighbours removed
    };

    void Reduce(Pending* p);
    void Peel(std::uint32_t newly_known);

    explicit FountainDecoder(const FountainParams& p);

    FountainParams p_;
    LtDegreeTable table_;
    std::vector<std::uint8_t> out_;     // k * symbol_size
    std::vector<bool> known_;           // per source symbol
    std::vector<Pending> pending_;      // symbols with >1 unknown neighbour
    std::vector<std::vector<std::size_t>> waiting_;  // source idx -> pending indices
    std::uint32_t recovered_ = 0;
    std::uint32_t ingested_ = 0;
};

}  // namespace fileflow

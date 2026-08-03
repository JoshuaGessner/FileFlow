// Intra-frame forward error correction (component C11, the FEC half of ADR-0009).
//
// ADR-0009 specifies intra-frame FEC *plus* cross-frame fountain coding. The fountain half has
// worked since Phase 1; this is the half that was missing, and its absence was expensive: a
// frame with a handful of unreadable cells failed its CRC and was discarded ENTIRELY, throwing
// away thousands of good cells to lose a few bad ones. The fountain then had to spend a whole
// extra frame recovering what a few parity bytes could have fixed in place.
//
// TWO DESIGN CHOICES CARRY THIS LAYER:
//
// 1. ERASURE DECODING. The receiver already knows which cells it could not read -- that is
//    what NaN samples, SoftSymbol::erased and the photometric residual check are all for. Reed
//    -Solomon corrects `nsym` erasures but only `nsym/2` errors, so supplying those positions
//    doubles the correction power at zero cost in parity. Every stage upstream has been
//    preserving that information; this is where it finally gets spent.
//
// 2. INTERLEAVING. Optical damage is spatially clustered -- a glare spot, a fingerprint, a
//    scratch, the transition band of a rolling-shutter mixture. Without interleaving such a
//    burst lands inside one codeword and exceeds its budget while neighbouring codewords sit
//    idle. Interleaving scatters consecutive payload bytes across different codewords, turning
//    one fatal burst into a survivable scatter in each. This is the difference between a code
//    that works on paper and one that works on a real screen.
#pragma once

#include <fileflow/gf256.h>
#include <fileflow/result.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace fileflow {

struct IntraFecParams {
    // Parity bytes per Reed-Solomon codeword. Sets the code rate and the correction budget:
    // `nsym` erasures, or `nsym/2` errors, or any mix with 2*errors + erasures <= nsym.
    //
    // 32 is a starting point, not a tuned value -- EXP-011 selects it from measured cell
    // erasure rates. Raising it costs payload capacity directly.
    std::size_t nsym = 32;

    // Bytes per codeword including parity. 255 is the natural maximum over GF(256).
    std::size_t codeword_bytes = 255;

    [[nodiscard]] Status Validate() const noexcept;
    [[nodiscard]] std::size_t message_bytes_per_codeword() const noexcept {
        return codeword_bytes - nsym;
    }
    [[nodiscard]] double code_rate() const noexcept {
        return static_cast<double>(message_bytes_per_codeword()) /
               static_cast<double>(codeword_bytes);
    }
};

// Encodes and decodes one optical frame's payload as a set of interleaved RS codewords.
class IntraFec {
  public:
    // `frame_capacity_bytes` is what the optical frame can carry (M0Modulator's payload
    // capacity). The usable message shrinks by the parity overhead.
    static Result<IntraFec> Create(std::size_t frame_capacity_bytes, IntraFecParams p = {});

    // Payload bytes available to the fountain layer after parity.
    [[nodiscard]] std::size_t message_bytes() const noexcept { return message_bytes_; }
    [[nodiscard]] std::size_t coded_bytes() const noexcept { return coded_bytes_; }
    [[nodiscard]] std::size_t codewords() const noexcept { return codewords_; }
    [[nodiscard]] const IntraFecParams& params() const noexcept { return p_; }

    // Returns exactly coded_bytes(), ready to render into the frame.
    [[nodiscard]] Result<std::vector<std::uint8_t>> Encode(
        std::span<const std::uint8_t> message) const;

    // How hard the code had to work on one frame. The adaptive link controller needs
    // HEADROOM, not just success: a frame that corrected 31 of 32 erasures decoded fine and
    // is one bad cell from failing. Success alone cannot distinguish comfort from luck.
    struct Stats {
        std::size_t codewords_decoded = 0;
        std::size_t symbols_corrected = 0;
        // Worst per-codeword erasure load. Compare against params().nsym for headroom.
        //
        // Populated across ALL codewords even when the frame is uncorrectable -- the failing
        // codeword's own load is included, which it was not in the first version (F21).
        std::size_t worst_erasures_in_codeword = 0;

        // Worst per-codeword CORRECTION BUDGET consumed, in parity bytes: `2*errors + erasures`.
        //
        // This -- not the erasure load -- is what the code actually spends, and it is the
        // sufficient statistic the adaptive link controller runs on. A codeword decodes at
        // `nsym = n` exactly when `2*errors + erasures <= n`, and neither term depends on `n`,
        // so one run at one rung scores the whole ladder (link.h, finding F20).
        //
        // Erasures alone are NOT sufficient, and measurably so: on the impaired channel of F18
        // the worst erasure load was 9 at every rung, yet 11 frames still failed at `nsym = 16`.
        // Undetected errors cost double, and ignoring them makes every prediction optimistic
        // (finding F23).
        //
        // Errors are recovered as `corrected - erasures`: `ReedSolomon::Decode` returns the
        // total number of symbols it changed, and a verified correction accounts for exactly the
        // declared erasures plus the errors its locator found.
        std::size_t worst_budget_used = 0;

        // True when at least one codeword could not be decoded, so its budget could not be
        // measured -- only bounded below by `nsym + 1`. `worst_budget_used` is then a FLOOR,
        // not a measurement, and predictions for rungs above the current one are optimistic.
        // Surfaced rather than hidden because a censored estimate must not be reported with the
        // same confidence as a complete one.
        bool budget_censored = false;

        [[nodiscard]] double budget_used(std::size_t nsym) const noexcept {
            return nsym == 0 ? 0.0
                             : static_cast<double>(worst_erasures_in_codeword) /
                                   static_cast<double>(nsym);
        }
    };

    // `erased_positions` are indices into `coded` that the demodulator could not read.
    // Returns exactly message_bytes(), or kUncorrectable if any codeword exceeds its budget.
    [[nodiscard]] Result<std::vector<std::uint8_t>> Decode(
        std::span<const std::uint8_t> coded,
        std::span<const std::size_t> erased_positions = {}, Stats* stats = nullptr) const;

    // Where byte `i` of the interleaved stream lives in codeword-local terms. Exposed so
    // tests can verify the interleave actually scatters bursts rather than assuming it.
    [[nodiscard]] std::size_t CodewordOf(std::size_t interleaved_index) const noexcept {
        return interleaved_index % codewords_;
    }

  private:
    IntraFec(IntraFecParams p, std::size_t codewords, std::size_t message_bytes,
             std::size_t coded_bytes)
        : p_(p),
          rs_(p.nsym),
          codewords_(codewords),
          message_bytes_(message_bytes),
          coded_bytes_(coded_bytes) {}

    IntraFecParams p_;
    ReedSolomon rs_;
    std::size_t codewords_;
    std::size_t message_bytes_;
    std::size_t coded_bytes_;
};

}  // namespace fileflow

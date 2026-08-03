// GF(2^8) arithmetic and systematic Reed-Solomon.
//
// RS is used for the frame HEADER (docs/research/coding-theory.md): short, symbol-oriented,
// excellent burst tolerance, and it must decode reliably or the whole frame is lost.
// `H` (header success probability) appears directly in the goodput model.
//
// The PAYLOAD code is a separate decision pending EXP-011 — RS is hard-decision and
// discards the demodulator's soft information, which is why it is not the payload choice.
#pragma once

#include <fileflow/result.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace fileflow::gf {

// Primitive polynomial x^8 + x^4 + x^3 + x^2 + 1 (0x11D), generator 2.
// Standard choice, shared with QR/DVB, so tables are cross-checkable against references.
inline constexpr std::uint16_t kPrimitive = 0x11D;

struct Tables {
    std::array<std::uint8_t, 512> exp{};
    std::array<std::uint8_t, 256> log{};
    Tables() noexcept;
};

const Tables& T() noexcept;

[[nodiscard]] inline std::uint8_t Add(std::uint8_t a, std::uint8_t b) noexcept { return a ^ b; }

[[nodiscard]] inline std::uint8_t Mul(std::uint8_t a, std::uint8_t b) noexcept {
    if (a == 0 || b == 0) return 0;
    const auto& t = T();
    return t.exp[static_cast<std::size_t>(t.log[a]) + static_cast<std::size_t>(t.log[b])];
}

[[nodiscard]] inline std::uint8_t Div(std::uint8_t a, std::uint8_t b) noexcept {
    if (a == 0) return 0;
    if (b == 0) return 0;  // caller must avoid; returning 0 keeps this noexcept and total
    const auto& t = T();
    const int d = static_cast<int>(t.log[a]) - static_cast<int>(t.log[b]) + 255;
    return t.exp[static_cast<std::size_t>(d)];
}

[[nodiscard]] inline std::uint8_t Pow(std::uint8_t a, int n) noexcept {
    if (a == 0) return 0;
    const auto& t = T();
    int e = (static_cast<int>(t.log[a]) * n) % 255;
    if (e < 0) e += 255;
    return t.exp[static_cast<std::size_t>(e)];
}

[[nodiscard]] inline std::uint8_t Inv(std::uint8_t a) noexcept {
    const auto& t = T();
    return a == 0 ? 0 : t.exp[255 - static_cast<std::size_t>(t.log[a])];
}

}  // namespace fileflow::gf

namespace fileflow {

// Systematic RS(n, k) over GF(256): output is the k message symbols followed by
// n-k parity symbols. Corrects up to (n-k)/2 symbol errors.
class ReedSolomon {
  public:
    // nsym = number of parity symbols. Message length is bounded so n <= 255.
    explicit ReedSolomon(std::size_t nsym);

    [[nodiscard]] std::size_t parity_symbols() const noexcept { return nsym_; }
    [[nodiscard]] std::size_t max_message() const noexcept { return 255 - nsym_; }
    [[nodiscard]] std::size_t correctable() const noexcept { return nsym_ / 2; }

    // Returns message || parity.
    [[nodiscard]] Result<std::vector<std::uint8_t>> Encode(
        std::span<const std::uint8_t> message) const;

    // Erasures are worth TWICE what errors are worth.
    //
    // An error is a symbol that is wrong at an unknown position: the decoder must spend
    // syndrome budget finding *where* it is and *what* it should be. An erasure is a symbol
    // known to be untrustworthy: the position is free, so only the value must be solved. Hence
    // the classic bound  2*errors + erasures <= nsym  -- with positions supplied, the code
    // corrects nsym erasures instead of nsym/2 errors.
    //
    // This matters enormously here because the receiver ALREADY KNOWS which cells it could not
    // read. NaN samples, SoftSymbol::erased and the photometric residual check all exist to
    // identify untrustworthy cells rather than guess at them, and that information was being
    // discarded at the last step. Passing it through doubles the correction power for free.
    [[nodiscard]] std::size_t correctable_erasures() const noexcept { return nsym_; }

    // In-place correction of a full codeword (message || parity).
    //
    // `erasure_positions` are indices into `codeword` known to be unreliable; their current
    // contents are ignored. Returns the number of symbols corrected, or kUncorrectable when
    // 2*errors + erasures exceeds nsym.
    [[nodiscard]] Result<std::size_t> Decode(
        std::span<std::uint8_t> codeword,
        std::span<const std::size_t> erasure_positions = {}) const;

  private:
    [[nodiscard]] std::vector<std::uint8_t> Syndromes(std::span<const std::uint8_t> cw) const;

    std::size_t nsym_;
    std::vector<std::uint8_t> gen_;  // generator polynomial
};

}  // namespace fileflow

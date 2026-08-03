// CRC-32 (IEEE) and SHA-256.
//
// CRC-32 protects frame and session headers, and sits ABOVE the FEC layer to catch
// miscorrection — a code can decode "successfully" to the wrong codeword (THREAT-MODEL T5).
// SHA-256 is the end-to-end integrity anchor (G3): nothing is delivered without it.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace fileflow {

[[nodiscard]] std::uint32_t Crc32(std::span<const std::uint8_t> data,
                                  std::uint32_t seed = 0) noexcept;

class Sha256 {
  public:
    static constexpr std::size_t kDigestSize = 32;
    using Digest = std::array<std::uint8_t, kDigestSize>;

    Sha256() noexcept { Reset(); }
    void Reset() noexcept;
    void Update(std::span<const std::uint8_t> data) noexcept;
    [[nodiscard]] Digest Finish() noexcept;

    // One-shot convenience.
    [[nodiscard]] static Digest Of(std::span<const std::uint8_t> data) noexcept;

  private:
    void Compress(const std::uint8_t* block) noexcept;

    std::array<std::uint32_t, 8> h_{};
    std::array<std::uint8_t, 64> buf_{};
    std::size_t buf_len_ = 0;
    std::uint64_t total_bits_ = 0;
};

[[nodiscard]] std::string ToHex(std::span<const std::uint8_t> data);

}  // namespace fileflow

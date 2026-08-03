// Bounds-checked byte reader/writer.
//
// THIS IS THE ATTACKER-FACING SURFACE. Every byte that reaches a ByteReader came off the
// camera and is fully attacker-controlled (docs/security/THREAT-MODEL.md, T1).
//
// The project uses plain C++20 with no memory-safety hedge, so this type carries real
// weight: protocol parsers must go through it and must never index a raw buffer directly.
// Every read is bounds-checked; every size computation is checked for overflow.
#pragma once

#include <fileflow/result.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace fileflow {

// Checked multiply. Returns false on overflow rather than wrapping.
[[nodiscard]] inline bool CheckedMul(std::size_t a, std::size_t b, std::size_t* out) noexcept {
#if defined(__has_builtin)
#if __has_builtin(__builtin_mul_overflow)
    return !__builtin_mul_overflow(a, b, out);
#endif
#endif
    if (a != 0 && b > SIZE_MAX / a) return false;
    *out = a * b;
    return true;
}

[[nodiscard]] inline bool CheckedAdd(std::size_t a, std::size_t b, std::size_t* out) noexcept {
#if defined(__has_builtin)
#if __has_builtin(__builtin_add_overflow)
    return !__builtin_add_overflow(a, b, out);
#endif
#endif
    if (b > SIZE_MAX - a) return false;
    *out = a + b;
    return true;
}

// Big-endian on the wire: the optical channel is a network, and network byte order keeps
// hex dumps of captured frames readable.
class ByteReader {
  public:
    explicit ByteReader(std::span<const std::uint8_t> data) noexcept : d_(data) {}

    [[nodiscard]] std::size_t remaining() const noexcept { return d_.size() - pos_; }
    [[nodiscard]] std::size_t position() const noexcept { return pos_; }
    [[nodiscard]] bool empty() const noexcept { return remaining() == 0; }

    Result<std::uint8_t> U8() noexcept {
        if (remaining() < 1) return Error::kTruncated;
        return d_[pos_++];
    }

    Result<std::uint16_t> U16() noexcept {
        if (remaining() < 2) return Error::kTruncated;
        auto v = static_cast<std::uint16_t>((static_cast<std::uint16_t>(d_[pos_]) << 8) |
                                            static_cast<std::uint16_t>(d_[pos_ + 1]));
        pos_ += 2;
        return v;
    }

    Result<std::uint32_t> U24() noexcept {
        if (remaining() < 3) return Error::kTruncated;
        std::uint32_t v = (static_cast<std::uint32_t>(d_[pos_]) << 16) |
                          (static_cast<std::uint32_t>(d_[pos_ + 1]) << 8) |
                          static_cast<std::uint32_t>(d_[pos_ + 2]);
        pos_ += 3;
        return v;
    }

    Result<std::uint32_t> U32() noexcept {
        if (remaining() < 4) return Error::kTruncated;
        std::uint32_t v = (static_cast<std::uint32_t>(d_[pos_]) << 24) |
                          (static_cast<std::uint32_t>(d_[pos_ + 1]) << 16) |
                          (static_cast<std::uint32_t>(d_[pos_ + 2]) << 8) |
                          static_cast<std::uint32_t>(d_[pos_ + 3]);
        pos_ += 4;
        return v;
    }

    Result<std::uint64_t> U64() noexcept {
        if (remaining() < 8) return Error::kTruncated;
        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v = (v << 8) | d_[pos_ + static_cast<std::size_t>(i)];
        pos_ += 8;
        return v;
    }

    // Borrowed view — no copy, no allocation. Bounds-checked.
    Result<std::span<const std::uint8_t>> Bytes(std::size_t n) noexcept {
        if (remaining() < n) return Error::kTruncated;
        auto s = d_.subspan(pos_, n);
        pos_ += n;
        return s;
    }

    // Skip an unknown TLV extension by its length (PROTOCOL-SPEC: skip, never reject).
    Status Skip(std::size_t n) noexcept {
        if (remaining() < n) return Error::kTruncated;
        pos_ += n;
        return Status::Ok();
    }

  private:
    std::span<const std::uint8_t> d_;
    std::size_t pos_ = 0;
};

class ByteWriter {
  public:
    ByteWriter() = default;
    explicit ByteWriter(std::size_t reserve) { buf_.reserve(reserve); }

    void U8(std::uint8_t v) { buf_.push_back(v); }
    void U16(std::uint16_t v) {
        buf_.push_back(static_cast<std::uint8_t>(v >> 8));
        buf_.push_back(static_cast<std::uint8_t>(v & 0xFF));
    }
    void U24(std::uint32_t v) {
        buf_.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFF));
        buf_.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
        buf_.push_back(static_cast<std::uint8_t>(v & 0xFF));
    }
    void U32(std::uint32_t v) {
        buf_.push_back(static_cast<std::uint8_t>(v >> 24));
        buf_.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFF));
        buf_.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
        buf_.push_back(static_cast<std::uint8_t>(v & 0xFF));
    }
    void U64(std::uint64_t v) {
        for (int i = 7; i >= 0; --i)
            buf_.push_back(static_cast<std::uint8_t>((v >> (i * 8)) & 0xFF));
    }
    void Bytes(std::span<const std::uint8_t> s) { buf_.insert(buf_.end(), s.begin(), s.end()); }

    [[nodiscard]] const std::vector<std::uint8_t>& data() const noexcept { return buf_; }
    [[nodiscard]] std::vector<std::uint8_t> take() && noexcept { return std::move(buf_); }
    [[nodiscard]] std::size_t size() const noexcept { return buf_.size(); }

  private:
    std::vector<std::uint8_t> buf_;
};

}  // namespace fileflow

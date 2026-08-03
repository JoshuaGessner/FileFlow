// Result<T, E> — error handling without exceptions.
//
// ADR-0013: no exceptions cross the JNI boundary, and C++23's std::expected is not
// available in NDK libc++. This is the minimal stand-in.
#pragma once

#include <cstdint>
#include <string_view>
#include <utility>
#include <variant>

namespace fileflow {

// Every failure mode reachable from untrusted optical input.
// See docs/security/INPUT-VALIDATION.md — each bound has a distinct code so that a
// fuzzer crash or a field report says *which* limit was hit.
enum class Error : std::uint16_t {
    kNone = 0,

    // --- Bounds and arithmetic (docs/security/INPUT-VALIDATION.md) ---
    kTruncated,           // reader ran off the end of the buffer
    kSizeOverflow,        // checked arithmetic overflowed
    kValueOutOfRange,     // field outside its documented bound
    kLengthMismatch,      // declared length disagrees with actual
    kTooManyExtensions,

    // --- Frame layer ---
    kBadMagic,
    kUnsupportedVersion,
    kHeaderCrcMismatch,
    kUnknownProfile,
    kGridMismatch,

    // --- Coding ---
    kUncorrectable,       // FEC could not correct; frame becomes an erasure
    kFountainIncomplete,
    kDegenerateParameters,
    kIterationLimit,

    // --- Geometry ---
    kMarkersNotFound,
    kDegenerateHomography,

    // --- File layer ---
    kHashMismatch,        // hard failure; nothing is delivered
    kPayloadTooLarge,
    kIoError,

    kInternal,
};

std::string_view ErrorName(Error e) noexcept;

template <typename T>
class Result {
  public:
    // NOLINTNEXTLINE(google-explicit-constructor) — implicit success is the ergonomic point
    Result(T value) : v_(std::move(value)) {}
    // NOLINTNEXTLINE(google-explicit-constructor)
    Result(Error e) : v_(e) {}

    [[nodiscard]] bool ok() const noexcept { return std::holds_alternative<T>(v_); }
    explicit operator bool() const noexcept { return ok(); }

    [[nodiscard]] Error error() const noexcept {
        return ok() ? Error::kNone : std::get<Error>(v_);
    }

    // Precondition: ok(). Callers must check first; debug builds trap via std::get.
    [[nodiscard]] const T& value() const& { return std::get<T>(v_); }
    [[nodiscard]] T& value() & { return std::get<T>(v_); }
    [[nodiscard]] T&& value() && { return std::get<T>(std::move(v_)); }

    [[nodiscard]] T value_or(T fallback) const& {
        return ok() ? std::get<T>(v_) : std::move(fallback);
    }

  private:
    std::variant<T, Error> v_;
};

// Void-returning fallible operations.
class Status {
  public:
    Status() = default;
    // NOLINTNEXTLINE(google-explicit-constructor)
    Status(Error e) : e_(e) {}

    [[nodiscard]] bool ok() const noexcept { return e_ == Error::kNone; }
    explicit operator bool() const noexcept { return ok(); }
    [[nodiscard]] Error error() const noexcept { return e_; }

    static Status Ok() noexcept { return {}; }

  private:
    Error e_ = Error::kNone;
};

// Propagate a failure out of the current function. Deliberately a macro: there is no
// clean way to early-return from an expression in C++20 without one.
#define FF_TRY(expr)                             \
    do {                                         \
        ::fileflow::Status _ff_s = (expr);       \
        if (!_ff_s.ok()) return _ff_s.error();   \
    } while (0)

// Token pasting needs two levels of indirection: `_ff_r_##__LINE__` pastes BEFORE __LINE__
// expands, producing the literal identifier `_ff_r___LINE__` in every expansion. That made
// the macro collide with itself the first time it was used twice in one scope -- it appeared
// to work only because nothing had done that yet (finding F11).
#define FF_CONCAT_INNER(a, b) a##b
#define FF_CONCAT(a, b) FF_CONCAT_INNER(a, b)

#define FF_ASSIGN_OR_RETURN(decl, expr)                              \
    auto FF_CONCAT(_ff_r_, __LINE__) = (expr);                       \
    if (!FF_CONCAT(_ff_r_, __LINE__).ok())                           \
        return FF_CONCAT(_ff_r_, __LINE__).error();                  \
    decl = std::move(FF_CONCAT(_ff_r_, __LINE__)).value()

}  // namespace fileflow

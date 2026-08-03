#include <fileflow/gf256.h>

#include <algorithm>

namespace fileflow::gf {

Tables::Tables() noexcept {
    std::uint16_t x = 1;
    for (std::size_t i = 0; i < 255; ++i) {
        exp[i] = static_cast<std::uint8_t>(x);
        log[static_cast<std::size_t>(x)] = static_cast<std::uint8_t>(i);
        // Explicit narrowing on both lines: integer promotion makes `x << 1` and `x ^ kPrimitive`
        // int-typed, so assigning back to uint16_t is a narrowing conversion that GCC flags
        // under -Wconversion. The values provably fit (x stays under 0x200 by construction);
        // the casts say so rather than leaving it implicit.
        x = static_cast<std::uint16_t>(x << 1);
        if (x & 0x100U) x = static_cast<std::uint16_t>(x ^ kPrimitive);
    }
    // Duplicate so Mul() can index log[a]+log[b] (max 508) without a modulo.
    for (std::size_t i = 255; i < 512; ++i) exp[i] = exp[i - 255];
    log[0] = 0;  // undefined mathematically; never read via a guarded path
}

const Tables& T() noexcept {
    static const Tables t;
    return t;
}

}  // namespace fileflow::gf

namespace fileflow {
namespace {

// Polynomial multiply over GF(256). Coefficients are most-significant-first.
std::vector<std::uint8_t> PolyMul(std::span<const std::uint8_t> a,
                                  std::span<const std::uint8_t> b) {
    std::vector<std::uint8_t> r(a.size() + b.size() - 1, 0);
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i] == 0) continue;
        for (std::size_t j = 0; j < b.size(); ++j) {
            r[i + j] = gf::Add(r[i + j], gf::Mul(a[i], b[j]));
        }
    }
    return r;
}

std::uint8_t PolyEval(std::span<const std::uint8_t> p, std::uint8_t x) {
    std::uint8_t y = 0;
    for (std::uint8_t c : p) y = gf::Add(gf::Mul(y, x), c);
    return y;
}

}  // namespace

ReedSolomon::ReedSolomon(std::size_t nsym) : nsym_(nsym) {
    // g(x) = prod_{i=0}^{nsym-1} (x - alpha^i)
    gen_ = {1};
    for (std::size_t i = 0; i < nsym_; ++i) {
        const std::array<std::uint8_t, 2> factor = {1, gf::Pow(2, static_cast<int>(i))};
        gen_ = PolyMul(gen_, factor);
    }
}

Result<std::vector<std::uint8_t>> ReedSolomon::Encode(
    std::span<const std::uint8_t> message) const {
    if (message.size() > max_message()) return Error::kValueOutOfRange;

    std::vector<std::uint8_t> out(message.size() + nsym_, 0);
    std::copy(message.begin(), message.end(), out.begin());

    // Synthetic division of message*x^nsym by g(x); remainder is the parity.
    for (std::size_t i = 0; i < message.size(); ++i) {
        const std::uint8_t coef = out[i];
        if (coef == 0) continue;
        for (std::size_t j = 1; j < gen_.size(); ++j) {
            out[i + j] = gf::Add(out[i + j], gf::Mul(gen_[j], coef));
        }
    }
    // Restore the systematic message portion (the loop above clobbered nothing before i,
    // but the parity region now holds the remainder).
    std::copy(message.begin(), message.end(), out.begin());
    return out;
}

std::vector<std::uint8_t> ReedSolomon::Syndromes(std::span<const std::uint8_t> cw) const {
    std::vector<std::uint8_t> s(nsym_, 0);
    for (std::size_t i = 0; i < nsym_; ++i) {
        s[i] = PolyEval(cw, gf::Pow(2, static_cast<int>(i)));
    }
    return s;
}

Result<std::size_t> ReedSolomon::Decode(std::span<std::uint8_t> codeword,
                                        std::span<const std::size_t> erasure_positions) const {
    if (codeword.size() < nsym_ + 1 || codeword.size() > 255) return Error::kValueOutOfRange;
    const std::size_t n_len = codeword.size();

    // Validate erasures before using them. A position past the end would index out of bounds
    // during locator construction, and duplicates would inflate the locator's degree and make
    // the decoder claim more correction budget than it has.
    std::vector<std::size_t> erasures;
    erasures.reserve(erasure_positions.size());
    for (const std::size_t p : erasure_positions) {
        if (p >= n_len) return Error::kValueOutOfRange;
        erasures.push_back(p);
    }
    std::sort(erasures.begin(), erasures.end());
    erasures.erase(std::unique(erasures.begin(), erasures.end()), erasures.end());

    // Beyond nsym erasures the code is out of budget no matter how good the positions are.
    if (erasures.size() > nsym_) return Error::kUncorrectable;

    const std::vector<std::uint8_t> synd = Syndromes(codeword);
    if (std::all_of(synd.begin(), synd.end(), [](std::uint8_t v) { return v == 0; })) {
        return std::size_t{0};  // clean, even if erasures were declared
    }

    // --- Erasure locator: Lambda_e(x) = prod (1 + X_i x), coefficients ascending ---
    // The positions are KNOWN, so this part of the locator costs no syndrome budget.
    std::vector<std::uint8_t> lambda_e{1};
    for (const std::size_t pos : erasures) {
        const std::uint8_t x_i = gf::Pow(2, static_cast<int>(n_len - 1 - pos));
        // Multiply by (1 + X_i x): new[j] = old[j] + X_i * old[j-1], descending so each read
        // sees the previous iteration's value.
        lambda_e.push_back(0);
        for (std::size_t j = lambda_e.size() - 1; j-- > 0;) {
            lambda_e[j + 1] = gf::Add(lambda_e[j + 1], gf::Mul(x_i, lambda_e[j]));
        }
    }

    // --- Forney syndromes: T(x) = S(x) * Lambda_e(x) mod x^nsym ---
    // Folding the known erasures into the syndromes leaves a residual sequence in which only
    // the UNKNOWN errors remain, so Berlekamp-Massey below solves a smaller problem.
    std::vector<std::uint8_t> forney(nsym_, 0);
    for (std::size_t i = 0; i < nsym_; ++i) {
        std::uint8_t acc = 0;
        for (std::size_t j = 0; j < lambda_e.size() && j <= i; ++j) {
            acc = gf::Add(acc, gf::Mul(lambda_e[j], synd[i - j]));
        }
        forney[i] = acc;
    }

    // Remaining budget for unknown-position errors.
    const std::size_t f = erasures.size();
    const std::size_t max_errors = (nsym_ - f) / 2;
    const std::size_t bm_syndromes = nsym_ - f;

    // --- Berlekamp-Massey on the Forney syndromes: error-locator sigma(x) ---
    std::vector<std::uint8_t> sigma{1};
    std::vector<std::uint8_t> prev{1};
    std::size_t l = 0;
    std::size_t m = 1;
    std::uint8_t b = 1;

    // Consume the Forney syndromes starting at offset f, NOT at zero.
    //
    // The key equation is Lambda_err(x) * T(x) = Omega(x) mod x^nsym with deg Omega < f + e.
    // The first f coefficients of T are therefore Omega's, carrying no information about the
    // unknown errors -- feeding them to Berlekamp-Massey invents a spurious error locator.
    // The symptom was precise: pure-erasure cases failed everywhere EXCEPT f == nsym, where
    // the loop does not execute and the bug cannot express itself.
    for (std::size_t n = 0; n < bm_syndromes; ++n) {
        std::uint8_t delta = forney[f + n];
        for (std::size_t i = 1; i <= l && i < sigma.size(); ++i) {
            delta = gf::Add(delta, gf::Mul(sigma[i], forney[f + n - i]));
        }
        if (delta == 0) {
            ++m;
        } else if (2 * l <= n) {
            const std::vector<std::uint8_t> t = sigma;
            const std::uint8_t scale = gf::Div(delta, b);
            if (sigma.size() < prev.size() + m) sigma.resize(prev.size() + m, 0);
            for (std::size_t i = 0; i < prev.size(); ++i) {
                sigma[i + m] = gf::Add(sigma[i + m], gf::Mul(scale, prev[i]));
            }
            l = n + 1 - l;
            prev = t;
            b = delta;
            m = 1;
        } else {
            const std::uint8_t scale = gf::Div(delta, b);
            if (sigma.size() < prev.size() + m) sigma.resize(prev.size() + m, 0);
            for (std::size_t i = 0; i < prev.size(); ++i) {
                sigma[i + m] = gf::Add(sigma[i + m], gf::Mul(scale, prev[i]));
            }
            ++m;
        }
    }

    if (l > max_errors) return Error::kUncorrectable;

    // --- Combined locator: Lambda(x) = Lambda_e(x) * sigma(x) ---
    std::vector<std::uint8_t> lambda(lambda_e.size() + sigma.size() - 1, 0);
    for (std::size_t i = 0; i < lambda_e.size(); ++i) {
        if (lambda_e[i] == 0) continue;
        for (std::size_t j = 0; j < sigma.size(); ++j) {
            lambda[i + j] = gf::Add(lambda[i + j], gf::Mul(lambda_e[i], sigma[j]));
        }
    }
    sigma = lambda;

    // --- Chien search: roots of the combined locator give every corrected position ---
    std::vector<std::size_t> positions;
    for (std::size_t i = 0; i < n_len; ++i) {
        // Evaluate sigma at alpha^-i. Coefficients here are least-significant-first.
        std::uint8_t acc = 0;
        const std::uint8_t x = gf::Pow(2, -static_cast<int>(i));
        for (std::size_t j = sigma.size(); j-- > 0;) {
            acc = gf::Add(gf::Mul(acc, x), sigma[j]);
        }
        if (acc == 0) positions.push_back(n_len - 1 - i);
    }
    // Every erasure plus every located error must be accounted for. A shortfall means the
    // locator has roots outside the codeword, which is the signature of an uncorrectable
    // pattern rather than a recoverable one.
    if (positions.size() != l + f) return Error::kUncorrectable;

    // --- Forney: compute error magnitudes ---
    // omega(x) = S(x) * sigma(x) mod x^nsym
    std::vector<std::uint8_t> omega(nsym_, 0);
    for (std::size_t i = 0; i < nsym_; ++i) {
        std::uint8_t acc = 0;
        for (std::size_t j = 0; j <= i && j < sigma.size(); ++j) {
            acc = gf::Add(acc, gf::Mul(sigma[j], synd[i - j]));
        }
        omega[i] = acc;
    }

    std::size_t corrected = 0;
    for (std::size_t pos : positions) {
        const auto exponent = static_cast<int>(n_len - 1 - pos);
        const std::uint8_t xi = gf::Pow(2, exponent);
        const std::uint8_t xi_inv = gf::Inv(xi);

        // omega(xi_inv)
        std::uint8_t num = 0;
        for (std::size_t j = omega.size(); j-- > 0;) {
            num = gf::Add(gf::Mul(num, xi_inv), omega[j]);
        }

        // sigma'(xi_inv) — formal derivative keeps only odd-index terms in GF(2^m)
        std::uint8_t den = 0;
        for (std::size_t j = 1; j < sigma.size(); j += 2) {
            den = gf::Add(den, gf::Mul(sigma[j], gf::Pow(xi_inv, static_cast<int>(j - 1))));
        }
        if (den == 0) return Error::kUncorrectable;

        const std::uint8_t magnitude = gf::Mul(xi, gf::Div(num, den));
        codeword[pos] = gf::Add(codeword[pos], magnitude);
        ++corrected;
    }

    // Verify: a miscorrection would otherwise flow silently into the payload (T5).
    const std::vector<std::uint8_t> check = Syndromes(codeword);
    if (!std::all_of(check.begin(), check.end(), [](std::uint8_t v) { return v == 0; })) {
        return Error::kUncorrectable;
    }
    return corrected;
}

}  // namespace fileflow

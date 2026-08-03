#include <fileflow/fountain.h>

#include <fileflow/bytes.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace fileflow {

Status FountainParams::Validate() const noexcept {
    if (source_symbols == 0 || source_symbols > kMaxSourceSymbols) return Error::kValueOutOfRange;
    if (symbol_size == 0 || symbol_size > kMaxSymbolSize) return Error::kValueOutOfRange;
    if (!(c > 0.0) || !(delta > 0.0) || delta >= 1.0) return Error::kDegenerateParameters;

    // Checked before any allocation derived from these values (INPUT-VALIDATION SR-1).
    std::size_t total = 0;
    if (!CheckedMul(source_symbols, symbol_size, &total)) return Error::kSizeOverflow;
    if (total > (std::size_t{1} << 31)) return Error::kPayloadTooLarge;
    return Status::Ok();
}

LtDegreeTable::LtDegreeTable(const FountainParams& p) : p_(p) {
    const auto k = static_cast<double>(p_.source_symbols);
    const std::uint32_t kk = p_.source_symbols;

    // Ideal soliton: rho(1) = 1/k, rho(d) = 1/(d(d-1))
    std::vector<double> rho(kk + 1, 0.0);
    rho[1] = 1.0 / k;
    for (std::uint32_t d = 2; d <= kk; ++d) {
        rho[d] = 1.0 / (static_cast<double>(d) * static_cast<double>(d - 1));
    }

    // Robust component tau, spike at k/R
    const double r = p_.c * std::log(k / p_.delta) * std::sqrt(k);
    std::vector<double> tau(kk + 1, 0.0);
    if (r > 1.0) {
        const auto pivot = static_cast<std::uint32_t>(std::floor(k / r));
        for (std::uint32_t d = 1; d < pivot && d <= kk; ++d) {
            tau[d] = r / (static_cast<double>(d) * k);
        }
        if (pivot >= 1 && pivot <= kk) {
            tau[pivot] = r * std::log(r / p_.delta) / k;
        }
    }

    double beta = 0.0;
    for (std::uint32_t d = 1; d <= kk; ++d) beta += rho[d] + tau[d];

    cdf_.assign(kk + 1, 0.0);
    double acc = 0.0;
    for (std::uint32_t d = 1; d <= kk; ++d) {
        acc += (rho[d] + tau[d]) / beta;
        cdf_[d] = acc;
    }
    if (!cdf_.empty()) cdf_.back() = 1.0;  // guard against FP drift
}

std::uint32_t LtDegreeTable::SampleDegree(SplitMix64* rng) const {
    const double u = rng->NextDouble();
    // Linear scan: degrees are heavily concentrated at 1-3, so this beats binary search
    // in practice and keeps the encoder/decoder agreement trivially auditable.
    for (std::uint32_t d = 1; d < cdf_.size(); ++d) {
        if (u <= cdf_[d]) return d;
    }
    return p_.source_symbols;
}

void LtDegreeTable::Neighbours(std::uint32_t esi, std::vector<std::uint32_t>* out) const {
    out->clear();
    const std::uint32_t k = p_.source_symbols;

    // Systematic prefix: symbol i IS source symbol i.
    if (esi < k) {
        out->push_back(esi);
        return;
    }

    // Mixing the session seed keeps two concurrent sessions from generating identical
    // symbol structure (THREAT-MODEL T8 session confusion).
    SplitMix64 rng((static_cast<std::uint64_t>(p_.session_seed) << 32) ^
                   (static_cast<std::uint64_t>(esi) * 0x9E3779B97F4A7C15ULL));

    std::uint32_t d = SampleDegree(&rng);
    d = std::min(d, k);

    out->reserve(d);
    // Sample d distinct indices. For small d relative to k this rejection loop is cheap;
    // for large d we fall back to a partial shuffle to stay bounded.
    if (static_cast<std::uint64_t>(d) * 2 < k) {
        while (out->size() < d) {
            const std::uint32_t idx = rng.Below(k);
            if (std::find(out->begin(), out->end(), idx) == out->end()) out->push_back(idx);
        }
    } else {
        std::vector<std::uint32_t> pool(k);
        for (std::uint32_t i = 0; i < k; ++i) pool[i] = i;
        for (std::uint32_t i = 0; i < d; ++i) {
            const std::uint32_t j = i + rng.Below(k - i);
            std::swap(pool[i], pool[j]);
            out->push_back(pool[i]);
        }
    }
    std::sort(out->begin(), out->end());
}

// ---------------------------------------------------------------- encoder

Result<FountainEncoder> FountainEncoder::Create(const FountainParams& p,
                                                std::span<const std::uint8_t> source) {
    FF_TRY(p.Validate());

    std::size_t total = 0;
    if (!CheckedMul(p.source_symbols, p.symbol_size, &total)) return Error::kSizeOverflow;
    if (source.size() > total) return Error::kLengthMismatch;

    std::vector<std::uint8_t> src(total, 0);
    std::copy(source.begin(), source.end(), src.begin());
    return FountainEncoder(p, std::move(src));
}

std::vector<std::uint8_t> FountainEncoder::Symbol(std::uint32_t esi) const {
    std::vector<std::uint32_t> nb;
    table_.Neighbours(esi, &nb);

    std::vector<std::uint8_t> out(p_.symbol_size, 0);
    for (std::uint32_t idx : nb) {
        const std::uint8_t* s = src_.data() + static_cast<std::size_t>(idx) * p_.symbol_size;
        for (std::uint32_t b = 0; b < p_.symbol_size; ++b) out[b] ^= s[b];
    }
    return out;
}

// ---------------------------------------------------------------- decoder

FountainDecoder::FountainDecoder(const FountainParams& p)
    : p_(p),
      table_(p),
      out_(static_cast<std::size_t>(p.source_symbols) * p.symbol_size, 0),
      known_(p.source_symbols, false),
      waiting_(p.source_symbols) {}

Result<FountainDecoder> FountainDecoder::Create(const FountainParams& p) {
    FF_TRY(p.Validate());
    return FountainDecoder(p);
}

void FountainDecoder::Reduce(Pending* p) {
    // Remove every already-known neighbour by XORing it out.
    auto it = p->unknown.begin();
    while (it != p->unknown.end()) {
        const std::uint32_t idx = *it;
        if (known_[idx]) {
            const std::uint8_t* s = out_.data() + static_cast<std::size_t>(idx) * p_.symbol_size;
            for (std::uint32_t b = 0; b < p_.symbol_size; ++b) p->data[b] ^= s[b];
            it = p->unknown.erase(it);
        } else {
            ++it;
        }
    }
}

void FountainDecoder::Peel(std::uint32_t newly_known) {
    // Iterative rather than recursive: a deep chain on a hostile input would otherwise
    // blow the stack (THREAT-MODEL T4).
    std::vector<std::uint32_t> queue{newly_known};

    while (!queue.empty()) {
        const std::uint32_t idx = queue.back();
        queue.pop_back();

        auto waiters = std::move(waiting_[idx]);
        waiting_[idx].clear();

        for (std::size_t pi : waiters) {
            Pending& pend = pending_[pi];
            if (pend.unknown.empty()) continue;
            Reduce(&pend);
            if (pend.unknown.size() == 1) {
                const std::uint32_t target = pend.unknown[0];
                if (!known_[target]) {
                    std::uint8_t* dst =
                        out_.data() + static_cast<std::size_t>(target) * p_.symbol_size;
                    std::memcpy(dst, pend.data.data(), p_.symbol_size);
                    known_[target] = true;
                    ++recovered_;
                    queue.push_back(target);
                }
                pend.unknown.clear();
            }
        }
    }
}

bool FountainDecoder::Ingest(std::uint32_t esi, std::span<const std::uint8_t> data) {
    if (data.size() != p_.symbol_size) return complete();
    if (complete()) return true;
    ++ingested_;

    Pending pend;
    table_.Neighbours(esi, &pend.unknown);
    pend.data.assign(data.begin(), data.end());

    Reduce(&pend);

    if (pend.unknown.empty()) return complete();  // redundant, carried no new information

    if (pend.unknown.size() == 1) {
        const std::uint32_t target = pend.unknown[0];
        if (!known_[target]) {
            std::uint8_t* dst = out_.data() + static_cast<std::size_t>(target) * p_.symbol_size;
            std::memcpy(dst, pend.data.data(), p_.symbol_size);
            known_[target] = true;
            ++recovered_;
            Peel(target);
        }
        return complete();
    }

    pending_.push_back(std::move(pend));
    const std::size_t pi = pending_.size() - 1;
    for (std::uint32_t idx : pending_[pi].unknown) waiting_[idx].push_back(pi);
    return complete();
}

double FountainDecoder::overhead() const noexcept {
    if (p_.source_symbols == 0) return 0.0;
    const auto k = static_cast<double>(p_.source_symbols);
    return (static_cast<double>(ingested_) - k) / k;
}

Result<std::vector<std::uint8_t>> FountainDecoder::Take() {
    if (!complete()) return Error::kFountainIncomplete;
    return out_;
}

}  // namespace fileflow

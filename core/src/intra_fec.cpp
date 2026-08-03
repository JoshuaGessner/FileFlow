#include <fileflow/intra_fec.h>

#include <algorithm>

namespace fileflow {

Status IntraFecParams::Validate() const noexcept {
    if (codeword_bytes < 2 || codeword_bytes > 255) return Error::kValueOutOfRange;
    if (nsym == 0 || nsym >= codeword_bytes) return Error::kValueOutOfRange;
    return Status::Ok();
}

Result<IntraFec> IntraFec::Create(std::size_t frame_capacity_bytes, IntraFecParams p) {
    FF_TRY(p.Validate());
    if (frame_capacity_bytes < p.codeword_bytes) return Error::kValueOutOfRange;

    // Whole codewords only. A partial trailing codeword would need its own length handling on
    // both sides, and the leftover bytes are worth less than the complexity -- at 255-byte
    // codewords the waste is under 255 bytes per frame, well below 1% at our grid sizes.
    const std::size_t codewords = frame_capacity_bytes / p.codeword_bytes;
    if (codewords == 0) return Error::kValueOutOfRange;

    const std::size_t coded = codewords * p.codeword_bytes;
    const std::size_t message = codewords * p.message_bytes_per_codeword();
    return IntraFec(p, codewords, message, coded);
}

Result<std::vector<std::uint8_t>> IntraFec::Encode(
    std::span<const std::uint8_t> message) const {
    if (message.size() != message_bytes_) return Error::kLengthMismatch;

    const std::size_t k = p_.message_bytes_per_codeword();
    std::vector<std::uint8_t> out(coded_bytes_, 0);

    for (std::size_t cw = 0; cw < codewords_; ++cw) {
        // De-interleave the message for this codeword: it owns every codewords_-th byte.
        std::vector<std::uint8_t> msg(k);
        for (std::size_t i = 0; i < k; ++i) {
            msg[i] = message[i * codewords_ + cw];
        }

        FF_ASSIGN_OR_RETURN(auto encoded, rs_.Encode(msg));
        if (encoded.size() != p_.codeword_bytes) return Error::kInternal;

        // Interleave back out. Consecutive optical cells therefore belong to DIFFERENT
        // codewords, so a spatial burst is spread across all of them instead of destroying
        // one -- the whole point of the interleave.
        for (std::size_t i = 0; i < p_.codeword_bytes; ++i) {
            out[i * codewords_ + cw] = encoded[i];
        }
    }
    return out;
}

Result<std::vector<std::uint8_t>> IntraFec::Decode(std::span<const std::uint8_t> coded,
                                                   std::span<const std::size_t> erased_positions,
                                                   Stats* stats) const {
    if (coded.size() != coded_bytes_) return Error::kLengthMismatch;
    if (stats != nullptr) *stats = Stats{};

    const std::size_t k = p_.message_bytes_per_codeword();

    // Route each erasure to the codeword that owns it, converting the interleaved index into
    // a codeword-local one. This is the step that makes interleaving pay: a run of adjacent
    // erasures becomes a few isolated erasures in each codeword.
    std::vector<std::vector<std::size_t>> per_cw(codewords_);
    for (const std::size_t pos : erased_positions) {
        if (pos >= coded_bytes_) return Error::kValueOutOfRange;
        per_cw[pos % codewords_].push_back(pos / codewords_);
    }

    // Record the worst load across ALL codewords before decoding anything.
    //
    // Doing it inside the loop instead -- as the first version did -- meant an early return on
    // the first uncorrectable codeword left the statistic reflecting only the codewords already
    // processed, and never the failing one. That under-reports the worst load precisely when it
    // is largest, i.e. on exactly the frames the adaptive link controller most needs to hear
    // about: a frame that failed at nsym=32 with a load of 40 would report whatever the
    // earlier, healthier codewords happened to carry. The loads are all known here, before any
    // decode is attempted, so completeness costs nothing (component C14, finding F21).
    if (stats != nullptr) {
        for (const auto& cw_erasures : per_cw) {
            stats->worst_erasures_in_codeword =
                std::max(stats->worst_erasures_in_codeword, cw_erasures.size());
        }
    }

    std::vector<std::uint8_t> message(message_bytes_, 0);

    for (std::size_t cw = 0; cw < codewords_; ++cw) {
        std::vector<std::uint8_t> word(p_.codeword_bytes);
        for (std::size_t i = 0; i < p_.codeword_bytes; ++i) {
            word[i] = coded[i * codewords_ + cw];
        }

        // A failure here is a genuinely uncorrectable frame. Reporting it rather than
        // returning partial data keeps the erasure honest: the fountain layer is built to
        // absorb a lost frame, and would be poisoned by a silently half-corrected one.
        auto corrected_r = rs_.Decode(word, per_cw[cw]);
        if (!corrected_r.ok()) {
            if (stats != nullptr) {
                // The budget this codeword needed cannot be measured -- decoding is how it gets
                // measured. All that is known is that it exceeded the budget on offer, so record
                // that bound and mark the estimate censored rather than letting a floor pass for
                // a measurement.
                stats->budget_censored = true;
                stats->worst_budget_used = std::max(stats->worst_budget_used, p_.nsym + 1);
            }
            return corrected_r.error();
        }
        const std::size_t corrected = std::move(corrected_r).value();
        if (stats != nullptr) {
            ++stats->codewords_decoded;
            stats->symbols_corrected += corrected;
            // `corrected` counts every symbol the decoder changed: the declared erasures plus
            // the errors its locator found. So the errors fall out by subtraction, and the
            // budget is the classic 2*errors + erasures. The guard covers the all-syndromes-zero
            // early-out, where a clean codeword reports 0 corrections despite declared erasures.
            const std::size_t f = per_cw[cw].size();
            const std::size_t errors = corrected > f ? corrected - f : 0;
            stats->worst_budget_used = std::max(stats->worst_budget_used, 2 * errors + f);
        }

        for (std::size_t i = 0; i < k; ++i) {
            message[i * codewords_ + cw] = word[i];
        }
    }
    return message;
}

}  // namespace fileflow

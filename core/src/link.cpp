#include <fileflow/link.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace fileflow {

// ---------------------------------------------------------------- ChannelEstimator

Status ChannelEstimatorConfig::Validate() const noexcept {
    // No upper bound worth enforcing: a very long half-life is simply indistinguishable from
    // no forgetting at all, which is the documented default.
    return Status::Ok();
}

Result<ChannelEstimator> ChannelEstimator::Create(std::size_t codewords_per_frame,
                                                  std::size_t codeword_bytes,
                                                  ChannelEstimatorConfig cfg) {
    FF_TRY(cfg.Validate());
    if (codewords_per_frame == 0) return Error::kValueOutOfRange;
    // Matches IntraFecParams::Validate, so the estimator cannot be configured for a code the
    // FEC layer would refuse to build.
    if (codeword_bytes < 2 || codeword_bytes > 255) return Error::kValueOutOfRange;

    ChannelEstimator e(codewords_per_frame, codeword_bytes, cfg);
    if (cfg.half_life_frames > 0) {
        e.decay_ = std::pow(0.5, 1.0 / static_cast<double>(cfg.half_life_frames));
    }
    return e;
}

void ChannelEstimator::Decay() noexcept {
    if (decay_ >= 1.0) return;
    for (double& bin : budget_hist_) bin *= decay_;
    fec_weight_ *= decay_;
    pre_fec_weight_ *= decay_;
}

void ChannelEstimator::ObserveFrame(std::size_t worst_codeword_budget, bool censored) noexcept {
    Decay();
    ++frames_;
    if (censored) ++censored_;
    // `2*errors + erasures` genuinely can exceed the bin count, and this is fed from a decoder
    // parsing attacker-controlled optical input either way, so clamp rather than trust. An
    // out-of-range index here would be a buffer overrun.
    const std::size_t budget = std::min(worst_codeword_budget, kLoadBins - 1);
    budget_hist_[budget] += 1.0;
    fec_weight_ += 1.0;
    worst_budget_seen_ = std::max(worst_budget_seen_, budget);
}

void ChannelEstimator::ObservePreFecLoss() noexcept {
    Decay();
    ++frames_;
    ++pre_fec_losses_;
    pre_fec_weight_ += 1.0;
}

double ChannelEstimator::pre_fec_success() const noexcept {
    const double total = fec_weight_ + pre_fec_weight_;
    // No observations at all: report 1.0 rather than 0.0. Zero would make every rung score
    // zero, and "no evidence" must not read as "everything fails" -- the controller gates on
    // min_observations, and this keeps its scores interpretable before that gate opens.
    return total > 0.0 ? fec_weight_ / total : 1.0;
}

double ChannelEstimator::FrameSuccessAt(std::size_t nsym) const noexcept {
    if (fec_weight_ <= 0.0) return 0.0;
    const std::size_t top = std::min(nsym, kLoadBins - 1);
    double ok = 0.0;
    for (std::size_t i = 0; i <= top; ++i) ok += budget_hist_[i];
    // Clamped because this is a probability and callers multiply by it. Summing decayed bins
    // and dividing by a separately-decayed total does not land exactly on 1.0, and a
    // "probability" of 1.0000000000000002 leaking into a score is the kind of detail that
    // survives review and then confuses a report.
    return std::clamp(ok / fec_weight_, 0.0, 1.0);
}

std::size_t ChannelEstimator::BudgetQuantile(double q) const noexcept {
    if (fec_weight_ <= 0.0) return 0;
    const double want = std::clamp(q, 0.0, 1.0) * fec_weight_;
    double run = 0.0;
    for (std::size_t i = 0; i < kLoadBins; ++i) {
        run += budget_hist_[i];
        if (run >= want) return i;
    }
    return kLoadBins - 1;
}

double ChannelEstimator::mean_budget() const noexcept {
    if (fec_weight_ <= 0.0) return 0.0;
    double sum = 0.0;
    for (std::size_t i = 0; i < kLoadBins; ++i) {
        sum += static_cast<double>(i) * budget_hist_[i];
    }
    return sum / fec_weight_;
}

void ChannelEstimator::Reset() noexcept {
    budget_hist_.fill(0.0);
    fec_weight_ = 0.0;
    pre_fec_weight_ = 0.0;
    frames_ = 0;
    pre_fec_losses_ = 0;
    censored_ = 0;
    worst_budget_seen_ = 0;
}

// ---------------------------------------------------------------- LinkController

Status LinkControllerConfig::Validate() const noexcept {
    if (ladder.empty()) return Error::kValueOutOfRange;
    if (min_observations == 0) return Error::kValueOutOfRange;
    if (promote_margin < 0.0 || fallback_margin < 0.0) return Error::kValueOutOfRange;

    for (std::size_t i = 0; i < ladder.size(); ++i) {
        // Same bounds IntraFecParams::Validate enforces: a rung the FEC layer would refuse is
        // not a rung, and discovering that at Create time beats discovering it at a switch.
        if (ladder[i] == 0 || ladder[i] >= 255) return Error::kValueOutOfRange;
        // Strictly ascending, so "one rung stronger" is well defined for the hysteresis step.
        if (i > 0 && ladder[i] <= ladder[i - 1]) return Error::kValueOutOfRange;
    }
    return Status::Ok();
}

double LinkController::SmallestPromotionGain(const std::vector<std::size_t>& ladder,
                                             std::size_t codeword_bytes) noexcept {
    const double cb = static_cast<double>(codeword_bytes);
    double smallest = std::numeric_limits<double>::infinity();
    for (std::size_t i = 1; i < ladder.size(); ++i) {
        if (ladder[i] >= codeword_bytes) continue;  // Validate rejects these separately
        // Promoting from ladder[i] to the next weaker code ladder[i-1] multiplies the message
        // capacity by (cb - ladder[i-1]) / (cb - ladder[i]).
        const double gain = (cb - static_cast<double>(ladder[i - 1])) /
                                (cb - static_cast<double>(ladder[i])) -
                            1.0;
        smallest = std::min(smallest, gain);
    }
    return smallest;
}

Result<LinkController> LinkController::Create(std::size_t codewords_per_frame,
                                              std::size_t codeword_bytes, LinkProfile initial,
                                              LinkControllerConfig cfg) {
    FF_TRY(cfg.Validate());
    if (codewords_per_frame == 0) return Error::kValueOutOfRange;
    if (codeword_bytes < 2 || codeword_bytes > 255) return Error::kValueOutOfRange;
    // The starting profile must itself be a rung, or the first Select() would have to step from
    // a position that has no neighbours.
    if (std::find(cfg.ladder.begin(), cfg.ladder.end(), initial.nsym) == cfg.ladder.end()) {
        return Error::kValueOutOfRange;
    }
    for (const std::size_t nsym : cfg.ladder) {
        if (nsym >= codeword_bytes) return Error::kValueOutOfRange;
    }
    // Refuse a margin no single promotion on this ladder could ever clear. Such a controller
    // does not oscillate, does not error, and never reaches the code rate the channel supports
    // -- it just quietly runs a session at less capacity than it had. Loud beats silent.
    if (cfg.ladder.size() > 1 &&
        cfg.promote_margin >= SmallestPromotionGain(cfg.ladder, codeword_bytes)) {
        return Error::kDegenerateParameters;
    }
    return LinkController(codewords_per_frame, codeword_bytes, initial, std::move(cfg));
}

RungScore LinkController::ScoreOne(const ChannelEstimator& est, std::size_t nsym) const {
    const double cb = static_cast<double>(codeword_bytes_);
    RungScore s;
    s.nsym = nsym;
    s.code_rate = (cb - static_cast<double>(nsym)) / cb;
    s.predicted_frame_success = est.FrameSuccessAt(nsym);
    s.message_bytes_per_frame =
        static_cast<double>(codewords_) * (cb - static_cast<double>(nsym));
    s.score = s.predicted_frame_success * est.pre_fec_success() * s.message_bytes_per_frame;
    return s;
}

std::vector<RungScore> LinkController::ScoreLadder(const ChannelEstimator& est) const {
    std::vector<RungScore> out;
    out.reserve(cfg_.ladder.size());
    for (const std::size_t nsym : cfg_.ladder) out.push_back(ScoreOne(est, nsym));
    return out;
}

RungScore LinkController::BestRung(const ChannelEstimator& est) const {
    RungScore best = ScoreOne(est, cfg_.ladder.front());
    for (std::size_t i = 1; i < cfg_.ladder.size(); ++i) {
        const RungScore s = ScoreOne(est, cfg_.ladder[i]);
        // `>=` rather than `>`, over an ascending ladder, breaks ties toward MORE parity: when
        // two rungs score equally the stronger code is strictly safer against an estimate that
        // is slightly wrong, and the estimate is always slightly wrong.
        if (s.score >= best.score) best = s;
    }
    return best;
}

LinkProfile LinkController::Select(const ChannelEstimator& est, std::uint64_t frame_index) {
    // Too little evidence to act on. Staying put is the correct default: the initial profile
    // was chosen deliberately and a few frames cannot beat that reasoning.
    if (est.fec_weight() < static_cast<double>(cfg_.min_observations)) return current_;

    const RungScore best = BestRung(est);
    if (best.nsym == current_.nsym) return current_;

    const RungScore cur = ScoreOne(est, current_.nsym);
    const bool strengthening = best.nsym > current_.nsym;

    // Every frame is failing at the current rung. Waiting out a dwell here is pure loss -- the
    // link is delivering nothing and cannot get worse by moving. "Fall back fast" in its
    // strongest form, and the one case that overrides the dwell.
    const bool desperate = cur.score <= 0.0 && best.score > 0.0;

    if (!desperate) {
        const std::uint64_t dwell =
            strengthening ? cfg_.fallback_dwell_frames : cfg_.promote_dwell_frames;
        // Underflow-safe: frame_index is monotonic but callers may reset it across sessions.
        if (frame_index < last_change_ || frame_index - last_change_ < dwell) return current_;

        const double margin = strengthening ? cfg_.fallback_margin : cfg_.promote_margin;
        if (best.score < cur.score * (1.0 + margin)) return current_;
    }

    std::size_t target = best.nsym;
    if (!strengthening) {
        // Promote slowly: step ONE rung toward the target instead of jumping. A wrong estimate
        // then costs one rung of robustness rather than all of it, and a genuine oscillation
        // shows up as a slow walk that the change counter makes visible, rather than as a flip
        // that looks like a single decision.
        const auto it = std::find(cfg_.ladder.begin(), cfg_.ladder.end(), current_.nsym);
        if (it != cfg_.ladder.begin()) target = *std::prev(it);
    }
    // Strengthening jumps straight to the argmax: an under-protected link is losing whole
    // frames every frame it waits, so the asymmetry is paid for in exactly the direction
    // MODULATION-SPEC asks for.

    if (target == current_.nsym) return current_;
    current_.nsym = target;
    last_change_ = frame_index;
    ++changes_;
    return current_;
}

}  // namespace fileflow

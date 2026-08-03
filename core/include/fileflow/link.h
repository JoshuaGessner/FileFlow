// Adaptive link controller (component C14): channel-state estimation (ADP-01) and code-rate
// selection with hysteresis (ADP-02).
//
// WHAT MAKES THIS CHEAP -- and it is the whole reason the component is worth building now:
//
// `IntraFec` derives its codeword count from the FRAME CAPACITY alone
// (`codewords = frame_capacity / codeword_bytes`); `nsym` shrinks only the message. So the
// interleave mapping, the coded frame length and therefore the per-codeword damage pattern are
// ALL INVARIANT IN `nsym`. A codeword decodes at `nsym = n` exactly when the budget it needs,
// `2*errors + erasures`, is <= n -- and neither term depends on n.
//
// Two consequences follow, and they are the design:
//
//  1. The per-frame worst codeword BUDGET is a SUFFICIENT STATISTIC for the entire ladder. One
//     histogram of it -- 256 bins, one increment per frame, no allocation -- scores every
//     candidate rung counterfactually, from a run made at a single rung. The receiver never
//     has to try a rung to find out what it would have done.
//
//  2. That statistic is `IntraFec::Stats::worst_budget_used`, computed from numbers the decoder
//     already produces. C14 needs NO new per-frame measurement, only the subtraction that
//     recovers the error count from what Reed-Solomon reports.
//
// ERASURES ALONE ARE NOT ENOUGH, and this cost a wrong answer before it was fixed. The first
// version binned the worst ERASURE load, on the reasoning that erasures are what the receiver
// knows about. On the impaired channel of F18 that predicted a 100% frame success rate at
// `nsym = 16`, and recommended it -- while the real decoder failed 11 of 117 frames there and
// lost 24% of the goodput available at `nsym = 32`. The missing term was undetected errors,
// which cost two parity bytes each (finding F23).
//
// WHAT THIS DELIBERATELY DOES NOT DO:
//
//  - **It does not actuate.** `nsym` sets the FEC message size, which sets the fountain symbol
//    size, which the session manifest fixes for the whole transfer (an XOR-based erasure code
//    cannot combine symbols of unequal length). So code-rate selection is a SESSION-START
//    decision against this protocol, not a mid-session control loop, and on a one-way link the
//    transmitter cannot hear the receiver's answer at all (OQ-013). This class computes the
//    recommendation and reports it; carrying it to the transmitter is a protocol question, and
//    pretending otherwise would produce goodput figures no real link could reach.
//
//  - **It does not model thermal state.** The registry's interface takes a `ThermalState`
//    (ADP-05) and this one does not, because the decode-cost numbers that would justify any
//    thermal policy come from EXP-011 and EXP-020 and neither has run. An invented policy here
//    would be a design decision living only in code, which CONTRIBUTING forbids. OQ-036.
//
//  - **It cannot measure the budget of a frame that did not decode.** Decoding is how the
//    budget gets measured, so an uncorrectable frame yields only the bound "more than the rung
//    it was tried at". Such observations are recorded at that floor and COUNTED
//    (`censored_observations`), because they predict failure correctly for every rung at or
//    below the current one but credit the frame optimistically above it. The practical rule:
//    an estimate taken at a rung that loses frames is trustworthy for weakening the code and
//    optimistic for strengthening it.
#pragma once

#include <fileflow/frame.h>
#include <fileflow/result.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace fileflow {

// One selectable operating point. Only `nsym` varies today: M1-M4 are unimplemented, and
// promoting a modulation profile has exactly the same fountain-symbol-size consequence that
// blocks mid-session `nsym` changes.
struct LinkProfile {
    ModulationProfile modulation = ModulationProfile::kM0BinaryLuminance;
    std::size_t nsym = 32;

    [[nodiscard]] bool operator==(const LinkProfile&) const noexcept = default;
};

// What one rung of the ladder is predicted to deliver.
//
// `score` is expected MESSAGE BYTES PER DISPLAY STATE PRESENTED -- deliberately not "goodput",
// because it omits the fountain-overhead coupling (see ScoreLadder). Naming it goodput would
// breach the six-rate discipline (PERFORMANCE-PHILOSOPHY).
struct RungScore {
    std::size_t nsym = 0;
    double code_rate = 0.0;                  // Rfec = (codeword_bytes - nsym) / codeword_bytes
    double predicted_frame_success = 0.0;    // P(decodes | frame reached the FEC layer)
    double message_bytes_per_frame = 0.0;    // capacity if it decodes
    double score = 0.0;                      // expected message bytes per display state
};

// ---------------------------------------------------------------- ADP-01

struct ChannelEstimatorConfig {
    // Exponential forgetting, in frames. 0 disables it, which is correct when estimating ONE
    // nsym for a whole session -- every frame is equally relevant to that decision. Set it to
    // track a channel that changes during the session, which is what the oscillation test
    // (EXP-023 H3) exercises.
    std::uint64_t half_life_frames = 0;

    [[nodiscard]] Status Validate() const noexcept;
};

// Receiver-side channel state. Consumes only quantities the live receiver already produces,
// so nothing here needs the simulator's ground truth.
//
// Fixed storage and no allocation after construction: this is fed from the per-frame path, and
// CONTRIBUTING bans allocation there.
class ChannelEstimator {
  public:
    // Every rung satisfies `nsym < codeword_bytes <= 255`, so a budget of 255 already means
    // "fails at every rung" and 256 bins covers every distinction that can matter. Budgets
    // above that are clamped, losing nothing: `2*errors + erasures` can exceed 255, but every
    // such value is equivalent for the ladder.
    static constexpr std::size_t kLoadBins = 256;

    static Result<ChannelEstimator> Create(std::size_t codewords_per_frame,
                                           std::size_t codeword_bytes = 255,
                                           ChannelEstimatorConfig cfg = {});

    // A frame that reached the FEC layer. `worst_codeword_budget` is
    // `IntraFec::Stats::worst_budget_used` -- the parity bytes the worst codeword actually
    // consumed, `2*errors + erasures`. Pass `IntraFec::Stats::budget_censored` as `censored`:
    // an uncorrectable frame yields a lower bound rather than a measurement, and the estimator
    // reports how much of its evidence is bounded so a caller can say so too.
    void ObserveFrame(std::size_t worst_codeword_budget, bool censored = false) noexcept;

    // A frame lost BEFORE the FEC layer: screen not found, header unreadable, photometric
    // estimate refused. Counted separately and deliberately, because no amount of parity
    // recovers it. Folding these into the FEC statistics would make a tracking failure look
    // like a code-rate problem and drive the controller to spend parity on it.
    void ObservePreFecLoss() noexcept;

    [[nodiscard]] std::uint64_t frames_observed() const noexcept { return frames_; }
    [[nodiscard]] std::uint64_t pre_fec_losses() const noexcept { return pre_fec_losses_; }

    // Effective FEC-layer sample count. Fractional once forgetting is on.
    [[nodiscard]] double fec_weight() const noexcept { return fec_weight_; }

    // P(a frame reaches the FEC layer at all). Constant across rungs, so it cancels in the
    // ranking -- carried anyway so `score` means what its name says.
    [[nodiscard]] double pre_fec_success() const noexcept;

    // P(worst codeword budget <= nsym | the frame reached the FEC layer).
    [[nodiscard]] double FrameSuccessAt(std::size_t nsym) const noexcept;

    // Frames whose budget was only bounded below, because they did not decode. Predictions for
    // rungs STRONGER than the one that produced them are optimistic by exactly this much: a
    // frame recorded at the `nsym + 1` floor is credited with succeeding at any higher rung,
    // and it may not. Report it alongside any recommendation drawn from a lossy run.
    [[nodiscard]] std::uint64_t censored_observations() const noexcept { return censored_; }

    // Budget distribution summaries, for telemetry and for humans reading a run report.
    [[nodiscard]] std::size_t BudgetQuantile(double q) const noexcept;
    [[nodiscard]] std::size_t worst_budget_seen() const noexcept { return worst_budget_seen_; }
    [[nodiscard]] double mean_budget() const noexcept;

    [[nodiscard]] std::size_t codewords_per_frame() const noexcept { return codewords_; }
    [[nodiscard]] std::size_t codeword_bytes() const noexcept { return codeword_bytes_; }

    void Reset() noexcept;

  private:
    ChannelEstimator(std::size_t codewords, std::size_t codeword_bytes,
                     ChannelEstimatorConfig cfg)
        : codewords_(codewords), codeword_bytes_(codeword_bytes), cfg_(cfg) {}

    void Decay() noexcept;

    std::size_t codewords_ = 1;
    std::size_t codeword_bytes_ = 255;
    ChannelEstimatorConfig cfg_{};

    std::array<double, kLoadBins> budget_hist_{};
    double fec_weight_ = 0.0;
    double pre_fec_weight_ = 0.0;
    std::uint64_t frames_ = 0;
    std::uint64_t pre_fec_losses_ = 0;
    std::uint64_t censored_ = 0;
    std::size_t worst_budget_seen_ = 0;
    double decay_ = 1.0;
};

// ---------------------------------------------------------------- ADP-02

struct LinkControllerConfig {
    // Candidate parity values, strictly ascending. Ascending order is required, not merely
    // conventional: "one rung stronger" has to be a well-defined step for hysteresis.
    std::vector<std::size_t> ladder{8, 16, 24, 32, 40, 48, 64};

    // Do not move on noise. A handful of frames cannot distinguish a channel change from
    // ordinary variance.
    std::size_t min_observations = 32;

    // MODULATION-SPEC: "fall back fast, promote slowly." A profile change costs a
    // resynchronisation, so the two directions are deliberately asymmetric -- weakening the
    // code has to earn its predicted gain, strengthening it barely has to earn anything. The
    // asymmetry that matters most in practice is the DWELL below, not these margins.
    //
    // ⚠ `promote_margin` IS COUPLED TO THE LADDER and cannot be chosen independently of it.
    // Adjacent rungs of the default ladder differ by only ~3.4% of frame capacity at the dense
    // end, so a margin of 0.10 makes promotion past that point impossible: the controller
    // reaches nsym=24 on a perfectly clean channel, finds every further step worth less than
    // its margin, and parks there permanently -- leaving ~6% of every frame unused for the rest
    // of the session, silently and forever. That is a worse failure than oscillation because
    // nothing about it looks wrong. `Create` therefore REJECTS a margin no single rung can
    // clear, rather than leaving the trap set (finding F22).
    double promote_margin = 0.02;    // weaken the code (lower nsym) only for a >=2% gain
    double fallback_margin = 0.02;   // strengthen it for a >=2% gain
    std::uint64_t promote_dwell_frames = 96;
    std::uint64_t fallback_dwell_frames = 16;

    [[nodiscard]] Status Validate() const noexcept;
};

class LinkController {
  public:
    // `codewords_per_frame` and `codeword_bytes` must come from the same `IntraFec` the
    // estimator is observing -- they set the payload arithmetic every score depends on.
    static Result<LinkController> Create(std::size_t codewords_per_frame,
                                        std::size_t codeword_bytes, LinkProfile initial,
                                        LinkControllerConfig cfg = {});

    // Smallest relative payload gain any single promotion on this ladder can offer. A
    // `promote_margin` at or above this value strands the controller: see the warning on
    // `LinkControllerConfig::promote_margin`. Exposed so a caller tuning either field can see
    // the constraint instead of discovering it as a mysteriously conservative code rate.
    [[nodiscard]] static double SmallestPromotionGain(const std::vector<std::size_t>& ladder,
                                                     std::size_t codeword_bytes) noexcept;

    // Score every rung from the estimator's load distribution.
    //
    // A PURE function of the estimate: no state, no hysteresis, no history. Exposed separately
    // because the RANKING is the claim EXP-023 tests, and a controller that picks the right
    // rung for the wrong reason has not passed.
    //
    // The objective is expected message bytes per display state, NOT goodput. It omits the
    // fountain-overhead coupling: a rung that loses more frames also pays higher reception
    // overhead, so its true cost is worse than scored here. The omission therefore biases the
    // choice toward TOO LITTLE parity, and is stated rather than modelled because an
    // uncalibrated overhead model would be a guess dressed as a correction.
    //
    // Allocates. Belongs on the low-frequency control loop, never the per-frame path.
    [[nodiscard]] std::vector<RungScore> ScoreLadder(const ChannelEstimator& est) const;

    // The unconstrained argmax: what the channel wants, ignoring the cost of switching.
    // Ties break toward MORE parity -- an equal-scoring stronger code is strictly safer
    // against an estimate that is slightly wrong.
    [[nodiscard]] RungScore BestRung(const ChannelEstimator& est) const;

    // The hysteresis-constrained decision. `frame_index` is the caller's monotonic frame
    // counter; dwell is counted in frames rather than seconds because frames are the unit a
    // switch actually costs.
    LinkProfile Select(const ChannelEstimator& est, std::uint64_t frame_index);

    [[nodiscard]] const LinkProfile& current() const noexcept { return current_; }
    [[nodiscard]] std::uint64_t changes() const noexcept { return changes_; }
    [[nodiscard]] std::uint64_t last_change_frame() const noexcept { return last_change_; }
    [[nodiscard]] const LinkControllerConfig& config() const noexcept { return cfg_; }

  private:
    LinkController(std::size_t codewords, std::size_t codeword_bytes, LinkProfile initial,
                   LinkControllerConfig cfg)
        : codewords_(codewords),
          codeword_bytes_(codeword_bytes),
          cfg_(std::move(cfg)),
          current_(initial) {}

    [[nodiscard]] RungScore ScoreOne(const ChannelEstimator& est, std::size_t nsym) const;

    std::size_t codewords_ = 1;
    std::size_t codeword_bytes_ = 255;
    LinkControllerConfig cfg_;
    LinkProfile current_{};
    std::uint64_t changes_ = 0;
    std::uint64_t last_change_ = 0;
};

}  // namespace fileflow

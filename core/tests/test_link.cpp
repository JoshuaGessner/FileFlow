// Adaptive link controller (C14): channel estimation and code-rate selection.
//
// The component's whole claim is that the receiver can score EVERY rung of the parity ladder
// from a run made at ONE rung, because the per-codeword erasure load does not depend on `nsym`.
// That claim is tested here against the real `IntraFec`, not against a model of it -- if the
// interleave or the codeword count ever starts depending on `nsym`, the counterfactual becomes
// silently wrong, and a test that only exercised the controller's arithmetic would still pass.
//
// The second thing under test is that hysteresis does the work the margins claim: the
// oscillation test runs the SAME channel through a margin-free controller and requires it to
// misbehave, so the passing case cannot be an artifact of an easy scenario.
#include <fileflow/fountain.h>  // SplitMix64
#include <fileflow/intra_fec.h>
#include <fileflow/link.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

using namespace fileflow;

namespace {

constexpr std::size_t kCapacity = 2750;  // 10 codewords of 255 bytes
constexpr std::size_t kCodewords = 10;

const std::vector<std::size_t>& Ladder() {
    static const std::vector<std::size_t> k{8, 16, 24, 32, 40, 48, 64};
    return k;
}

ChannelEstimator MakeEstimator(std::uint64_t half_life = 0) {
    auto e = ChannelEstimator::Create(kCodewords, 255, {.half_life_frames = half_life});
    EXPECT_TRUE(e.ok()) << ErrorName(e.error());
    return std::move(e).value();
}

LinkController MakeController(std::size_t initial_nsym, LinkControllerConfig cfg = {}) {
    auto c = LinkController::Create(kCodewords, 255, LinkProfile{.nsym = initial_nsym},
                                    std::move(cfg));
    EXPECT_TRUE(c.ok()) << ErrorName(c.error());
    return std::move(c).value();
}

// Distinct, sorted erasure positions. Duplicates would be counted twice by the decoder and
// would inflate the load, so the generator dedupes -- the same discipline the demodulator's
// erased-byte list already has.
std::vector<std::size_t> ErasurePattern(SplitMix64* rng, std::size_t count,
                                        std::size_t coded_bytes) {
    std::vector<std::size_t> pat;
    pat.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        pat.push_back(rng->Below(static_cast<std::uint32_t>(coded_bytes)));
    }
    std::sort(pat.begin(), pat.end());
    pat.erase(std::unique(pat.begin(), pat.end()), pat.end());
    return pat;
}

std::size_t WorstLoad(const std::vector<std::size_t>& pattern, std::size_t codewords) {
    std::vector<std::size_t> per_cw(codewords, 0);
    for (const std::size_t p : pattern) ++per_cw[p % codewords];
    return *std::max_element(per_cw.begin(), per_cw.end());
}

// Run one damage pattern through the real codec at one rung.
//
// `erasures` are corrupted AND declared -- the decoder is told where they are. `errors` are
// corrupted and NOT declared, so the decoder must locate them, at twice the cost. Both are
// needed: a channel with only erasures cannot distinguish a budget-based predictor from an
// erasure-based one, and that is precisely the confusion that made the first version of this
// component recommend the wrong rung (F23).
bool DecodesAt(std::size_t nsym, const std::vector<std::size_t>& erasures,
               const std::vector<std::size_t>& errors, std::uint64_t seed,
               IntraFec::Stats* stats = nullptr) {
    auto f = IntraFec::Create(kCapacity, {.nsym = nsym});
    EXPECT_TRUE(f.ok()) << ErrorName(f.error());
    const IntraFec& fec = f.value();

    SplitMix64 rng(seed);
    std::vector<std::uint8_t> msg(fec.message_bytes());
    for (auto& b : msg) b = static_cast<std::uint8_t>(rng.Next() & 0xFF);

    auto coded = fec.Encode(msg);
    EXPECT_TRUE(coded.ok()) << ErrorName(coded.error());
    std::vector<std::uint8_t> damaged = coded.value();
    for (const std::size_t p : erasures) {
        damaged[p] = static_cast<std::uint8_t>(rng.Next() & 0xFF);
    }
    for (const std::size_t p : errors) {
        // Guarantee an actual change, or the "error" costs the code nothing and the test would
        // be measuring a weaker channel than it claims.
        damaged[p] = static_cast<std::uint8_t>(damaged[p] ^ (1u + (rng.Next() & 0xFEu)));
    }

    auto back = fec.Decode(damaged, erasures, stats);
    return back.ok() && back.value() == msg;
}

// Erasure-only convenience overload for the tests that do not need unflagged errors.
bool DecodesAt(std::size_t nsym, const std::vector<std::size_t>& erasures, std::uint64_t seed,
               IntraFec::Stats* stats = nullptr) {
    return DecodesAt(nsym, erasures, {}, seed, stats);
}

}  // namespace

// ---------------------------------------------------------------- construction

TEST(ChannelEstimator, RejectsUnusableParameters) {
    EXPECT_FALSE(ChannelEstimator::Create(0).ok());
    EXPECT_FALSE(ChannelEstimator::Create(10, 1).ok());
    EXPECT_FALSE(ChannelEstimator::Create(10, 256).ok());
    EXPECT_TRUE(ChannelEstimator::Create(10, 255).ok());
}

TEST(LinkController, RejectsUnusableConfiguration) {
    EXPECT_FALSE(LinkController::Create(10, 255, {.nsym = 32}, {.ladder = {}}).ok());
    // Not ascending: "one rung stronger" would be undefined.
    EXPECT_FALSE(LinkController::Create(10, 255, {.nsym = 32}, {.ladder = {32, 16}}).ok());
    EXPECT_FALSE(LinkController::Create(10, 255, {.nsym = 32}, {.ladder = {16, 16}}).ok());
    // Rungs the FEC layer itself would refuse.
    EXPECT_FALSE(LinkController::Create(10, 255, {.nsym = 0}, {.ladder = {0, 16}}).ok());
    EXPECT_FALSE(LinkController::Create(10, 255, {.nsym = 255}, {.ladder = {255}}).ok());
    // A rung the FEC layer would refuse for THIS codeword size, even though it is < 255.
    EXPECT_FALSE(LinkController::Create(10, 64, {.nsym = 64}, {.ladder = {8, 64}}).ok());
    // A starting profile that is not on the ladder has no neighbours to step to.
    EXPECT_FALSE(LinkController::Create(10, 255, {.nsym = 20}, {.ladder = {8, 16, 32}}).ok());
    EXPECT_FALSE(LinkController::Create(0, 255, {.nsym = 32}).ok());
    EXPECT_TRUE(LinkController::Create(10, 255, {.nsym = 32}).ok());
}

TEST(LinkController, RejectsAPromoteMarginThatWouldStrandTheLadder) {
    // The defect this check exists to prevent (F22): a margin larger than any single rung's
    // payload gain leaves the controller permanently short of the code rate the channel
    // supports, with nothing in its behaviour looking wrong. Adjacent rungs of the default
    // ladder are ~3.4% apart at the dense end, so 10% -- a perfectly reasonable-looking
    // hysteresis figure, and the value this controller first shipped with -- strands it.
    EXPECT_NEAR(LinkController::SmallestPromotionGain(Ladder(), 255), 247.0 / 239.0 - 1.0, 1e-12);

    EXPECT_FALSE(LinkController::Create(10, 255, {.nsym = 32}, {.promote_margin = 0.10}).ok());
    EXPECT_EQ(LinkController::Create(10, 255, {.nsym = 32}, {.promote_margin = 0.10}).error(),
              Error::kDegenerateParameters);
    EXPECT_TRUE(LinkController::Create(10, 255, {.nsym = 32}, {.promote_margin = 0.03}).ok());

    // A coarser ladder tolerates a coarser margin: the constraint is a relationship between the
    // two, not a bound on either alone.
    EXPECT_TRUE(LinkController::Create(10, 255, {.nsym = 128},
                                       {.ladder = {8, 128}, .promote_margin = 0.10})
                    .ok());
    // A single-rung ladder cannot promote at all, so no margin can strand it.
    EXPECT_TRUE(
        LinkController::Create(10, 255, {.nsym = 32}, {.ladder = {32}, .promote_margin = 9.0})
            .ok());
}

// ---------------------------------------------------------------- H1, the load-bearing claim

TEST(LinkCounterfactual, WorstCodewordLoadDoesNotDependOnNsym) {
    // The property the entire component rests on, asserted directly rather than through its
    // consequences: same frame capacity, same erasure positions, every rung must report the
    // same worst load. A change to the interleave or the codeword count would break this, and
    // the controller would then be confidently answering about a channel it never saw.
    SplitMix64 rng(0x1A);
    for (int trial = 0; trial < 12; ++trial) {
        const auto pattern = ErasurePattern(&rng, 40 + rng.Below(300), 2550);
        const std::size_t expected = WorstLoad(pattern, kCodewords);

        for (const std::size_t nsym : Ladder()) {
            auto f = IntraFec::Create(kCapacity, {.nsym = nsym});
            ASSERT_TRUE(f.ok());
            // Coded length is invariant too -- that is why one position list is valid at every
            // rung in the first place.
            EXPECT_EQ(f.value().coded_bytes(), 2550u);
            EXPECT_EQ(f.value().codewords(), kCodewords);

            IntraFec::Stats stats;
            DecodesAt(nsym, pattern, 99, &stats);
            EXPECT_EQ(stats.worst_erasures_in_codeword, expected)
                << "nsym=" << nsym << " trial=" << trial;
        }
    }
}

TEST(LinkCounterfactual, StatsReportTheFailingCodewordsOwnLoad) {
    // Regression for the telemetry defect found while building this component (F21): the worst
    // load used to be accumulated inside the decode loop, so an early return on the first
    // uncorrectable codeword left the statistic reflecting only the healthy codewords decoded
    // before it. The frames the controller most needs to hear about were the ones under-reported.
    //
    // Codeword 0 gets a survivable load, codeword 7 an impossible one. Interleaved index
    // i belongs to codeword i % 10.
    std::vector<std::size_t> pattern;
    for (std::size_t i = 0; i < 4; ++i) pattern.push_back(i * kCodewords + 0);   // cw 0: 4
    for (std::size_t i = 0; i < 60; ++i) pattern.push_back(i * kCodewords + 7);  // cw 7: 60
    std::sort(pattern.begin(), pattern.end());

    IntraFec::Stats stats;
    const bool ok = DecodesAt(32, pattern, 5, &stats);
    EXPECT_FALSE(ok) << "60 erasures in one codeword must exceed a 32-byte budget";
    EXPECT_EQ(stats.worst_erasures_in_codeword, 60u)
        << "the failing codeword's own load must survive the early return";
}

TEST(LinkCounterfactual, ErasureLoadAloneMispredictsWhenErrorsArePresent) {
    // The defect that made the first version of this component recommend the wrong rung (F23),
    // isolated to a single frame so the mechanism is unmistakable.
    //
    // Codeword 3 gets 10 declared erasures and 4 undeclared errors. Budget = 2*4 + 10 = 18.
    // An erasure-only predictor sees a load of 10 and expects nsym=16 to be comfortable; the
    // real decoder needs 18 and fails.
    std::vector<std::size_t> erasures, errors;
    for (std::size_t i = 0; i < 10; ++i) erasures.push_back(i * kCodewords + 3);
    for (std::size_t i = 20; i < 24; ++i) errors.push_back(i * kCodewords + 3);

    IntraFec::Stats at32;
    EXPECT_TRUE(DecodesAt(32, erasures, errors, 11, &at32));
    EXPECT_EQ(at32.worst_erasures_in_codeword, 10u);
    EXPECT_EQ(at32.worst_budget_used, 18u) << "2 errors' worth of budget each, plus the erasures";
    EXPECT_FALSE(at32.budget_censored);

    // The erasure count says 16 is fine. The budget says it is not. The decoder agrees with
    // the budget.
    EXPECT_FALSE(DecodesAt(16, erasures, errors, 11));
    EXPECT_TRUE(DecodesAt(24, erasures, errors, 11));

    ChannelEstimator est = MakeEstimator();
    est.ObserveFrame(at32.worst_budget_used, at32.budget_censored);
    EXPECT_DOUBLE_EQ(est.FrameSuccessAt(16), 0.0);
    EXPECT_DOUBLE_EQ(est.FrameSuccessAt(24), 1.0);
}

TEST(LinkCounterfactual, CensoredBudgetIsAFloorAndSaysSo) {
    // 60 erasures in one codeword cannot be corrected at nsym=32, so the budget it needed is
    // never measured -- only bounded. The bound must be reported as a bound.
    std::vector<std::size_t> erasures;
    for (std::size_t i = 0; i < 60; ++i) erasures.push_back(i * kCodewords + 7);

    IntraFec::Stats stats;
    EXPECT_FALSE(DecodesAt(32, erasures, 5, &stats));
    EXPECT_TRUE(stats.budget_censored);
    EXPECT_EQ(stats.worst_budget_used, 33u) << "nsym + 1: all that is known is 'more than 32'";
    // The true requirement is 60, well above the floor -- which is exactly why the flag exists.
    EXPECT_TRUE(DecodesAt(64, erasures, 5));
}

TEST(LinkCounterfactual, OneRungPredictsTheWholeLadder) {
    // EXP-023 H1, end to end: observe only at a single reference rung, then predict the frame
    // success rate at every other rung and check it against brute force through the real codec.
    //
    // The reference rung is the STRONGEST on the ladder, so every frame decodes there and every
    // budget is measured rather than censored. That is the condition under which the prediction
    // is exact, and stating it as a precondition of the test is the honest way to encode it: a
    // reference rung that loses frames yields floors, and floors are optimistic upward.
    constexpr std::size_t kFrames = 160;
    constexpr std::size_t kReferenceNsym = 64;

    SplitMix64 rng(0xC14C14);
    std::vector<std::vector<std::size_t>> erasures(kFrames);
    std::vector<std::vector<std::size_t>> errors(kFrames);
    for (std::size_t i = 0; i < kFrames; ++i) {
        // Sized so worst-codeword budgets straddle the ladder: no rung's prediction is
        // trivially 0.0 or 1.0, and both damage types contribute.
        erasures[i] = ErasurePattern(&rng, rng.Below(200), 2550);
        errors[i] = ErasurePattern(&rng, rng.Below(60), 2550);
        // A position cannot be both declared and undeclared.
        std::vector<std::size_t> clean;
        for (const std::size_t p : errors[i]) {
            if (!std::binary_search(erasures[i].begin(), erasures[i].end(), p)) {
                clean.push_back(p);
            }
        }
        errors[i] = std::move(clean);
    }

    // What the receiver is allowed to know: one run, at one rung.
    ChannelEstimator est = MakeEstimator();
    for (std::size_t i = 0; i < kFrames; ++i) {
        IntraFec::Stats stats;
        DecodesAt(kReferenceNsym, erasures[i], errors[i], 1000 + i, &stats);
        est.ObserveFrame(stats.worst_budget_used, stats.budget_censored);
    }
    ASSERT_EQ(est.frames_observed(), kFrames);
    ASSERT_EQ(est.censored_observations(), 0u)
        << "the reference rung must decode everything for this prediction to be exact";

    // Ground truth: actually run every rung.
    for (const std::size_t nsym : Ladder()) {
        std::size_t decoded = 0;
        for (std::size_t i = 0; i < kFrames; ++i) {
            if (DecodesAt(nsym, erasures[i], errors[i], 1000 + i)) ++decoded;
        }
        const double measured = static_cast<double>(decoded) / static_cast<double>(kFrames);
        EXPECT_NEAR(est.FrameSuccessAt(nsym), measured, 1e-12)
            << "nsym=" << nsym << " predicted " << est.FrameSuccessAt(nsym) << " measured "
            << measured;
    }
}

// ---------------------------------------------------------------- estimation

TEST(ChannelEstimator, ReportsNothingBeforeItHasSeenAnything) {
    const ChannelEstimator est = MakeEstimator();
    EXPECT_EQ(est.frames_observed(), 0u);
    EXPECT_DOUBLE_EQ(est.fec_weight(), 0.0);
    EXPECT_DOUBLE_EQ(est.FrameSuccessAt(32), 0.0);
    // "No evidence" must not read as "everything failed": a zero here would make every rung
    // score zero and the desperate path fire on an empty estimate.
    EXPECT_DOUBLE_EQ(est.pre_fec_success(), 1.0);
    EXPECT_EQ(est.worst_budget_seen(), 0u);
    EXPECT_DOUBLE_EQ(est.mean_budget(), 0.0);
}

TEST(ChannelEstimator, ScoresTheLadderFromABudgetDistribution) {
    ChannelEstimator est = MakeEstimator();
    for (int i = 0; i < 70; ++i) est.ObserveFrame(8);
    for (int i = 0; i < 30; ++i) est.ObserveFrame(40);

    EXPECT_DOUBLE_EQ(est.FrameSuccessAt(4), 0.0);
    EXPECT_DOUBLE_EQ(est.FrameSuccessAt(8), 0.7);
    EXPECT_DOUBLE_EQ(est.FrameSuccessAt(39), 0.7);
    EXPECT_DOUBLE_EQ(est.FrameSuccessAt(40), 1.0);
    EXPECT_DOUBLE_EQ(est.FrameSuccessAt(255), 1.0);
    EXPECT_EQ(est.worst_budget_seen(), 40u);
    EXPECT_NEAR(est.mean_budget(), 0.7 * 8 + 0.3 * 40, 1e-9);
    EXPECT_EQ(est.BudgetQuantile(0.5), 8u);
    EXPECT_EQ(est.BudgetQuantile(1.0), 40u);
    EXPECT_EQ(est.censored_observations(), 0u);
}

TEST(ChannelEstimator, CountsCensoredObservationsSeparately) {
    // A frame that did not decode bounds its budget from below instead of measuring it. The
    // count has to survive to the report, because it is exactly the amount by which any
    // recommendation to STRENGTHEN the code is optimistic.
    ChannelEstimator est = MakeEstimator();
    for (int i = 0; i < 90; ++i) est.ObserveFrame(12);
    for (int i = 0; i < 10; ++i) est.ObserveFrame(33, /*censored=*/true);

    EXPECT_EQ(est.censored_observations(), 10u);
    EXPECT_EQ(est.frames_observed(), 100u);
    // The floor still predicts failure correctly at and below the rung that produced it.
    EXPECT_DOUBLE_EQ(est.FrameSuccessAt(32), 0.9);
    // Above it, the censored frames are credited with success -- the optimism, made visible.
    EXPECT_DOUBLE_EQ(est.FrameSuccessAt(33), 1.0);
}

TEST(ChannelEstimator, PreFecLossesScaleScoresWithoutChangingTheRanking) {
    // A tracking failure must not read as a code-rate problem. Parity cannot recover a frame
    // whose screen was never found, so these losses scale every rung equally and the argmax
    // must not move -- otherwise the controller spends parity on a geometry bug.
    ChannelEstimator clean = MakeEstimator();
    ChannelEstimator lossy = MakeEstimator();
    for (int i = 0; i < 70; ++i) { clean.ObserveFrame(8); lossy.ObserveFrame(8); }
    for (int i = 0; i < 30; ++i) { clean.ObserveFrame(40); lossy.ObserveFrame(40); }
    for (int i = 0; i < 100; ++i) lossy.ObservePreFecLoss();

    EXPECT_DOUBLE_EQ(clean.pre_fec_success(), 1.0);
    EXPECT_DOUBLE_EQ(lossy.pre_fec_success(), 0.5);
    EXPECT_EQ(lossy.pre_fec_losses(), 100u);

    const LinkController ctl = MakeController(32);
    EXPECT_EQ(ctl.BestRung(clean).nsym, ctl.BestRung(lossy).nsym);
    // The scores themselves are halved, because they mean "per display state presented".
    EXPECT_NEAR(ctl.BestRung(lossy).score, 0.5 * ctl.BestRung(clean).score, 1e-9);
}

TEST(ChannelEstimator, ClampsAnImpossibleBudgetReport) {
    // Fed from a decoder parsing attacker-controlled optical input. `2*errors + erasures` can
    // legitimately exceed the bin count, and a hostile frame can drive it far higher still.
    ChannelEstimator est = MakeEstimator();
    est.ObserveFrame(1'000'000);
    est.ObserveFrame(255);
    EXPECT_EQ(est.worst_budget_seen(), ChannelEstimator::kLoadBins - 1);
    EXPECT_DOUBLE_EQ(est.FrameSuccessAt(255), 1.0);
    EXPECT_DOUBLE_EQ(est.FrameSuccessAt(254), 0.0);
}

TEST(ChannelEstimator, ForgettingIsOffByDefault) {
    // Correct for the session-start decision this component actually informs: every frame of
    // the session is equally relevant to choosing one nsym for that session.
    ChannelEstimator est = MakeEstimator();
    for (int i = 0; i < 100; ++i) est.ObserveFrame(8);
    for (int i = 0; i < 100; ++i) est.ObserveFrame(40);
    EXPECT_NEAR(est.FrameSuccessAt(8), 0.5, 1e-9);
}

TEST(ChannelEstimator, ForgettingTracksAChannelThatDegrades) {
    ChannelEstimator est = MakeEstimator(/*half_life=*/32);
    for (int i = 0; i < 300; ++i) est.ObserveFrame(8);
    EXPECT_NEAR(est.FrameSuccessAt(8), 1.0, 1e-9);

    for (int i = 0; i < 300; ++i) est.ObserveFrame(40);
    // The old regime is forgotten, not averaged in.
    EXPECT_LT(est.FrameSuccessAt(8), 0.01);
    EXPECT_NEAR(est.FrameSuccessAt(40), 1.0, 1e-9);
    // The counters still record the whole history even though the distribution does not.
    EXPECT_EQ(est.frames_observed(), 600u);
}

TEST(ChannelEstimator, ResetDropsTheEstimateButKeepsTheConfiguration) {
    ChannelEstimator est = MakeEstimator();
    for (int i = 0; i < 50; ++i) est.ObserveFrame(24);
    est.Reset();
    EXPECT_EQ(est.frames_observed(), 0u);
    EXPECT_DOUBLE_EQ(est.FrameSuccessAt(24), 0.0);
    EXPECT_EQ(est.codewords_per_frame(), kCodewords);
    EXPECT_EQ(est.codeword_bytes(), 255u);
}

// ---------------------------------------------------------------- selection

TEST(LinkController, ChoosesGoodputNotTheLowestErrorRate) {
    // The failure mode the component registry names explicitly: "optimising symbol error rate
    // instead of goodput". Here 40 and every rung above it decode every frame, so a
    // reliability-maximising controller is free to pick the strongest code -- and would be
    // wrong, because the extra parity costs more payload than it saves.
    ChannelEstimator est = MakeEstimator();
    for (int i = 0; i < 70; ++i) est.ObserveFrame(8);
    for (int i = 0; i < 30; ++i) est.ObserveFrame(40);

    const LinkController ctl = MakeController(32);
    const auto scores = ctl.ScoreLadder(est);
    ASSERT_EQ(scores.size(), Ladder().size());

    // Reliability is flat from 40 upward; the score is not.
    EXPECT_DOUBLE_EQ(scores[4].predicted_frame_success, 1.0);  // nsym 40
    EXPECT_DOUBLE_EQ(scores[6].predicted_frame_success, 1.0);  // nsym 64
    EXPECT_GT(scores[4].score, scores[6].score);

    EXPECT_EQ(ctl.BestRung(est).nsym, 40u);
    EXPECT_NEAR(ctl.BestRung(est).score, 1.0 * kCodewords * (255 - 40), 1e-9);
    // 8 is cheapest per frame but loses 30% of them: 0.7 * 2470 < 1.0 * 2150.
    EXPECT_LT(scores[0].score, scores[4].score);
}

TEST(LinkController, SpendsNoParityOnAChannelThatNeedsNone) {
    ChannelEstimator est = MakeEstimator();
    for (int i = 0; i < 100; ++i) est.ObserveFrame(6);
    const LinkController ctl = MakeController(32);
    EXPECT_EQ(ctl.BestRung(est).nsym, 8u);
}

TEST(LinkController, TiesBreakTowardMoreParity) {
    // Equal scores are not equal risk: the estimate is always slightly wrong, and the stronger
    // code is the one that survives being wrong.
    //
    // The tie is made EXACT in binary floating point rather than approximate, so the test
    // exercises the `>=` comparison itself. nsym=15 carries 2400 message bytes and nsym=135
    // carries 1200 -- a ratio of exactly 2 -- so a success rate of exactly 0.5 at the weaker
    // code ties it against a perfect rate at the stronger one. 128 of 256 frames gives 0.5 with
    // no rounding anywhere.
    ChannelEstimator est = MakeEstimator();
    for (int i = 0; i < 128; ++i) est.ObserveFrame(15);
    for (int i = 0; i < 128; ++i) est.ObserveFrame(135);

    const LinkController ctl = MakeController(15, {.ladder = {15, 135}});
    const auto scores = ctl.ScoreLadder(est);
    ASSERT_EQ(scores.size(), 2u);
    ASSERT_DOUBLE_EQ(scores[0].predicted_frame_success, 0.5);
    ASSERT_DOUBLE_EQ(scores[1].predicted_frame_success, 1.0);
    ASSERT_DOUBLE_EQ(scores[0].score, scores[1].score) << "the tie must be exact, not close";
    EXPECT_EQ(ctl.BestRung(est).nsym, 135u);
}

TEST(LinkController, StaysPutUntilItHasEnoughEvidence) {
    ChannelEstimator est = MakeEstimator();
    LinkController ctl = MakeController(32, {.min_observations = 32});

    for (int i = 0; i < 31; ++i) {
        est.ObserveFrame(6);
        EXPECT_EQ(ctl.Select(est, static_cast<std::uint64_t>(1000 + i)).nsym, 32u);
    }
    EXPECT_EQ(ctl.changes(), 0u);
    est.ObserveFrame(6);
    EXPECT_NE(ctl.Select(est, 2000).nsym, 32u);
}

TEST(LinkController, RefusesToPromoteForASmallGain) {
    // A marginal channel: 97.5% of frames would survive nsym=16, all of them survive nsym=24.
    // Weakening the code buys 3.4% more capacity per frame but forfeits 2.5% of frames, netting
    // under the 2% margin -- so the code stays where it is rather than trading a resync for
    // almost nothing. This is the realistic shape of a bad promotion: not a tiny capacity gain,
    // but a capacity gain that reliability very nearly cancels.
    ChannelEstimator est = MakeEstimator();
    for (int i = 0; i < 195; ++i) est.ObserveFrame(16);
    for (int i = 0; i < 5; ++i) est.ObserveFrame(24);

    LinkController ctl = MakeController(24);
    ASSERT_EQ(ctl.BestRung(est).nsym, 16u) << "unconstrained, 16 is still the argmax";
    EXPECT_EQ(ctl.Select(est, 5000).nsym, 24u);
    EXPECT_EQ(ctl.changes(), 0u);

    // The same estimate with the margin removed does move -- so the refusal above is the
    // margin's doing, not an accident of the scenario.
    LinkController eager = MakeController(24, {.promote_margin = 0.0});
    EXPECT_EQ(eager.Select(est, 5000).nsym, 16u);
}

TEST(LinkController, PromotesOneRungAtATimeButFallsBackInOneStep) {
    // "Fall back fast, promote slowly" (MODULATION-SPEC), asserted as the asymmetry it is.
    ChannelEstimator clean = MakeEstimator();
    for (int i = 0; i < 200; ++i) clean.ObserveFrame(4);

    LinkController up = MakeController(64);
    // Target is 8, five rungs away. Each Select moves one rung, and only after the dwell.
    EXPECT_EQ(up.Select(clean, 1000).nsym, 48u);
    EXPECT_EQ(up.Select(clean, 1001).nsym, 48u) << "dwell must gate the next promotion";
    EXPECT_EQ(up.Select(clean, 1000 + 96).nsym, 40u);
    EXPECT_EQ(up.Select(clean, 1000 + 192).nsym, 32u);
    EXPECT_EQ(up.changes(), 3u);

    // Degradation, by contrast, is met in a single step to the argmax.
    ChannelEstimator bad = MakeEstimator();
    for (int i = 0; i < 90; ++i) bad.ObserveFrame(20);
    for (int i = 0; i < 10; ++i) bad.ObserveFrame(30);

    LinkController down = MakeController(24);
    EXPECT_EQ(down.Select(bad, 5).nsym, 24u) << "fallback still respects its (shorter) dwell";
    EXPECT_EQ(down.Select(bad, 100).nsym, 32u);
    EXPECT_EQ(down.changes(), 1u);
}

TEST(LinkController, IgnoresTheDwellWhenEveryFrameIsFailing) {
    // Nothing is being delivered, so waiting out a dwell is pure loss and cannot be justified
    // by the cost of a resynchronisation.
    ChannelEstimator est = MakeEstimator();
    for (int i = 0; i < 100; ++i) est.ObserveFrame(40);

    LinkController ctl = MakeController(8);
    ASSERT_DOUBLE_EQ(est.FrameSuccessAt(8), 0.0);
    EXPECT_EQ(ctl.Select(est, 0).nsym, 40u);
    EXPECT_EQ(ctl.changes(), 1u);
}

TEST(LinkController, HoldsStillOnAChannelStraddlingADecisionBoundary) {
    // EXP-023 H3. The channel is tuned so the two best rungs score within a few percent of
    // each other, and forgetting keeps the estimate wobbling across the boundary -- the exact
    // situation that makes a naive controller flap.
    constexpr std::size_t kFrames = 2000;
    // score(16) = P16 * 2390, score(24) = 1.0 * 2310; equal at P16 = 0.9665.
    constexpr double kHeavyLoadRate = 0.034;

    LinkControllerConfig tuned;  // the shipped margins
    LinkControllerConfig naive{.min_observations = 32,
                               .promote_margin = 0.0,
                               .fallback_margin = 0.0,
                               .promote_dwell_frames = 0,
                               .fallback_dwell_frames = 0};

    ChannelEstimator est_tuned = MakeEstimator(/*half_life=*/64);
    ChannelEstimator est_naive = MakeEstimator(/*half_life=*/64);
    LinkController ctl_tuned = MakeController(24, tuned);
    LinkController ctl_naive = MakeController(24, naive);

    SplitMix64 rng(0xB0);
    for (std::size_t i = 0; i < kFrames; ++i) {
        const std::size_t load = rng.NextDouble() < kHeavyLoadRate ? 24 : 16;
        est_tuned.ObserveFrame(load);
        est_naive.ObserveFrame(load);
        ctl_tuned.Select(est_tuned, i);
        ctl_naive.Select(est_naive, i);
    }

    const double per_100 =
        100.0 * static_cast<double>(ctl_tuned.changes()) / static_cast<double>(kFrames);
    EXPECT_LE(per_100, 1.0) << "H3: at most one profile change per 100 frames, saw "
                            << ctl_tuned.changes() << " in " << kFrames;

    // And the margins are what achieved it: the same channel flaps without them.
    EXPECT_GT(ctl_naive.changes(), ctl_tuned.changes() + 10)
        << "the scenario must be capable of oscillation, or H3 proves nothing";
}

TEST(LinkController, ConvergesAfterAScriptedDegradation) {
    // The registry's stated test strategy for C14: scripted channel change, verify it converges
    // and does not thrash. Clean -> severe -> clean again.
    ChannelEstimator est = MakeEstimator(/*half_life=*/48);
    LinkController ctl = MakeController(32);

    std::uint64_t frame = 0;
    auto run = [&](std::size_t load, int frames) {
        for (int i = 0; i < frames; ++i) {
            est.ObserveFrame(load);
            ctl.Select(est, frame++);
        }
    };

    run(4, 600);
    EXPECT_EQ(ctl.current().nsym, 8u) << "a clean channel should end up paying minimal parity";
    const std::uint64_t after_clean = ctl.changes();

    run(44, 600);
    EXPECT_EQ(ctl.current().nsym, 48u) << "must strengthen past a load of 44";
    // Not DOUBLE_EQ: with forgetting on, the decayed bins do not sum to the decayed total to
    // the last bit, so the rate is 1.0 only to within rounding.
    EXPECT_NEAR(est.FrameSuccessAt(ctl.current().nsym), 1.0, 1e-12);

    run(4, 1200);
    EXPECT_EQ(ctl.current().nsym, 8u) << "and come back down once the channel recovers";

    // Converged, not thrashing: the whole 2400-frame script costs a handful of changes, and
    // walking 8 -> 48 -> 8 needs at least six of them by construction.
    EXPECT_GE(ctl.changes(), 6u);
    EXPECT_LE(ctl.changes(), 20u) << "saw " << ctl.changes() << " changes (" << after_clean
                                  << " during the opening clean phase)";
}

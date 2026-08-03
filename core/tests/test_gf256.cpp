#include <fileflow/fountain.h>  // SplitMix64
#include <fileflow/gf256.h>

#include <gtest/gtest.h>

#include <numeric>
#include <vector>

using namespace fileflow;

TEST(Gf256, FieldAxioms) {
    for (int a = 0; a < 256; ++a) {
        const auto ua = static_cast<std::uint8_t>(a);
        EXPECT_EQ(gf::Mul(ua, 0), 0);
        EXPECT_EQ(gf::Mul(ua, 1), ua);
        EXPECT_EQ(gf::Add(ua, ua), 0);  // characteristic 2
    }
}

TEST(Gf256, MultiplicationIsCommutativeAndAssociative) {
    for (int a = 1; a < 256; a += 7) {
        for (int b = 1; b < 256; b += 11) {
            const auto ua = static_cast<std::uint8_t>(a);
            const auto ub = static_cast<std::uint8_t>(b);
            EXPECT_EQ(gf::Mul(ua, ub), gf::Mul(ub, ua));
            for (int c = 1; c < 256; c += 29) {
                const auto uc = static_cast<std::uint8_t>(c);
                EXPECT_EQ(gf::Mul(gf::Mul(ua, ub), uc), gf::Mul(ua, gf::Mul(ub, uc)));
            }
        }
    }
}

TEST(Gf256, InverseIsExact) {
    for (int a = 1; a < 256; ++a) {
        const auto ua = static_cast<std::uint8_t>(a);
        EXPECT_EQ(gf::Mul(ua, gf::Inv(ua)), 1) << "a=" << a;
        EXPECT_EQ(gf::Div(ua, ua), 1) << "a=" << a;
    }
}

TEST(Gf256, PowMatchesRepeatedMultiplication) {
    for (int a = 1; a < 256; a += 13) {
        const auto ua = static_cast<std::uint8_t>(a);
        std::uint8_t acc = 1;
        for (int n = 0; n < 20; ++n) {
            EXPECT_EQ(gf::Pow(ua, n), acc) << "a=" << a << " n=" << n;
            acc = gf::Mul(acc, ua);
        }
    }
}

TEST(Gf256, NegativePowIsInverse) {
    for (int a = 1; a < 256; a += 17) {
        const auto ua = static_cast<std::uint8_t>(a);
        EXPECT_EQ(gf::Mul(gf::Pow(ua, 5), gf::Pow(ua, -5)), 1);
    }
}

// ---------------------------------------------------------------- Reed-Solomon

namespace {
std::vector<std::uint8_t> Message(std::size_t n) {
    std::vector<std::uint8_t> m(n);
    for (std::size_t i = 0; i < n; ++i) m[i] = static_cast<std::uint8_t>(i * 31 + 7);
    return m;
}
}  // namespace

TEST(ReedSolomon, EncodeIsSystematic) {
    const ReedSolomon rs(16);
    const auto msg = Message(40);
    auto cw = rs.Encode(msg);
    ASSERT_TRUE(cw.ok());
    ASSERT_EQ(cw.value().size(), 56u);
    for (std::size_t i = 0; i < msg.size(); ++i) EXPECT_EQ(cw.value()[i], msg[i]);
}

TEST(ReedSolomon, CleanCodewordNeedsNoCorrection) {
    const ReedSolomon rs(16);
    auto cw = rs.Encode(Message(40));
    ASSERT_TRUE(cw.ok());
    auto data = cw.value();
    auto n = rs.Decode(data);
    ASSERT_TRUE(n.ok());
    EXPECT_EQ(n.value(), 0u);
}

TEST(ReedSolomon, CorrectsUpToItsLimit) {
    const ReedSolomon rs(16);  // corrects 8 symbol errors
    const auto msg = Message(40);

    for (std::size_t errs = 1; errs <= rs.correctable(); ++errs) {
        auto cw = rs.Encode(msg);
        ASSERT_TRUE(cw.ok());
        auto data = cw.value();

        // Spread the errors out; RS is symbol-oriented so position does not matter, but
        // spreading exercises both message and parity regions.
        for (std::size_t e = 0; e < errs; ++e) {
            data[(e * 7) % data.size()] ^= static_cast<std::uint8_t>(0xA5 + e);
        }

        auto n = rs.Decode(data);
        ASSERT_TRUE(n.ok()) << "errs=" << errs << " err=" << ErrorName(n.error());
        for (std::size_t i = 0; i < msg.size(); ++i) {
            EXPECT_EQ(data[i], msg[i]) << "errs=" << errs << " i=" << i;
        }
    }
}

TEST(ReedSolomon, CorrectsABurst) {
    // Bursts are the expected failure mode on this channel: glare, smudges and occlusions
    // damage contiguous regions (docs/research/coding-theory.md).
    const ReedSolomon rs(16);
    const auto msg = Message(40);
    auto cw = rs.Encode(msg);
    ASSERT_TRUE(cw.ok());
    auto data = cw.value();

    for (std::size_t i = 10; i < 18; ++i) data[i] ^= 0xFF;

    auto n = rs.Decode(data);
    ASSERT_TRUE(n.ok());
    EXPECT_EQ(n.value(), 8u);
    for (std::size_t i = 0; i < msg.size(); ++i) EXPECT_EQ(data[i], msg[i]);
}

TEST(ReedSolomon, ReportsUncorrectableRatherThanMiscorrecting) {
    // A code that silently returns wrong data is far worse than one that admits failure --
    // this is why a CRC sits above the FEC layer (THREAT-MODEL T5).
    const ReedSolomon rs(8);  // corrects only 4
    const auto msg = Message(40);
    auto cw = rs.Encode(msg);
    ASSERT_TRUE(cw.ok());
    auto data = cw.value();

    for (std::size_t i = 0; i < 20; ++i) data[i] ^= static_cast<std::uint8_t>(0x3C + i);

    auto n = rs.Decode(data);
    if (n.ok()) {
        // If it claims success it must actually be right. Anything else is miscorrection.
        for (std::size_t i = 0; i < msg.size(); ++i) EXPECT_EQ(data[i], msg[i]);
    } else {
        EXPECT_EQ(n.error(), Error::kUncorrectable);
    }
}

TEST(ReedSolomon, RejectsOversizedMessage) {
    const ReedSolomon rs(16);
    EXPECT_EQ(rs.Encode(Message(250)).error(), Error::kValueOutOfRange);
}

TEST(ReedSolomon, RejectsMalformedCodewordLength) {
    const ReedSolomon rs(16);
    std::vector<std::uint8_t> tiny(4, 0);
    EXPECT_EQ(rs.Decode(tiny).error(), Error::kValueOutOfRange);
}

// ---------------------------------------------------------------- erasure decoding
//
// Erasures are worth TWICE what errors are worth: 2*errors + erasures <= nsym. The receiver
// already knows which cells it could not read (NaN samples, SoftSymbol::erased, the
// photometric residual check), so passing those positions through doubles the correction power
// for free. These tests hold the decoder to that bound exactly.

TEST(ReedSolomonErasures, CorrectsTwiceAsManyErasuresAsErrors) {
    // THE headline property. nsym=16 corrects 8 errors, but 16 erasures.
    constexpr std::size_t kNsym = 16;
    const ReedSolomon rs(kNsym);
    std::vector<std::uint8_t> msg(100);
    for (std::size_t i = 0; i < msg.size(); ++i) msg[i] = static_cast<std::uint8_t>(i * 7 + 1);

    auto enc = rs.Encode(msg);
    ASSERT_TRUE(enc.ok());
    const std::vector<std::uint8_t> clean = enc.value();

    // 16 erasures -- double the 8-error limit -- must be recoverable.
    auto cw = clean;
    std::vector<std::size_t> pos;
    for (std::size_t i = 0; i < kNsym; ++i) {
        pos.push_back(i * 5);
        cw[i * 5] = 0xEE;  // contents are irrelevant; the position is what matters
    }
    auto r = rs.Decode(cw, pos);
    ASSERT_TRUE(r.ok()) << "16 erasures should be correctable with nsym=16";
    EXPECT_EQ(cw, clean);

    // The same 16 corruptions WITHOUT position information exceed the error budget.
    auto cw2 = clean;
    for (std::size_t i = 0; i < kNsym; ++i) cw2[i * 5] = 0xEE;
    EXPECT_FALSE(rs.Decode(cw2).ok()) << "16 blind errors must exceed nsym=16";
}

TEST(ReedSolomonErasures, HonoursTheCombinedBound) {
    // 2*errors + erasures <= nsym, checked right at the boundary in both directions.
    constexpr std::size_t kNsym = 16;
    const ReedSolomon rs(kNsym);
    std::vector<std::uint8_t> msg(80, 0x5A);
    auto enc = rs.Encode(msg);
    ASSERT_TRUE(enc.ok());
    const std::vector<std::uint8_t> clean = enc.value();

    // 4 errors + 8 erasures = 16. Exactly at the bound: must succeed.
    {
        auto cw = clean;
        std::vector<std::size_t> pos;
        for (std::size_t i = 0; i < 8; ++i) { pos.push_back(i); cw[i] = 0x11; }
        for (std::size_t i = 0; i < 4; ++i) cw[40 + i * 3] ^= 0x7F;  // unknown positions
        auto r = rs.Decode(cw, pos);
        ASSERT_TRUE(r.ok()) << "4 errors + 8 erasures = 16 should be correctable";
        EXPECT_EQ(cw, clean);
    }

    // 5 errors + 8 erasures = 18. Over the bound: must refuse, not miscorrect.
    {
        auto cw = clean;
        std::vector<std::size_t> pos;
        for (std::size_t i = 0; i < 8; ++i) { pos.push_back(i); cw[i] = 0x11; }
        for (std::size_t i = 0; i < 5; ++i) cw[40 + i * 3] ^= 0x7F;
        auto r = rs.Decode(cw, pos);
        // Refusing is required; silently returning wrong data is the failure that matters.
        if (r.ok()) EXPECT_EQ(cw, clean) << "decoder claimed success but produced wrong data";
    }
}

TEST(ReedSolomonErasures, DeclaringErasuresOnCleanDataIsHarmless) {
    // The demodulator may erase a cell that happened to be right. That must cost correction
    // budget but never corrupt the result.
    const ReedSolomon rs(10);
    std::vector<std::uint8_t> msg(50);
    for (std::size_t i = 0; i < msg.size(); ++i) msg[i] = static_cast<std::uint8_t>(i);
    auto enc = rs.Encode(msg);
    ASSERT_TRUE(enc.ok());
    const std::vector<std::uint8_t> clean = enc.value();

    auto cw = clean;
    const std::vector<std::size_t> pos{3, 9, 17};  // flagged, but the bytes are correct
    auto r = rs.Decode(cw, pos);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(cw, clean);
}

TEST(ReedSolomonErasures, RejectsHostileErasurePositions) {
    // Erasure positions reach the decoder from the demodulator. Out-of-range indices would
    // index past the codeword while building the locator.
    const ReedSolomon rs(8);
    std::vector<std::uint8_t> msg(40, 1);
    auto enc = rs.Encode(msg);
    ASSERT_TRUE(enc.ok());
    auto cw = enc.value();

    const std::vector<std::size_t> past_end{cw.size()};
    EXPECT_EQ(rs.Decode(cw, past_end).error(), Error::kValueOutOfRange);

    const std::vector<std::size_t> way_past{SIZE_MAX};
    EXPECT_EQ(rs.Decode(cw, way_past).error(), Error::kValueOutOfRange);

    // More erasures than parity symbols cannot be satisfied whatever the positions.
    std::vector<std::size_t> too_many;
    for (std::size_t i = 0; i < 9; ++i) too_many.push_back(i);
    EXPECT_EQ(rs.Decode(cw, too_many).error(), Error::kUncorrectable);
}

TEST(ReedSolomonErasures, DuplicatePositionsDoNotInflateTheBudget) {
    // Duplicates must not be counted twice -- that would let a caller claim more correction
    // capability than the code actually has and produce miscorrections.
    const ReedSolomon rs(8);
    std::vector<std::uint8_t> msg(40);
    for (std::size_t i = 0; i < msg.size(); ++i) msg[i] = static_cast<std::uint8_t>(i * 3);
    auto enc = rs.Encode(msg);
    ASSERT_TRUE(enc.ok());
    const std::vector<std::uint8_t> clean = enc.value();

    auto cw = clean;
    cw[5] = 0xAB;
    cw[11] = 0xCD;
    // Same two positions repeated many times.
    const std::vector<std::size_t> dupes{5, 5, 5, 11, 11, 5, 11, 11, 5};
    auto r = rs.Decode(cw, dupes);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(cw, clean);
}

TEST(ReedSolomonErasures, RandomisedAgainstTheBound) {
    // Exhaustive-ish check that the decoder is correct whenever the bound permits, and never
    // returns WRONG data when it does not.
    SplitMix64 rng(20260802);
    const ReedSolomon rs(20);

    for (int trial = 0; trial < 400; ++trial) {
        std::vector<std::uint8_t> msg(60);
        for (auto& b : msg) b = static_cast<std::uint8_t>(rng.Next() & 0xFF);
        auto enc = rs.Encode(msg);
        ASSERT_TRUE(enc.ok());
        const std::vector<std::uint8_t> clean = enc.value();

        const std::size_t f = rng.Next() % 21;             // erasures, 0..20
        const std::size_t e = rng.Next() % 6;              // errors, 0..5
        auto cw = clean;

        std::vector<std::size_t> pos;
        std::vector<bool> used(clean.size(), false);
        for (std::size_t i = 0; i < f; ++i) {
            const std::size_t p = rng.Next() % clean.size();
            if (used[p]) continue;
            used[p] = true;
            pos.push_back(p);
            cw[p] = static_cast<std::uint8_t>(rng.Next() & 0xFF);
        }
        std::size_t real_errors = 0;
        for (std::size_t i = 0; i < e; ++i) {
            const std::size_t p = rng.Next() % clean.size();
            if (used[p]) continue;
            used[p] = true;
            const auto delta = static_cast<std::uint8_t>(1 + (rng.Next() % 255));
            cw[p] = static_cast<std::uint8_t>(cw[p] ^ delta);
            ++real_errors;
        }

        auto r = rs.Decode(cw, pos);
        const bool within_bound = 2 * real_errors + pos.size() <= rs.parity_symbols();

        if (within_bound) {
            ASSERT_TRUE(r.ok()) << "trial " << trial << ": " << real_errors << " errors + "
                                << pos.size() << " erasures is within the bound";
            EXPECT_EQ(cw, clean) << "trial " << trial;
        } else if (r.ok()) {
            // Beyond the bound a decoder MAY fail; what it must never do is succeed with
            // wrong data. The syndrome re-check exists to make that impossible.
            EXPECT_EQ(cw, clean) << "trial " << trial << ": miscorrection beyond the bound";
        }
    }
}

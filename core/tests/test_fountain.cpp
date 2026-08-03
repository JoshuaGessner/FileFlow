#include <fileflow/fountain.h>

#include <gtest/gtest.h>

#include <set>
#include <vector>

using namespace fileflow;

namespace {

std::vector<std::uint8_t> Payload(std::size_t n, std::uint64_t seed = 42) {
    std::vector<std::uint8_t> v(n);
    SplitMix64 rng(seed);
    for (auto& b : v) b = static_cast<std::uint8_t>(rng.Next() & 0xFF);
    return v;
}

FountainParams Params(std::uint32_t k, std::uint32_t sym) {
    FountainParams p;
    p.source_symbols = k;
    p.symbol_size = sym;
    p.session_seed = 0xC0FFEE;
    return p;
}

}  // namespace

TEST(SplitMix64, IsDeterministicAcrossInstances) {
    // The decoder regenerates each symbol's neighbour set from its ESI alone, so this MUST
    // be reproducible or nothing decodes.
    SplitMix64 a(12345), b(12345);
    for (int i = 0; i < 1000; ++i) EXPECT_EQ(a.Next(), b.Next());
}

TEST(SplitMix64, BelowStaysInRange) {
    SplitMix64 rng(7);
    for (int i = 0; i < 10000; ++i) {
        const std::uint32_t v = rng.Below(37);
        EXPECT_LT(v, 37u);
    }
    EXPECT_EQ(rng.Below(1), 0u);
    EXPECT_EQ(rng.Below(0), 0u);
}

TEST(Fountain, ParamsRejectDegenerateValues) {
    EXPECT_EQ(Params(0, 256).Validate().error(), Error::kValueOutOfRange);
    EXPECT_EQ(Params(64, 0).Validate().error(), Error::kValueOutOfRange);

    FountainParams huge = Params(64, 256);
    huge.source_symbols = FountainParams::kMaxSourceSymbols + 1;
    EXPECT_EQ(huge.Validate().error(), Error::kValueOutOfRange);

    FountainParams bad_delta = Params(64, 256);
    bad_delta.delta = 1.5;
    EXPECT_EQ(bad_delta.Validate().error(), Error::kDegenerateParameters);
}

TEST(Fountain, IsSystematic) {
    // Source symbols first: a clean channel then decodes with almost no peeling work.
    const auto p = Params(32, 64);
    const auto data = Payload(32 * 64);
    auto enc = FountainEncoder::Create(p, data);
    ASSERT_TRUE(enc.ok());

    for (std::uint32_t i = 0; i < p.source_symbols; ++i) {
        const auto sym = enc.value().Symbol(i);
        ASSERT_EQ(sym.size(), p.symbol_size);
        for (std::uint32_t b = 0; b < p.symbol_size; ++b) {
            EXPECT_EQ(sym[b], data[i * p.symbol_size + b]) << "esi=" << i << " b=" << b;
        }
    }
}

TEST(Fountain, DecodesFromSystematicPrefixAlone) {
    const auto p = Params(64, 128);
    const auto data = Payload(64 * 128);
    auto enc = FountainEncoder::Create(p, data);
    ASSERT_TRUE(enc.ok());
    auto dec = FountainDecoder::Create(p);
    ASSERT_TRUE(dec.ok());

    for (std::uint32_t i = 0; i < p.source_symbols; ++i) {
        dec.value().Ingest(i, enc.value().Symbol(i));
    }
    ASSERT_TRUE(dec.value().complete());
    EXPECT_DOUBLE_EQ(dec.value().overhead(), 0.0);
    EXPECT_EQ(dec.value().Take().value(), data);
}

TEST(Fountain, RecoversFromRepairSymbolsAfterLoss) {
    const auto p = Params(64, 128);
    const auto data = Payload(64 * 128);
    auto enc = FountainEncoder::Create(p, data);
    ASSERT_TRUE(enc.ok());
    auto dec_r = FountainDecoder::Create(p);
    ASSERT_TRUE(dec_r.ok());
    auto& dec = dec_r.value();

    // Drop every 3rd systematic symbol, then feed repair symbols until complete.
    for (std::uint32_t i = 0; i < p.source_symbols; ++i) {
        if (i % 3 == 0) continue;
        dec.Ingest(i, enc.value().Symbol(i));
    }
    ASSERT_FALSE(dec.complete());

    std::uint32_t esi = p.source_symbols;
    while (!dec.complete() && esi < p.source_symbols * 20) {
        dec.Ingest(esi, enc.value().Symbol(esi));
        ++esi;
    }

    ASSERT_TRUE(dec.complete()) << "failed to decode after " << esi << " symbols";
    EXPECT_EQ(dec.Take().value(), data);
}

TEST(Fountain, ToleratesDuplicatesAndOutOfOrder) {
    const auto p = Params(32, 64);
    const auto data = Payload(32 * 64);
    auto enc = FountainEncoder::Create(p, data);
    ASSERT_TRUE(enc.ok());
    auto dec_r = FountainDecoder::Create(p);
    ASSERT_TRUE(dec_r.ok());
    auto& dec = dec_r.value();

    // Reverse order, every symbol delivered three times.
    for (std::uint32_t i = p.source_symbols; i-- > 0;) {
        for (int rep = 0; rep < 3; ++rep) dec.Ingest(i, enc.value().Symbol(i));
    }
    ASSERT_TRUE(dec.complete());
    EXPECT_EQ(dec.Take().value(), data);
}

TEST(Fountain, IgnoresWrongSizedSymbols) {
    const auto p = Params(16, 64);
    auto dec = FountainDecoder::Create(p);
    ASSERT_TRUE(dec.ok());
    const std::vector<std::uint8_t> wrong(7, 0xFF);
    EXPECT_FALSE(dec.value().Ingest(0, wrong));
    EXPECT_EQ(dec.value().recovered(), 0u);
}

TEST(Fountain, IncompleteTakeIsRefused) {
    const auto p = Params(16, 64);
    auto dec = FountainDecoder::Create(p);
    ASSERT_TRUE(dec.ok());
    EXPECT_EQ(dec.value().Take().error(), Error::kFountainIncomplete);
}

// Measures the Rfountain term the goodput model needs. This is the seed of EXP-012.
TEST(Fountain, RepairOnlyOverheadIsBounded) {
    const auto p = Params(128, 64);
    const auto data = Payload(128 * 64);
    auto enc = FountainEncoder::Create(p, data);
    ASSERT_TRUE(enc.ok());

    double worst = 0.0;
    for (std::uint32_t trial = 0; trial < 8; ++trial) {
        FountainParams tp = p;
        tp.session_seed = 0xC0FFEE + trial;
        auto e = FountainEncoder::Create(tp, data);
        ASSERT_TRUE(e.ok());
        auto d = FountainDecoder::Create(tp);
        ASSERT_TRUE(d.ok());

        // Skip the systematic prefix entirely: pure LT behaviour, the worst case.
        std::uint32_t esi = tp.source_symbols;
        while (!d.value().complete() && esi < tp.source_symbols * 50) {
            d.value().Ingest(esi, e.value().Symbol(esi));
            ++esi;
        }
        ASSERT_TRUE(d.value().complete()) << "trial " << trial;
        worst = std::max(worst, d.value().overhead());
    }

    // LT overhead is materially worse than RaptorQ's ~2%; that is the known, accepted cost
    // of avoiding the RaptorQ patent question (RISK-016). EXP-012 measures it properly.
    EXPECT_LT(worst, 2.0) << "LT reception overhead unexpectedly high";
}

#include <fileflow/modulation.h>

#include <fileflow/frame.h>
#include <gtest/gtest.h>

#include <algorithm>

#include <cmath>
#include <limits>
#include <vector>

using namespace fileflow;

namespace {

FrameLayout MakeLayout() {
    auto l = FrameLayout::Create(GridGeometry{120, 200}, LayoutConfig{});
    EXPECT_TRUE(l.ok());
    return std::move(l).value();
}

// Ideal channel: cell values read back exactly. This is the identity-channel case, and it
// MUST decode perfectly -- a failure here is a decoder bug, not a channel effect
// (docs/testing/SIMULATOR-PLAN.md, simulator self-validation).
std::vector<double> IdealSamples(const CellMatrix& m) {
    std::vector<double> s(m.data().size());
    for (std::size_t i = 0; i < s.size(); ++i) s[i] = m.data()[i];
    return s;
}

}  // namespace

TEST(PackBits, RoundTrips) {
    const std::vector<std::uint8_t> bytes{0x00, 0xFF, 0xA5, 0x3C, 0x81};
    std::vector<std::uint8_t> bits;
    PackBits(bytes, &bits);
    ASSERT_EQ(bits.size(), bytes.size() * 8);
    // MSB-first is fixed by the wire format.
    EXPECT_EQ(bits[8], 1);
    EXPECT_EQ(bits[16], 1);
    EXPECT_EQ(bits[17], 0);

    std::vector<std::uint8_t> back;
    UnpackBits(bits, &back);
    EXPECT_EQ(back, bytes);
}

TEST(M0, RendersStructuralCells) {
    const FrameLayout layout = MakeLayout();
    const M0Modulator mod(layout);

    CellMatrix frame(120, 200);
    const std::vector<std::uint8_t> hdr(HeaderCodec{}.coded_size(), 0xA5);
    const std::vector<std::uint8_t> pay(100, 0x5A);
    ASSERT_TRUE(mod.Render(hdr, pay, &frame).ok());

    // Markers must be present in EVERY frame -- reacquisition after occlusion cannot wait
    // for a periodic sync frame (OPTICAL-FRAME-CANDIDATES).
    bool any_marker_bright = false;
    for (std::uint32_t r = 0; r < 8; ++r) {
        for (std::uint32_t c = 0; c < 8; ++c) {
            if (frame.at(c, r) == kLevelBright) any_marker_bright = true;
        }
    }
    EXPECT_TRUE(any_marker_bright);
}

TEST(M0, RejectsOversizedPayload) {
    const FrameLayout layout = MakeLayout();
    const M0Modulator mod(layout);
    CellMatrix frame(120, 200);
    const std::vector<std::uint8_t> hdr(HeaderCodec{}.coded_size(), 0);
    const std::vector<std::uint8_t> too_big(mod.payload_capacity_bytes() + 10, 0);
    EXPECT_EQ(mod.Render(hdr, too_big, &frame).error(), Error::kValueOutOfRange);
}

TEST(M0, RejectsGridMismatch) {
    const FrameLayout layout = MakeLayout();
    const M0Modulator mod(layout);
    CellMatrix wrong(64, 64);
    const std::vector<std::uint8_t> hdr(HeaderCodec{}.coded_size(), 0);
    EXPECT_EQ(mod.Render(hdr, {}, &wrong).error(), Error::kGridMismatch);
}

TEST(M0, EstimatesReferenceFromPilotLattice) {
    const FrameLayout layout = MakeLayout();
    const M0Modulator mod(layout);

    CellMatrix frame(120, 200);
    const std::vector<std::uint8_t> hdr(HeaderCodec{}.coded_size(), 0x00);
    ASSERT_TRUE(mod.Render(hdr, {}, &frame).ok());

    const PhotometricRef ref = mod.EstimateReference(IdealSamples(frame));
    EXPECT_NEAR(ref.dark, 0.0, 1.0);
    EXPECT_NEAR(ref.bright, 255.0, 1.0);
    EXPECT_NEAR(ref.threshold(), 127.5, 2.0);
}

TEST(M0, DegenerateSeparationForcesErasures) {
    // A saturated or washed-out region is UNRECOVERABLE. It must be erased, not
    // "corrected" into confident wrong bits.
    const FrameLayout layout = MakeLayout();
    const M0Modulator mod(layout);

    std::vector<double> flat(120 * 200, 128.0);  // every cell identical
    const PhotometricRef ref = mod.EstimateReference(flat);

    SoftSymbolBuffer soft;
    mod.DemodulatePayload(flat, ref, &soft);

    std::size_t confident = 0;
    for (const auto& s : soft.symbols) {
        if (!s.erased && std::abs(s.llr) > 10) ++confident;
    }
    EXPECT_EQ(confident, 0u) << "washed-out frame produced confident bits";
}

TEST(M0, NanSampleBecomesErasureNotAGuess) {
    const FrameLayout layout = MakeLayout();
    const M0Modulator mod(layout);

    CellMatrix frame(120, 200);
    const std::vector<std::uint8_t> hdr(HeaderCodec{}.coded_size(), 0x00);
    ASSERT_TRUE(mod.Render(hdr, {}, &frame).ok());

    auto samples = IdealSamples(frame);
    const PhotometricRef ref = mod.EstimateReference(samples);

    // Occlude the first 50 payload cells.
    const auto& pay = layout.payload_cells();
    for (std::size_t i = 0; i < 50; ++i) {
        samples[pay[i]] = std::numeric_limits<double>::quiet_NaN();
    }

    SoftSymbolBuffer soft;
    mod.DemodulatePayload(samples, ref, &soft);
    for (std::size_t i = 0; i < 50; ++i) {
        EXPECT_TRUE(soft.symbols[i].erased) << "cell " << i << " was guessed, not erased";
        EXPECT_EQ(soft.symbols[i].llr, kLlrErasure);
    }
    EXPECT_FALSE(soft.symbols[100].erased);
}

TEST(M0, PayloadRoundTripsOnAnIdentityChannel) {
    const FrameLayout layout = MakeLayout();
    const M0Modulator mod(layout);
    const HeaderCodec codec;

    std::vector<std::uint8_t> payload(512);
    for (std::size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<std::uint8_t>(i * 37 + 11);
    }

    CellMatrix frame(120, 200);
    const std::vector<std::uint8_t> hdr(codec.coded_size(), 0x00);
    ASSERT_TRUE(mod.Render(hdr, payload, &frame).ok());

    const auto samples = IdealSamples(frame);
    const PhotometricRef ref = mod.EstimateReference(samples);

    SoftSymbolBuffer soft;
    mod.DemodulatePayload(samples, ref, &soft);
    const HardDecision hd = HardDecide(soft, payload.size());

    EXPECT_EQ(hd.erasures, 0u);
    EXPECT_EQ(hd.bytes, payload);
}

TEST(M0, HeaderRoundTripsThroughRenderAndDemodulate) {
    const FrameLayout layout = MakeLayout();
    const M0Modulator mod(layout);
    const HeaderCodec codec;

    FrameHeader h;
    h.session_id = 0x11223344;
    h.sequence = 4242;
    h.block_id = 2;
    h.esi = 77;
    h.payload_bytes = 128;

    auto coded = codec.Encode(h);
    ASSERT_TRUE(coded.ok());

    CellMatrix frame(120, 200);
    ASSERT_TRUE(mod.Render(coded.value(), std::vector<std::uint8_t>(128, 0x5A), &frame).ok());

    const auto samples = IdealSamples(frame);
    const PhotometricRef ref = mod.EstimateReference(samples);

    auto recovered = mod.DemodulateHeader(samples, ref, codec.coded_size());
    ASSERT_TRUE(recovered.ok()) << ErrorName(recovered.error());

    auto buf = recovered.value();
    auto decoded = codec.Decode(buf);
    ASSERT_TRUE(decoded.ok()) << ErrorName(decoded.error());
    EXPECT_EQ(decoded.value().sequence, h.sequence);
    EXPECT_EQ(decoded.value().session_id, h.session_id);
    EXPECT_EQ(decoded.value().esi, h.esi);
}

TEST(M0, LlrSignEncodesBitValue) {
    // Positive LLR => dark => bit 0. Getting this backwards would invert the whole payload
    // while still "decoding", so it is worth an explicit test.
    const FrameLayout layout = MakeLayout();
    const M0Modulator mod(layout);

    CellMatrix frame(120, 200);
    const std::vector<std::uint8_t> hdr(HeaderCodec{}.coded_size(), 0x00);
    // 0xFF => all payload bits 1 => bright cells => negative LLR
    ASSERT_TRUE(mod.Render(hdr, std::vector<std::uint8_t>(64, 0xFF), &frame).ok());

    auto samples = IdealSamples(frame);
    const PhotometricRef ref = mod.EstimateReference(samples);
    SoftSymbolBuffer soft;
    mod.DemodulatePayload(samples, ref, &soft);

    for (std::size_t i = 0; i < 64 * 8; ++i) {
        EXPECT_LT(soft.symbols[i].llr, 0) << "bright cell " << i << " should give negative LLR";
    }
}

TEST(HardDecide, MissingSymbolsBecomeErasuresNotZeros) {
    // REGRESSION. The loop used to stop at soft.symbols.size(), leaving the remaining bits at
    // zero with NO erasure flag -- handing the FEC decoder fabricated data indistinguishable
    // from real zeros. That is exactly the failure the NaN/erasure convention exists to
    // prevent, and it only bites when a frame arrives short, which is when it hurts most.
    SoftSymbolBuffer soft;
    soft.symbols.assign(24, SoftSymbol{.llr = 100, .erased = false});  // 3 bytes' worth

    const HardDecision hd = HardDecide(soft, 8);  // ask for 8 bytes, only 3 are backed
    EXPECT_EQ(hd.bytes.size(), 8u);

    // Bytes 3..7 have no symbols behind them and must be erased.
    for (std::size_t b = 3; b < 8; ++b) {
        EXPECT_NE(std::find(hd.erased_bytes.begin(), hd.erased_bytes.end(), b),
                  hd.erased_bytes.end())
            << "byte " << b << " had no symbols but was not erased";
    }
    // The backed bytes must NOT be erased.
    for (std::size_t b = 0; b < 3; ++b) {
        EXPECT_EQ(std::find(hd.erased_bytes.begin(), hd.erased_bytes.end(), b),
                  hd.erased_bytes.end())
            << "byte " << b << " was fully backed but marked erased";
    }
    EXPECT_EQ(hd.erasures, 40u);  // 5 bytes * 8 cells
}

TEST(HardDecide, ErasedBytePositionsMatchErasedCells) {
    SoftSymbolBuffer soft;
    soft.symbols.assign(32, SoftSymbol{.llr = 100, .erased = false});
    soft.symbols[0].erased = true;    // byte 0
    soft.symbols[15].erased = true;   // byte 1
    soft.symbols[31].erased = true;   // byte 3

    const HardDecision hd = HardDecide(soft, 4);
    EXPECT_EQ(hd.erasures, 3u);
    const std::vector<std::size_t> expected{0, 1, 3};
    EXPECT_EQ(hd.erased_bytes, expected) << "byte 2 was clean and must not be erased";
}

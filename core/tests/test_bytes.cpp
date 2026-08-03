// The attacker-facing surface. These tests exist because the project runs plain C++20
// with no memory-safety hedge -- every bound here is load-bearing.
#include <fileflow/bytes.h>

#include <gtest/gtest.h>

#include <limits>

using namespace fileflow;

TEST(Bytes, RoundTripsEveryWidth) {
    ByteWriter w;
    w.U8(0xAB);
    w.U16(0x1234);
    w.U24(0xABCDEF);
    w.U32(0xDEADBEEF);
    w.U64(0x0123456789ABCDEFULL);

    ByteReader r(w.data());
    EXPECT_EQ(r.U8().value(), 0xAB);
    EXPECT_EQ(r.U16().value(), 0x1234);
    EXPECT_EQ(r.U24().value(), 0xABCDEFU);
    EXPECT_EQ(r.U32().value(), 0xDEADBEEFU);
    EXPECT_EQ(r.U64().value(), 0x0123456789ABCDEFULL);
    EXPECT_TRUE(r.empty());
}

TEST(Bytes, WireFormatIsBigEndian) {
    ByteWriter w;
    w.U32(0x01020304);
    ASSERT_EQ(w.size(), 4u);
    EXPECT_EQ(w.data()[0], 0x01);
    EXPECT_EQ(w.data()[3], 0x04);
}

TEST(Bytes, ReadPastEndIsTruncatedNotUB) {
    const std::vector<std::uint8_t> buf{0x01, 0x02};
    ByteReader r(buf);
    EXPECT_TRUE(r.U16().ok());
    EXPECT_EQ(r.U8().error(), Error::kTruncated);
    EXPECT_EQ(r.U16().error(), Error::kTruncated);
    EXPECT_EQ(r.U32().error(), Error::kTruncated);
    EXPECT_EQ(r.U64().error(), Error::kTruncated);
}

TEST(Bytes, EmptyBufferIsSafe) {
    ByteReader r(std::span<const std::uint8_t>{});
    EXPECT_EQ(r.U8().error(), Error::kTruncated);
    EXPECT_EQ(r.remaining(), 0u);
}

TEST(Bytes, OversizedSpanRequestIsRejected) {
    const std::vector<std::uint8_t> buf(4, 0);
    ByteReader r(buf);
    // A hostile length field must not produce an out-of-range span.
    EXPECT_EQ(r.Bytes(std::numeric_limits<std::size_t>::max()).error(), Error::kTruncated);
    EXPECT_EQ(r.Bytes(5).error(), Error::kTruncated);
    EXPECT_TRUE(r.Bytes(4).ok());
}

TEST(Bytes, SkipIsBounded) {
    const std::vector<std::uint8_t> buf(4, 0);
    ByteReader r(buf);
    EXPECT_EQ(r.Skip(100).error(), Error::kTruncated);
    EXPECT_TRUE(r.Skip(4).ok());
    EXPECT_TRUE(r.empty());
}

TEST(Bytes, CheckedMulDetectsOverflow) {
    std::size_t out = 0;
    EXPECT_TRUE(CheckedMul(1000, 1000, &out));
    EXPECT_EQ(out, 1000000u);

    EXPECT_FALSE(CheckedMul(std::numeric_limits<std::size_t>::max(), 2, &out));
    EXPECT_FALSE(CheckedMul(std::numeric_limits<std::size_t>::max() / 2 + 1, 2, &out));

    EXPECT_TRUE(CheckedMul(0, std::numeric_limits<std::size_t>::max(), &out));
    EXPECT_EQ(out, 0u);
}

TEST(Bytes, CheckedAddDetectsOverflow) {
    std::size_t out = 0;
    EXPECT_TRUE(CheckedAdd(1, 2, &out));
    EXPECT_EQ(out, 3u);
    EXPECT_FALSE(CheckedAdd(std::numeric_limits<std::size_t>::max(), 1, &out));
}

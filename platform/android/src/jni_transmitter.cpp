// JNI marshalling for the optical frame generator (C04) driving the Android transmitter (C03).
//
// ADR-0014 again: every decision lives in portable C++ that the desktop suite exercises. This file
// marshals and owns lifetime; it decides nothing. The frame content comes from the SAME
// `FrameLayout` + `M0Modulator` + `HeaderCodec` + `IntraFec` + `FileTransmitter` the simulator
// drives, so a frame presented on a phone is the frame `ffsim` would have rendered for the same
// session -- which is what makes a real capture comparable to a simulated one at all.
//
// ON ADR-0003's "NO PER-FRAME DATA CROSSES JNI". A cell matrix is one byte per CELL, not per pixel:
// 34,560 bytes at 144x240, against 4.5 MB for the panel's 1440x3120. It is written directly into a
// caller-supplied direct `ByteBuffer`, so Kotlin uploads native memory to a GL texture without a
// copy. A non-direct buffer is REFUSED rather than silently copied.
//
// WHY CELLS AND NOT PIXELS. The renderer upscales by an exact integer factor per axis with
// GL_NEAREST, which is bit-exact rather than approximate *because* the cell pitch is required to be
// an integer number of panel pixels (DEVICE-MATRIX). So uploading cells rather than pixels costs
// nothing in fidelity and saves ~130x the bandwidth. If that integer-pitch requirement is ever
// relaxed, this shortcut stops being exact and must be revisited.
#include "jni_util.h"

#include <fileflow/frame.h>
#include <fileflow/fountain.h>
#include <fileflow/hash.h>
#include <fileflow/grid.h>
#include <fileflow/intra_fec.h>
#include <fileflow/modulation.h>
#include <fileflow/result.h>
#include <fileflow/transfer.h>

#include <jni.h>

#include <cstring>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <vector>

using fileflow::android_jni::ThrowIllegalState;

namespace {

// One transmit session. Owns everything the frame generator needs, so a frame can be produced from
// a handle and a sequence number with no per-frame allocation beyond the coder's own buffers.
struct TxHandle {
    fileflow::FrameLayout layout;
    fileflow::M0Modulator mod;
    fileflow::HeaderCodec hdr;
    std::optional<fileflow::IntraFec> fec;
    fileflow::FileTransmitter tx;
    fileflow::CellMatrix frame;
    fileflow::GridGeometry geom;
    std::vector<std::uint8_t> payload;
    std::uint32_t last_sequence = 0;

    TxHandle(fileflow::FrameLayout l, std::optional<fileflow::IntraFec> f,
             fileflow::FileTransmitter t, fileflow::GridGeometry g,
             std::vector<std::uint8_t> p)
        : layout(std::move(l)),
          mod(layout),
          hdr(),
          fec(std::move(f)),
          tx(std::move(t)),
          frame(g.cols, g.rows),
          geom(g),
          payload(std::move(p)) {}
};

TxHandle* FromJLong(jlong h) { return reinterpret_cast<TxHandle*>(h); }

}  // namespace

extern "C" {

// Signature matches app/src/main/kotlin/dev/fileflow/tx/NativeTransmitter.kt.
//
// `payload_bytes` is generated deterministically from `seed` rather than passed in: a transmit run
// needs a reproducible payload, and shipping the bytes across JNI to produce them here would be
// pointless. The receiver checks the SHA-256 it is told, so the payload only has to be reproducible
// on the sending side.
JNIEXPORT jlong JNICALL
Java_dev_fileflow_tx_NativeTransmitter_open(JNIEnv* env, jclass /*unused*/, jint cols, jint rows,
                                           jint nsym, jint payload_bytes, jlong seed) {
    if (cols <= 0 || rows <= 0 || payload_bytes <= 0) {
        ThrowIllegalState(env, "cols, rows and payloadBytes must all be positive");
        return 0;
    }

    fileflow::GridGeometry geom{static_cast<std::uint32_t>(cols), static_cast<std::uint32_t>(rows)};
    auto layout_r = fileflow::FrameLayout::Create(geom, fileflow::LayoutConfig{});
    if (!layout_r.ok()) {
        ThrowIllegalState(env, std::string("layout: ") +
                                   std::string(fileflow::ErrorName(layout_r.error())));
        return 0;
    }
    fileflow::FrameLayout layout = std::move(layout_r).value();

    const fileflow::M0Modulator probe_mod(layout);
    const fileflow::HeaderCodec probe_hdr;

    // The same check `ffsim` makes, and for the same reason: `FrameLayout::Create` validates
    // geometry but cannot know how large the CODED header is, so a grid too small for its own
    // header builds a valid-looking layout and then fails at render with a generic out-of-range
    // error. Checked here, where both facts are in scope.
    if (probe_mod.header_capacity_bytes() < probe_hdr.coded_size()) {
        ThrowIllegalState(env, "grid " + std::to_string(cols) + "x" + std::to_string(rows) +
                                   " is too small for its coded header: band holds " +
                                   std::to_string(probe_mod.header_capacity_bytes()) +
                                   " bytes, header needs " +
                                   std::to_string(probe_hdr.coded_size()));
        return 0;
    }

    std::optional<fileflow::IntraFec> fec;
    std::uint32_t symbol_size = 0;
    if (nsym > 0) {
        fileflow::IntraFecParams fp;
        fp.nsym = static_cast<std::size_t>(nsym);
        auto f = fileflow::IntraFec::Create(probe_mod.payload_capacity_bytes(), fp);
        if (!f.ok()) {
            ThrowIllegalState(env, std::string("intra-frame FEC: ") +
                                       std::string(fileflow::ErrorName(f.error())));
            return 0;
        }
        // The fountain symbol must fit the FEC layer's MESSAGE capacity, not the raw frame
        // capacity -- parity occupies the difference. Getting this wrong silently truncates every
        // symbol (and it is the constraint F20 traced the whole actuation problem back to).
        symbol_size = static_cast<std::uint32_t>(f.value().message_bytes());
        fec.emplace(std::move(f).value());
    } else {
        symbol_size = static_cast<std::uint32_t>(probe_mod.payload_capacity_bytes());
    }
    if (symbol_size == 0 || symbol_size > fileflow::FountainParams::kMaxSymbolSize) {
        ThrowIllegalState(env, "derived fountain symbol size " + std::to_string(symbol_size) +
                                   " is out of range for this grid");
        return 0;
    }

    std::vector<std::uint8_t> payload(static_cast<std::size_t>(payload_bytes));
    fileflow::SplitMix64 prng(static_cast<std::uint64_t>(seed) ^ 0xF11EF10ULL);
    for (auto& b : payload) b = static_cast<std::uint8_t>(prng.Next() & 0xFF);

    auto tx_r = fileflow::FileTransmitter::Create(payload, "tx.bin", 0x51533101, symbol_size, 64);
    if (!tx_r.ok()) {
        ThrowIllegalState(env, std::string("transmitter: ") +
                                   std::string(fileflow::ErrorName(tx_r.error())));
        return 0;
    }

    auto* handle = new (std::nothrow) TxHandle(std::move(layout), std::move(fec),
                                               std::move(tx_r).value(), geom, std::move(payload));
    if (handle == nullptr) {
        ThrowIllegalState(env, "out of memory opening the transmitter");
        return 0;
    }
    return reinterpret_cast<jlong>(handle);
}

// Renders the next display state into `dst` as one byte per cell, row-major.
//
// Returns 0 on success or a `fileflow::Error` ordinal. Not an exception: this is the per-frame path
// and it runs on the render thread, where throwing per frame would be both slow and wrong.
JNIEXPORT jint JNICALL
Java_dev_fileflow_tx_NativeTransmitter_nextFrame(JNIEnv* env, jclass /*unused*/, jlong handle,
                                                 jobject dst) {
    TxHandle* h = FromJLong(handle);
    if (h == nullptr || dst == nullptr) return static_cast<jint>(fileflow::Error::kInternal);

    auto* out = static_cast<std::uint8_t*>(env->GetDirectBufferAddress(dst));
    if (out == nullptr) return static_cast<jint>(fileflow::Error::kInternal);

    const std::size_t need = static_cast<std::size_t>(h->geom.cols) * h->geom.rows;
    if (env->GetDirectBufferCapacity(dst) < static_cast<jlong>(need)) {
        return static_cast<jint>(fileflow::Error::kTruncated);
    }

    auto fp = h->tx.NextFrame();
    h->last_sequence = fp.header.sequence;

    auto hdr_coded = h->hdr.Encode(fp.header);
    if (!hdr_coded.ok()) return static_cast<jint>(hdr_coded.error());

    std::vector<std::uint8_t> payload_coded;
    if (h->fec.has_value()) {
        auto e = h->fec->Encode(fp.data);
        if (!e.ok()) return static_cast<jint>(e.error());
        payload_coded = std::move(e).value();
    } else {
        payload_coded.assign(fp.data.begin(), fp.data.end());
    }

    if (auto s = h->mod.Render(hdr_coded.value(), payload_coded, &h->frame); !s.ok()) {
        return static_cast<jint>(s.error());
    }

    // CellMatrix already stores one byte per cell in row-major order, which is exactly the texture
    // layout the renderer wants, so this is a straight copy rather than a transform.
    std::memcpy(out, h->frame.data().data(), need);
    return 0;
}

JNIEXPORT jint JNICALL
Java_dev_fileflow_tx_NativeTransmitter_lastSequence(JNIEnv* /*env*/, jclass /*unused*/,
                                                    jlong handle) {
    TxHandle* h = FromJLong(handle);
    return h != nullptr ? static_cast<jint>(h->last_sequence) : -1;
}

// The payload hash the receiver must reproduce. Without it a completed transfer proves nothing,
// because "the bytes arrived" and "the right bytes arrived" are different claims (G3).
JNIEXPORT jstring JNICALL
Java_dev_fileflow_tx_NativeTransmitter_payloadSha256(JNIEnv* env, jclass /*unused*/, jlong handle) {
    TxHandle* h = FromJLong(handle);
    if (h == nullptr) return env->NewStringUTF("");
    const std::string hex = fileflow::ToHex(fileflow::Sha256::Of(h->payload));
    return env->NewStringUTF(hex.c_str());
}

JNIEXPORT jint JNICALL
Java_dev_fileflow_tx_NativeTransmitter_payloadCells(JNIEnv* /*env*/, jclass /*unused*/,
                                                    jlong handle) {
    TxHandle* h = FromJLong(handle);
    return h != nullptr ? static_cast<jint>(h->layout.payload_cells().size()) : -1;
}

JNIEXPORT void JNICALL
Java_dev_fileflow_tx_NativeTransmitter_close(JNIEnv* /*env*/, jclass /*unused*/, jlong handle) {
    delete FromJLong(handle);
}

}  // extern "C"

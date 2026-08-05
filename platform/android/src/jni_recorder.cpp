// JNI marshalling for the camera recorder (C05's recording mode).
//
// ADR-0014: this REUSES `harness::CaptureWriter` rather than reimplementing the bundle format in
// Kotlin. That is not convenience. F17 proved that replaying a bundle is bit-identical to live
// decode, and that is a proof about *this writer*. A second implementation would make the
// guarantee apply to the wrong code, and the very first real capture would arrive on an unproven
// path -- losing the exact property the harness was built early to establish.
//
// ON ADR-0003's "NO PER-FRAME DATA CROSSES JNI". This is a per-frame call, so it deserves an
// explicit defence rather than a footnote. The rule exists because *copying* frame-sized payloads
// across the boundary 60-120 times a second defeats the point of a native core. This passes a
// POINTER, not data: `WriteFrame` requires a direct `ByteBuffer` and reads the camera's own
// memory through `GetDirectBufferAddress`, so there is no copy and no allocation. A non-direct
// buffer is REFUSED rather than silently copied, because a silent copy is precisely the
// regression the rule is there to prevent.
//
// It remains true that the ≥120 fps path (ADR-0005) may want NDK Camera in C++ with no JNI at
// all. That is a capture-service question, not a recorder question, and it does not change
// ADR-0014.
#include "jni_util.h"

#include <fileflow/harness/capture.h>
#include <fileflow/image.h>
#include <fileflow/result.h>

#include <jni.h>

#include <memory>
#include <new>
#include <string>

using fileflow::android_jni::FieldReader;
using fileflow::android_jni::ThrowIllegalState;

namespace {

// Owns the writer across JNI calls. Handed to Kotlin as an opaque jlong: native code owns its
// memory and Kotlin holds a handle (ARCHITECTURE-OVERVIEW's Kotlin/JNI rule).
struct RecorderHandle {
    fileflow::harness::CaptureWriter writer;
    std::uint32_t width;
    std::uint32_t height;

    // Duplicate-delivery detection. THE reason this exists: the camera research notes warn that
    // a high-speed session may return the same sensor frame more than once, and that a duplicate
    // is *not* detectable from timestamps -- repeated buffers still carry distinct ones. So the
    // only sound test is whether the pixels changed.
    //
    // Sensor noise makes this decisive rather than approximate. Two genuinely distinct exposures
    // of even a perfectly static scene differ in a large fraction of their bytes, because read
    // and shot noise are per-exposure. A byte-identical frame is therefore a repeated buffer, not
    // a coincidence -- which means duplication can be measured with NO transmitter, no known
    // pattern, and nothing pointed at the camera. That is what makes EXP-007's distinctness half
    // runnable before any optical link exists.
    std::uint64_t prev_hash = 0;
    bool have_prev = false;
    std::uint32_t duplicate_frames = 0;
    std::uint64_t last_hash = 0;
};

// FNV-1a over the packed Y plane. Not cryptographic and does not need to be: it is comparing a
// frame against its immediate predecessor, where the adversary is a driver quirk rather than an
// attacker. Walks the rows honouring stride, so padding never enters the hash and a stride change
// cannot masquerade as a content change.
std::uint64_t HashYPlane(const std::uint8_t* base, int width, int height, int row_stride) {
    std::uint64_t h = 1469598103934665603ULL;
    for (int y = 0; y < height; ++y) {
        const std::uint8_t* row = base + static_cast<std::ptrdiff_t>(y) * row_stride;
        for (int x = 0; x < width; ++x) {
            h ^= row[x];
            h *= 1099511628211ULL;
        }
    }
    return h;
}

RecorderHandle* FromJLong(jlong h) { return reinterpret_cast<RecorderHandle*>(h); }

}  // namespace

extern "C" {

// Signature matches app/src/main/kotlin/dev/fileflow/capture/NativeRecorder.kt.
//
// The metadata object carries every field CAPTURE-HARNESS.md requires. Unrecorded fields stay at
// their sentinel values and `CaptureWriter::Finish` serialises them as unset, so a half-labelled
// dataset cannot quietly become a cited result (C17).
JNIEXPORT jlong JNICALL
Java_dev_fileflow_capture_NativeRecorder_open(JNIEnv* env, jclass /*unused*/, jstring bundle_dir,
                                             jobject meta_obj) {
    if (bundle_dir == nullptr || meta_obj == nullptr) {
        ThrowIllegalState(env, "bundleDir and metadata are both required");
        return 0;
    }

    const char* dir_c = env->GetStringUTFChars(bundle_dir, nullptr);
    if (dir_c == nullptr) return 0;
    const std::string dir(dir_c);
    env->ReleaseStringUTFChars(bundle_dir, dir_c);

    FieldReader m(env, meta_obj);
    fileflow::harness::CaptureMetadata meta;

    meta.sender_model = m.String("senderModel");
    meta.receiver_model = m.String("receiverModel");
    meta.os_build = m.String("osBuild");
    meta.app_commit = m.String("appCommit");
    meta.notes = m.String("notes");

    meta.display_mode = m.String("displayMode");
    meta.grid_cols = static_cast<std::uint32_t>(m.Int("gridCols"));
    meta.grid_rows = static_cast<std::uint32_t>(m.Int("gridRows"));
    meta.modulation_profile = m.String("modulationProfile");
    meta.screen_brightness = m.Double("screenBrightness");

    meta.camera_id = m.String("cameraId");
    meta.width = static_cast<std::uint32_t>(m.Int("width"));
    meta.height = static_cast<std::uint32_t>(m.Int("height"));
    meta.fps = m.Double("fps");
    meta.exposure_ns = m.Double("exposureNs");
    meta.iso = m.Double("iso");
    meta.focus_distance = m.Double("focusDistance");
    meta.white_balance = m.String("whiteBalance");

    meta.distance_cm = m.Double("distanceCm");
    meta.angle_deg = m.Double("angleDeg");
    meta.ambient_lux = m.Double("ambientLux");
    meta.motion_condition = m.String("motionCondition");

    meta.source_payload_sha256 = m.String("sourcePayloadSha256");
    meta.source_payload_bytes = static_cast<std::uint64_t>(m.Long("sourcePayloadBytes"));

    if (!m.ok()) {
        ThrowIllegalState(env, "CaptureMetadata marshalling failed: " + m.first_error());
        return 0;
    }

    auto w = fileflow::harness::CaptureWriter::Create(dir, meta);
    if (!w.ok()) {
        ThrowIllegalState(env, std::string("could not open capture bundle: ") +
                                   std::string(fileflow::ErrorName(w.error())));
        return 0;
    }

    auto* handle = new (std::nothrow) RecorderHandle{std::move(w).value(), meta.width, meta.height};
    if (handle == nullptr) {
        ThrowIllegalState(env, "out of memory opening the recorder");
        return 0;
    }
    return reinterpret_cast<jlong>(handle);
}

// Returns 0 on success, or the `fileflow::Error` ordinal. Deliberately NOT an exception: this is
// the per-frame path and throwing per dropped frame would be both slow and wrong -- a frame the
// recorder could not write is a data point, not a program error.
JNIEXPORT jint JNICALL
Java_dev_fileflow_capture_NativeRecorder_writeFrame(JNIEnv* env, jclass /*unused*/, jlong handle,
                                                    jobject y_plane, jint width, jint height,
                                                    jint row_stride) {
    RecorderHandle* h = FromJLong(handle);
    if (h == nullptr || y_plane == nullptr) {
        return static_cast<jint>(fileflow::Error::kInternal);
    }
    if (width <= 0 || height <= 0 || row_stride < width) {
        return static_cast<jint>(fileflow::Error::kValueOutOfRange);
    }

    // A non-direct buffer would force a copy, which is the thing ADR-0003 forbids on this path.
    // Refusing makes the violation loud instead of turning it into a quiet per-frame memcpy.
    auto* base = static_cast<const std::uint8_t*>(env->GetDirectBufferAddress(y_plane));
    if (base == nullptr) {
        return static_cast<jint>(fileflow::Error::kInternal);
    }

    // Bounds-check against what the buffer actually holds before constructing a view over it.
    // The camera reports stride and size; trusting them without checking is how a driver quirk
    // becomes an out-of-bounds read (INPUT-VALIDATION: bounds before use).
    const jlong capacity = env->GetDirectBufferCapacity(y_plane);
    const std::int64_t needed =
        static_cast<std::int64_t>(row_stride) * static_cast<std::int64_t>(height - 1) + width;
    if (capacity < needed) {
        return static_cast<jint>(fileflow::Error::kTruncated);
    }

    // Stride is carried through rather than assumed equal to width: the Y plane of
    // YUV_420_888 is very often padded. `CaptureWriter::WriteFrame` writes row by row, so the
    // bundle lands tightly packed and the driver's stride never reaches the file.
    const fileflow::ImageView8 img(base, width, height, row_stride);
    const fileflow::Status s = h->writer.WriteFrame(img);

    // Hashed after the write so a frame the writer refused never counts as delivered. This is a
    // second pass over the plane, which is a real cost on the per-frame path and is accepted
    // deliberately: it is only paid during a RECORDING run, where there is no decode competing
    // for the CPU, and the number it produces gates milestone 6.
    if (s.ok()) {
        const std::uint64_t hash = HashYPlane(base, width, height, row_stride);
        if (h->have_prev && hash == h->prev_hash) ++h->duplicate_frames;
        h->prev_hash = hash;
        h->last_hash = hash;
        h->have_prev = true;
    }
    return static_cast<jint>(s.error());
}

// Flushes metadata with the final frame count. A bundle is not valid until this runs (C17).
JNIEXPORT jint JNICALL
Java_dev_fileflow_capture_NativeRecorder_finish(JNIEnv* /*env*/, jclass /*unused*/, jlong handle) {
    RecorderHandle* h = FromJLong(handle);
    if (h == nullptr) return static_cast<jint>(fileflow::Error::kInternal);
    const fileflow::Status s = h->writer.Finish();
    return static_cast<jint>(s.error());
}

JNIEXPORT jint JNICALL
Java_dev_fileflow_capture_NativeRecorder_framesWritten(JNIEnv* /*env*/, jclass /*unused*/,
                                                        jlong handle) {
    RecorderHandle* h = FromJLong(handle);
    return h != nullptr ? static_cast<jint>(h->writer.frames_written()) : -1;
}

// Frames byte-identical to their immediate predecessor. Reported separately from the frame
// count because a session delivering 240 buffers/s of which half are repeats has an effective
// rate of 120, and quoting 240 would be exactly the kind of unlabelled number ADR-0012 forbids.
JNIEXPORT jint JNICALL
Java_dev_fileflow_capture_NativeRecorder_duplicateFrames(JNIEnv* /*env*/, jclass /*unused*/,
                                                          jlong handle) {
    RecorderHandle* h = FromJLong(handle);
    return h != nullptr ? static_cast<jint>(h->duplicate_frames) : -1;
}

// The most recent frame's hash, so the caller can log a per-frame trace and find WHERE the
// duplicates cluster. A duplicate every other frame means a rate lie; a burst of them means a
// stall, and those call for different fixes.
JNIEXPORT jlong JNICALL
Java_dev_fileflow_capture_NativeRecorder_lastFrameHash(JNIEnv* /*env*/, jclass /*unused*/,
                                                        jlong handle) {
    RecorderHandle* h = FromJLong(handle);
    return h != nullptr ? static_cast<jlong>(h->last_hash) : 0;
}

JNIEXPORT void JNICALL
Java_dev_fileflow_capture_NativeRecorder_close(JNIEnv* /*env*/, jclass /*unused*/, jlong handle) {
    delete FromJLong(handle);
}

// So Kotlin can turn a returned ordinal into something a bug report can carry, without
// duplicating the error table.
JNIEXPORT jstring JNICALL
Java_dev_fileflow_capture_NativeRecorder_errorName(JNIEnv* env, jclass /*unused*/, jint code) {
    const std::string_view name = fileflow::ErrorName(static_cast<fileflow::Error>(code));
    return env->NewStringUTF(std::string(name).c_str());
}

}  // extern "C"

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
};

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

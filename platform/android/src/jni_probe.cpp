// JNI marshalling for the capability probe (C02).
//
// ADR-0014: THIS FILE MAKES NO DECISIONS. It copies what Kotlin read out of
// `CameraCharacteristics` and `Display.Mode` into a `DeviceReport`, calls `fileflow::Decide`,
// and copies the resulting `DeviceProfile` back. Every judgement -- tiering, grid selection,
// whether to believe a vendor claim -- happens in `core/`, where desktop tests hold it to
// adversarial cases without needing a phone.
//
// That split is why this file is boring, and it should stay boring. If a conditional appears
// here that changes what the device is allowed to do, it belongs in `core/device.cpp`.
//
// THREADING: called once at startup, off the main thread (C02). Not on any per-frame path, so
// ADR-0003's rule that no per-frame data crosses JNI is untouched.
#include "jni_util.h"

#include <fileflow/device.h>

#include <jni.h>

#include <string>
#include <vector>

using fileflow::android_jni::EvidenceFromOrdinal;
using fileflow::android_jni::FieldReader;
using fileflow::android_jni::MakeStringList;
using fileflow::android_jni::ThrowIllegalState;

extern "C" {

// Signature matches app/src/main/kotlin/dev/fileflow/probe/NativeProbe.kt.
JNIEXPORT jobject JNICALL
Java_dev_fileflow_probe_NativeProbe_decide(JNIEnv* env, jclass /*unused*/, jobject report,
                                           jobjectArray modes) {
    if (report == nullptr) {
        ThrowIllegalState(env, "DeviceReport was null");
        return nullptr;
    }

    FieldReader r(env, report);

    fileflow::DeviceReport dr;
    dr.model = r.String("model");
    dr.soc = r.String("soc");
    dr.max_refresh_hz = r.Double("maxRefreshHz");
    dr.panel_width = static_cast<std::uint32_t>(r.Int("panelWidth"));
    dr.panel_height = static_cast<std::uint32_t>(r.Int("panelHeight"));
    dr.measured_fd = r.Double("measuredFd");
    dr.fd_evidence = EvidenceFromOrdinal(r.Int("fdEvidence"));
    dr.hardware_level = r.Int("hardwareLevel");
    dr.claims_manual_sensor = r.Bool("claimsManualSensor");
    dr.manual_sensor_evidence = EvidenceFromOrdinal(r.Int("manualSensorEvidence"));
    dr.timestamp_source_realtime = r.Bool("timestampSourceRealtime");
    dr.timestamp_evidence = EvidenceFromOrdinal(r.Int("timestampEvidence"));
    dr.rolling_shutter_skew_ns = r.Double("rollingShutterSkewNs");

    if (!r.ok()) {
        // Refuse rather than proceed on a partially-read report. A profile built from fields we
        // failed to read is worse than no profile: it would look authoritative.
        ThrowIllegalState(env, "DeviceReport marshalling failed: " + r.first_error());
        return nullptr;
    }

    if (modes != nullptr) {
        const jsize n = env->GetArrayLength(modes);
        for (jsize i = 0; i < n; ++i) {
            jobject m = env->GetObjectArrayElement(modes, i);
            if (m == nullptr) continue;
            FieldReader mr(env, m);

            fileflow::CameraMode cm;
            cm.width = static_cast<std::uint32_t>(mr.Int("width"));
            cm.height = static_cast<std::uint32_t>(mr.Int("height"));
            cm.max_fps = mr.Double("maxFps");
            cm.high_speed = mr.Bool("highSpeed");
            cm.cpu_readable = mr.Bool("cpuReadable");

            if (!mr.ok()) {
                ThrowIllegalState(env, "CameraMode marshalling failed: " + mr.first_error());
                return nullptr;
            }
            dr.camera_modes.push_back(cm);
            env->DeleteLocalRef(m);
        }
    }

    // The one call that matters. Everything above was copying.
    auto profile = fileflow::Decide(dr);
    if (!profile.ok()) {
        ThrowIllegalState(env, std::string("Decide rejected the report: ") +
                                   std::string(fileflow::ErrorName(profile.error())));
        return nullptr;
    }
    const fileflow::DeviceProfile& p = profile.value();

    jclass out_cls = env->FindClass("dev/fileflow/probe/NativeProfile");
    if (out_cls == nullptr) {
        ThrowIllegalState(env, "NativeProfile class not found");
        return nullptr;
    }
    jmethodID ctor = env->GetMethodID(out_cls, "<init>", "(IDIZZ[ILjava/util/List;)V");
    if (ctor == nullptr) {
        ThrowIllegalState(env, "NativeProfile constructor signature mismatch");
        return nullptr;
    }

    // Grids flattened to (cols, rows, cols, rows, ...): fewer JNI round trips than a list of
    // objects, and no second class to keep in sync with C++.
    jintArray grids = env->NewIntArray(static_cast<jsize>(p.supported_grids.size() * 2));
    if (grids == nullptr) return nullptr;
    {
        std::vector<jint> flat;
        flat.reserve(p.supported_grids.size() * 2);
        for (const auto& g : p.supported_grids) {
            flat.push_back(static_cast<jint>(g.cols));
            flat.push_back(static_cast<jint>(g.rows));
        }
        if (!flat.empty()) {
            env->SetIntArrayRegion(grids, 0, static_cast<jsize>(flat.size()), flat.data());
        }
    }

    jobject notes = MakeStringList(env, p.notes);
    if (notes == nullptr) {
        ThrowIllegalState(env, "could not build the notes list");
        return nullptr;
    }

    return env->NewObject(out_cls, ctor,
                          static_cast<jint>(p.tier),
                          p.usable_fd,
                          static_cast<jint>(p.selected_camera_mode),
                          p.manual_controls_usable ? JNI_TRUE : JNI_FALSE,
                          p.clock_cross_check_available ? JNI_TRUE : JNI_FALSE,
                          grids,
                          notes);
}

}  // extern "C"

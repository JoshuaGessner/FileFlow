package dev.fileflow.probe

/**
 * JNI bridge to the capability probe's decision logic (C02).
 *
 * ADR-0014: these classes are DATA CARRIERS. No judgement happens on the Kotlin side — the tier,
 * the grid list and every decision about whether to believe a vendor claim come from
 * `core/device.cpp`, which desktop tests hold to adversarial cases without needing a phone.
 *
 * ⚠ FIELD NAMES AND TYPES ARE PART OF THE ABI. `platform/android/src/jni_probe.cpp` looks these
 * up by name and signature at runtime, so a rename here is a runtime failure the compiler cannot
 * catch. The C++ side reads every field through a checked reader and throws
 * `IllegalStateException` naming the missing field rather than substituting a default — a probe
 * built from fields we failed to read would look authoritative and be fiction.
 */

/** How a capability claim was established. Ordinals must match `fileflow::Evidence`. */
enum class Evidence {
    /** Never looked. */
    UNKNOWN,
    /** The platform said so; not checked. */
    CLAIMED,
    /** We measured it and it held. */
    VERIFIED,
    /** We measured it and the platform was wrong (RISK-011). */
    REFUTED,
}

/** One camera output configuration the platform offered. Mirrors `fileflow::CameraMode`. */
class CameraModeInfo(
    @JvmField val width: Int,
    @JvmField val height: Int,
    @JvmField val maxFps: Double,
    /** Came from `getHighSpeedVideoSizes`/`Ranges`. */
    @JvmField val highSpeed: Boolean,
    /** An `ImageReader`/`YUV_420_888` path exists. */
    @JvmField val cpuReadable: Boolean,
)

/**
 * What the Android layer observed. Claims and measurements are kept separate on purpose: a device
 * that *advertises* 120 fps and one *measured* delivering 120 distinct frames are different
 * devices, and RISK-011 says the first lies often enough to matter.
 */
class DeviceReport(
    @JvmField val model: String = "",
    @JvmField val soc: String = "",
    @JvmField val maxRefreshHz: Double = 0.0,
    @JvmField val panelWidth: Int = 0,
    @JvmField val panelHeight: Int = 0,
    /** Distinct optical frames per second actually presented — Fd, not the refresh rate. */
    @JvmField val measuredFd: Double = 0.0,
    @JvmField val fdEvidence: Int = Evidence.UNKNOWN.ordinal,
    @JvmField val hardwareLevel: Int = -1,
    @JvmField val claimsManualSensor: Boolean = false,
    @JvmField val manualSensorEvidence: Int = Evidence.UNKNOWN.ordinal,
    @JvmField val timestampSourceRealtime: Boolean = false,
    @JvmField val timestampEvidence: Int = Evidence.UNKNOWN.ordinal,
    @JvmField val rollingShutterSkewNs: Double = -1.0,
)

/** Supported tier. Ordinals must match `fileflow::DeviceTier`. */
enum class DeviceTier {
    UNSUPPORTED,
    BASELINE,
    STANDARD,
    HIGH_RATE,
}

/**
 * The probe's verdict, as returned by native code.
 *
 * ⚠ The constructor signature is the JNI ABI: `(IDIZZ[ILjava/util/List;)V`. Reordering or
 * retyping a parameter breaks the bridge at runtime.
 */
class NativeProfile(
    private val tierOrdinal: Int,
    @JvmField val usableFd: Double,
    /** Index into the `CameraModeInfo` array that was passed in, or -1. */
    @JvmField val selectedCameraMode: Int,
    @JvmField val manualControlsUsable: Boolean,
    @JvmField val clockCrossCheckAvailable: Boolean,
    /** Flattened as (cols, rows, cols, rows, …) — one array beats a second bridged class. */
    @JvmField val gridsFlat: IntArray,
    /**
     * Why the probe decided what it did, in order. Populated on every path including success:
     * "why did this device get BASELINE" is the first question a bug report asks, and a tier
     * enum alone cannot answer it.
     */
    @JvmField val notes: List<String>,
) {
    val tier: DeviceTier
        get() = DeviceTier.entries.getOrElse(tierOrdinal) { DeviceTier.UNSUPPORTED }

    /** Grids as (cols, rows) pairs, densest first. */
    val grids: List<Pair<Int, Int>>
        get() = gridsFlat.toList().chunked(2).mapNotNull {
            if (it.size == 2) it[0] to it[1] else null
        }

    val canAttemptMilestone6: Boolean
        get() = tier == DeviceTier.HIGH_RATE

    override fun toString(): String = buildString {
        appendLine("tier                 $tier")
        appendLine("usable Fd            $usableFd states/s")
        appendLine("camera mode index    $selectedCameraMode")
        appendLine("manual controls      $manualControlsUsable")
        appendLine("clock cross-check    $clockCrossCheckAvailable")
        appendLine("milestone 6 testable $canAttemptMilestone6")
        appendLine("grids (densest first)")
        // All of them, untruncated. DEVICE-MATRIX's per-device grid claim -- that each charter
        // grid is integer-pitch on exactly one reference device -- is checked against this list,
        // and a truncated list cannot confirm or refute it.
        grids.forEach { (c, r) ->
            appendLine("  ${c}x$r  (${c * r} cells)")
        }
        appendLine("why:")
        notes.forEach { appendLine("  - $it") }
    }
}

object NativeProbe {
    init {
        System.loadLibrary("fileflow")
    }

    /**
     * Runs the portable decision logic. Throws [IllegalStateException] if the report could not be
     * marshalled or if native validation rejected it — refusing beats returning a profile built
     * from fields we failed to read.
     */
    @JvmStatic
    external fun decide(report: DeviceReport, modes: Array<CameraModeInfo>): NativeProfile
}

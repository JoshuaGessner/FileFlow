package dev.fileflow.probe

import android.content.Context
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraManager
import android.hardware.camera2.CameraMetadata
import android.hardware.camera2.params.StreamConfigurationMap
import android.os.Build
import android.util.Log
import android.view.Display

/**
 * Reads what the platform claims, and hands it to native code to judge (ADR-0014).
 *
 * This class is the part that genuinely can only be Kotlin: `CameraCharacteristics` and
 * `Display.Mode` have no NDK equivalent worth using here. It therefore does the *minimum* — read
 * fields, record how each was established, pass them down. Every conditional that decides what
 * the device is allowed to do lives in `core/device.cpp`.
 *
 * ⚠ NOTHING HERE IS VERIFIED. Every value is `Evidence.CLAIMED` at best, because verification
 * needs the camera and the display actually running — that is EXP-007 and EXP-006, and neither
 * has been performed. The probe's own default policy refuses to act on a claim
 * (`accept_claimed_evidence = false`), so a report from this class alone yields
 * `DeviceTier.UNSUPPORTED`. **That is correct, not a bug.** A probe that trusts is not a probe;
 * the tier arrives once [verifyFrameRate] and friends exist and have run.
 */
object CapabilityProbe {
    private const val TAG = "FileFlow.Probe"

    /**
     * Collects claims only. The result deliberately produces `UNSUPPORTED` until measurement
     * upgrades the evidence — see the class note.
     */
    fun readClaims(context: Context, cameraId: String? = null): Pair<DeviceReport, Array<CameraModeInfo>> {
        val display = displayOf(context)
        val modes = display?.supportedModes ?: emptyArray()
        val best = modes.maxByOrNull { it.refreshRate }

        val cm = context.getSystemService(Context.CAMERA_SERVICE) as? CameraManager
        val id = cameraId ?: cm?.cameraIdList?.firstOrNull { camera ->
            // Back camera by default: the receiver points at another phone's screen, and the
            // front sensor is typically lower resolution with no high-speed modes.
            cm.getCameraCharacteristics(camera)
                .get(CameraCharacteristics.LENS_FACING) == CameraMetadata.LENS_FACING_BACK
        } ?: cm?.cameraIdList?.firstOrNull()

        val chars = runCatching {
            if (cm != null && id != null) cm.getCameraCharacteristics(id) else null
        }.getOrNull()

        val report = DeviceReport(
            model = "${Build.MANUFACTURER} ${Build.MODEL}".take(120),
            soc = (Build.SOC_MODEL ?: Build.HARDWARE).take(120),
            maxRefreshHz = best?.refreshRate?.toDouble() ?: 0.0,
            panelWidth = best?.physicalWidth ?: 0,
            panelHeight = best?.physicalHeight ?: 0,

            // Fd is NOT the refresh rate and must not be inferred from it (ADR-0012, RISK-003).
            // Left at zero with UNKNOWN evidence until EXP-006 measures presented states.
            measuredFd = 0.0,
            fdEvidence = Evidence.UNKNOWN.ordinal,

            hardwareLevel = chars?.get(CameraCharacteristics.INFO_SUPPORTED_HARDWARE_LEVEL) ?: -1,
            claimsManualSensor = chars?.let(::claimsManualSensor) ?: false,
            // Claimed, never verified: RISK-011's named example is a device that advertises
            // MANUAL_SENSOR and ignores the settings.
            manualSensorEvidence = if (chars != null) Evidence.CLAIMED.ordinal
                                   else Evidence.UNKNOWN.ordinal,

            timestampSourceRealtime =
                chars?.get(CameraCharacteristics.SENSOR_INFO_TIMESTAMP_SOURCE) ==
                    CameraMetadata.SENSOR_INFO_TIMESTAMP_SOURCE_REALTIME,
            timestampEvidence = if (chars != null) Evidence.CLAIMED.ordinal
                                else Evidence.UNKNOWN.ordinal,

            // LEFT UNSET, and it cannot be otherwise (finding F25).
            //
            // `SENSOR_ROLLING_SHUTTER_SKEW` is a **CaptureResult** key, not a
            // CameraCharacteristics key `[FACT — verified against android.jar, API 35]`. A
            // startup probe therefore cannot read it at all: it arrives per-frame from an active
            // capture session. Our own research note and OQ-017 both read as though it were a
            // characteristic; corrected there.
            //
            // Consequence: skew reaches `DeviceReport` from the RECORDER, not from here, and the
            // sentinel stays negative until it does — rather than defaulting to a plausible
            // number nobody measured.
            rollingShutterSkewNs = -1.0,
        )

        return report to (chars?.let(::enumerateModes) ?: emptyArray())
    }

    /** Convenience: read claims and let native code judge them. */
    fun probe(context: Context): NativeProfile {
        val (report, modes) = readClaims(context)
        return NativeProbe.decide(report, modes)
    }

    private fun displayOf(context: Context): Display? = runCatching {
        context.display ?: (context.getSystemService(Context.DISPLAY_SERVICE)
            as? android.hardware.display.DisplayManager)?.getDisplay(Display.DEFAULT_DISPLAY)
    }.getOrNull()

    private fun claimsManualSensor(chars: CameraCharacteristics): Boolean =
        chars.get(CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES)
            ?.contains(CameraMetadata.REQUEST_AVAILABLE_CAPABILITIES_MANUAL_SENSOR) == true

    /**
     * Enumerates output configurations.
     *
     * The two families are reported SEPARATELY and never merged, because a constrained
     * high-speed session does not permit `ImageReader` output `[FACT]` — so a mode cannot be both
     * high-speed and CPU-readable. Native code drops any mode claiming both as impossible; this
     * function's job is only to avoid manufacturing such a mode itself.
     */
    private fun enumerateModes(chars: CameraCharacteristics): Array<CameraModeInfo> {
        val out = mutableListOf<CameraModeInfo>()
        val map = chars.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP)

        map?.let { addCpuModes(it, out) }
        map?.let { addHighSpeedModes(it, out) }

        Log.i(TAG, "enumerated ${out.size} camera mode(s)")
        return out.toTypedArray()
    }

    private fun addCpuModes(map: StreamConfigurationMap, out: MutableList<CameraModeInfo>) {
        // YUV_420_888: the Y plane is a full-resolution 8-bit luminance image, which is exactly
        // what the decoder wants and exactly what the capture bundle stores.
        val fmt = android.graphics.ImageFormat.YUV_420_888
        for (size in map.getOutputSizes(fmt) ?: emptyArray()) {
            val minNs = runCatching { map.getOutputMinFrameDuration(fmt, size) }.getOrDefault(0L)
            val fps = if (minNs > 0L) 1_000_000_000.0 / minNs.toDouble() else 0.0
            out += CameraModeInfo(
                width = size.width,
                height = size.height,
                maxFps = fps,
                highSpeed = false,
                cpuReadable = true,
            )
        }
    }

    private fun addHighSpeedModes(map: StreamConfigurationMap, out: MutableList<CameraModeInfo>) {
        for (size in runCatching { map.highSpeedVideoSizes }.getOrDefault(emptyArray())) {
            val ranges = runCatching { map.getHighSpeedVideoFpsRangesFor(size) }
                .getOrDefault(emptyArray())
            val top = ranges.maxOfOrNull { it.upper } ?: continue
            out += CameraModeInfo(
                width = size.width,
                height = size.height,
                maxFps = top.toDouble(),
                highSpeed = true,
                // Never true here. A high-speed session excludes ImageReader `[FACT]`; claiming
                // otherwise would fabricate a capture path that cannot exist.
                cpuReadable = false,
            )
        }
    }
}

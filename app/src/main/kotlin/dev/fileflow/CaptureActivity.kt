package dev.fileflow

import android.Manifest
import android.content.pm.PackageManager
import android.os.Bundle
import android.widget.ScrollView
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat
import dev.fileflow.capture.CameraRecorder
import java.io.File

/**
 * Runs one capture and reports what the camera actually delivered.
 *
 * This is the verification half of EXP-007. The probe (C02) reports what the camera *advertises*;
 * this reports what arrives, which is a different thing and the one the goodput model depends on.
 *
 * Launch it with the frame count and rate as intent extras so a run is scriptable and its
 * parameters land in the report rather than in someone's memory:
 *
 * ```
 * adb shell am start -n dev.fileflow/.CaptureActivity \
 *     --ei frames 300 --ei fps 60 --ei maxWidth 1920 --es notes "rigid mount, office light"
 * ```
 *
 * The bundle is written to app-private storage and pulled with `run-as`. It is the same format
 * `ffreplay` consumes, so the moment this works every analysis tool in the repo applies to real
 * frames — which is the entire point of doing it before any transmitter exists.
 */
class CaptureActivity : AppCompatActivity() {

    private lateinit var text: TextView

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        text = TextView(this).apply {
            typeface = android.graphics.Typeface.MONOSPACE
            textSize = 11f
            setPadding(24, 24, 24, 24)
            setTextIsSelectable(true)
        }
        setContentView(ScrollView(this).apply { addView(text) })

        if (ContextCompat.checkSelfPermission(this, Manifest.permission.CAMERA)
            != PackageManager.PERMISSION_GRANTED
        ) {
            // Requested properly so the app works when driven by hand. For scripted runs the
            // permission is pre-granted with `adb shell pm grant`, which avoids a UI tap standing
            // between us and a measurement.
            requestPermissions(arrayOf(Manifest.permission.CAMERA), REQ_CAMERA)
            show("waiting for CAMERA permission…")
            return
        }
        start()
    }

    override fun onRequestPermissionsResult(
        requestCode: Int,
        permissions: Array<out String>,
        grantResults: IntArray,
    ) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode == REQ_CAMERA &&
            grantResults.firstOrNull() == PackageManager.PERMISSION_GRANTED
        ) {
            start()
        } else {
            show("CAMERA permission denied — nothing can be measured without it")
        }
    }

    private fun start() {
        val frames = intent.getIntExtra("frames", 300)
        val fps = intent.getIntExtra("fps", 60)
        val maxWidth = intent.getIntExtra("maxWidth", 1920)
        val notes = intent.getStringExtra("notes")
            ?: "unspecified conditions — a capture whose conditions were not recorded is not a result"
        // `--ez write false` runs the no-write arm: same camera configuration, no disk I/O. The
        // difference between the arms is what makes a low delivered rate attributable to the
        // writer rather than to the sensor.
        val writeFrames = intent.getBooleanExtra("write", true)

        show("capturing $frames frames at $fps fps (max width $maxWidth, write=$writeFrames)…")

        // Off the main thread: the capture blocks, and a frozen UI thread would also stall the
        // callbacks it is waiting for.
        Thread {
            val dir = File(filesDir, BUNDLE_DIR)
            dir.deleteRecursively()
            val outcome = CameraRecorder(this).record(
                bundleDir = dir.absolutePath,
                frameCount = frames,
                targetFps = fps,
                maxWidth = maxWidth,
                notes = notes,
                writeFrames = writeFrames,
            )
            val report = format(outcome, dir)
            runOnUiThread { show(report) }
            emit(report, outcome)
        }.start()
    }

    private fun format(o: CameraRecorder.Outcome, dir: File): String = buildString {
        appendLine("FileFlow capture run (C05 CPU path) — EXP-007 verification half")
        appendLine("=".repeat(64))
        appendLine()
        appendLine("PROVENANCE")
        appendLine("  collected        ${java.util.Date()}")
        appendLine("  device           ${android.os.Build.MANUFACTURER} ${android.os.Build.MODEL}")
        appendLine("  fingerprint      ${android.os.Build.FINGERPRINT}")
        appendLine("  bundle           ${dir.absolutePath}")
        appendLine()

        appendLine("REQUESTED")
        appendLine("  camera           ${o.cameraId}")
        appendLine("  size             ${o.size.width}x${o.size.height}")
        appendLine("  fps              ${o.requestedFps}")
        appendLine()

        appendLine("DELIVERED  <-- the numbers that matter")
        appendLine("  disk writes      ${if (o.framesWereWritten) "ON" else "OFF (A/B arm: camera rate without writer I/O)"}")
        appendLine("  frames           ${o.framesWritten}")
        appendLine("  delivered rate   ${"%.2f".format(o.deliveredFps)} fps (from sensor timestamps)")
        // Never print an unmeasured quantity as a number. The no-write arm does not write, and
        // duplicate detection lives in the writer, so there is nothing to report -- and "0"
        // would read as "no duplicates found", which is a different and much stronger claim.
        val dupFrac = o.duplicateFraction
        if (dupFrac == null) {
            appendLine("  duplicate frames NOT MEASURED (no writes this arm — not the same as zero)")
            appendLine("  DISTINCT rate    NOT MEASURED")
        } else {
            appendLine("  duplicate frames ${o.duplicateFrames}  (${"%.4f".format(dupFrac)})")
            appendLine("  DISTINCT rate    ${"%.2f".format(o.distinctFps ?: 0.0)} fps  <-- bounds Fd, not the above")
        }
        appendLine("  worst gap        ${"%.2f".format(o.worstGapFrames)} frame periods")
        appendLine()

        appendLine("MANUAL CONTROL — read back from CaptureResult, not from the request (RISK-011)")
        appendLine("  manual requested ${o.manualRequested}")
        appendLine("  exposure         ${o.reportedExposureNs} ns")
        appendLine("  ISO              ${o.reportedIso}")
        appendLine("  focus distance   ${o.reportedFocusDistance}")
        appendLine("  AE mode          ${o.aeMode}  (0 = OFF, i.e. manual honoured)")
        appendLine("  frame duration   ${o.reportedFrameDurationNs} ns" +
                   if (o.reportedFrameDurationNs > 0) {
                       "  => ${"%.2f".format(1e9 / o.reportedFrameDurationNs)} fps ceiling"
                   } else "")
        appendLine("  EDGE_MODE        ${o.edgeMode}  (0 = OFF, requested; OQ-016)")
        appendLine("  NOISE_REDUCTION  ${o.noiseReductionMode}  (0 = OFF, requested; OQ-016)")
        appendLine()

        if (o.error != null) {
            appendLine("ERROR            ${o.error}")
            appendLine()
        }

        appendLine("-".repeat(64))
        appendLine("A duplicate is a frame BYTE-IDENTICAL to its predecessor. Sensor noise makes")
        appendLine("that decisive: two real exposures of a static scene differ in a large share of")
        appendLine("their bytes, so an identical frame is a repeated buffer. Timestamps cannot")
        appendLine("detect this — repeats still carry distinct ones.")
        appendLine()
        appendLine("This measures DELIVERY, not the optical channel. It says nothing about goodput,")
        appendLine("and Fd remains unmeasured until a transmitter presents known states (EXP-006).")
    }

    private fun emit(report: String, o: CameraRecorder.Outcome) {
        report.lineSequence().forEach { android.util.Log.i(LOG_TAG, it) }
        runCatching { File(filesDir, REPORT_FILE).writeText(report) }

        // Per-frame timestamps as a sibling of the bundle, not inside it: the bundle format is a
        // contract (C17) and this is recorder telemetry, not part of it. Kept because the
        // aggregate rate hides the shape — a steady 59 fps and a 60 fps stream with periodic
        // stalls are different problems.
        runCatching {
            val csv = StringBuilder("index,timestamp_ns,delta_ns\n")
            o.timestampsNs.forEachIndexed { i, t ->
                val d = if (i == 0) 0L else t - o.timestampsNs[i - 1]
                csv.append("$i,$t,$d\n")
            }
            File(filesDir, FRAMES_CSV).writeText(csv.toString())
        }
    }

    private fun show(s: String) { text.text = s }

    companion object {
        const val LOG_TAG = "FileFlow.Capture"
        const val BUNDLE_DIR = "capture-bundle"
        const val REPORT_FILE = "capture-report.txt"
        const val FRAMES_CSV = "capture-frames.csv"
        private const val REQ_CAMERA = 1
    }
}

package dev.fileflow

import android.Manifest
import android.content.pm.PackageManager
import android.graphics.Color
import android.os.Bundle
import android.util.Log
import android.view.ViewGroup.LayoutParams.MATCH_PARENT
import android.view.WindowManager
import android.widget.FrameLayout
import android.widget.ScrollView
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat
import dev.fileflow.aim.AimAnalyser
import dev.fileflow.aim.AimVerdict
import dev.fileflow.aim.AimView
import dev.fileflow.aim.LumaPreviewView
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
    private var aimView: AimView? = null
    private var previewView: LumaPreviewView? = null
    /** Focus the aim phase settled on; carried into the recording so it stops hunting. */
    private var lockedFocus: Float = -1f
    private var aimRecorder: CameraRecorder? = null
    private var aimThread: Thread? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        text = TextView(this).apply {
            typeface = android.graphics.Typeface.MONOSPACE
            // Legible at arm's length on a phone. The first version was 11sp monospace on the default
            // LIGHT theme, which is what an operator actually saw while trying to line up a camera
            // during a two-device run: a wall of small text and no aiming help at all.
            textSize = 13f
            setTextColor(Color.parseColor("#CFD8DC"))
            setPadding(28, 28, 28, 28)
            setTextIsSelectable(true)
        }

        // Keep the receiver's screen awake for the whole run.
        //
        // `setShowWhenLocked`/`setTurnScreenOn` are kept because camera access IS refused to a
        // background process, and an early two-device attempt failed with CAMERA_DISABLED
        // ("cannot open camera from background", proc state 20). What that proves is only that the
        // app was not foreground -- the original comment here asserted a LOCKED SCREEN was the
        // cause, which was never verified, and the operator reports both phones were unlocked. Two
        // fixes were applied at once (these flags and a wake/dismiss-keyguard step in the script), so
        // which one mattered is genuinely unknown. Kept as cheap insurance, not as a diagnosis.
        setShowWhenLocked(true)
        setTurnScreenOn(true)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)

        if (ContextCompat.checkSelfPermission(this, Manifest.permission.CAMERA)
            != PackageManager.PERMISSION_GRANTED
        ) {
            // Requested properly so the app works when driven by hand. For scripted runs the
            // permission is pre-granted with `adb shell pm grant`, which avoids a UI tap standing
            // between us and a measurement.
            requestPermissions(arrayOf(Manifest.permission.CAMERA), REQ_CAMERA)
            showReport("Waiting for camera permission…")
            return
        }
        begin()
    }

    /**
     * Aim first when asked to, then record.
     *
     * Guidance before capture is the whole point: six consecutive hardware failures were framing or
     * exposure problems (F33, F34), and a capture taken before the geometry is workable produces a
     * dataset that cannot be decoded and cannot say why.
     *
     * **Defaults ON.** It defaulted off so scripted runs would not block, and the consequence was
     * that the safety check never ran in the path actually used: a run recorded 60 frames at 1.1%
     * lit pixels, failed geometry on 55 of them, and nothing objected (F36). A scripted rig that is
     * genuinely already lined up passes `--ez aim false` explicitly, which is the right way round —
     * the assertion of readiness should be the deliberate act, not the skipping of the check.
     */
    private fun begin() {
        if (intent.getBooleanExtra("aim", true)) startAiming() else start()
    }

    private fun startAiming() {
        val cols = intent.getIntExtra("gridCols", 120)
        val rows = intent.getIntExtra("gridRows", 260)
        val v = AimView(this, cols, rows)
        aimView = v
        v.setBanner("Lining up — recording starts when this is steady")

        // A real viewfinder, with the analysis drawn over it.
        //
        // The overlay alone could not be aimed with. It shows what is WRONG but not where the camera
        // is POINTING, so lining two phones up meant moving them blind and waiting for the verdict to
        // change (F37). The preview is a second target on the same capture session, so the compositor
        // scales and colour-converts it and none of the analysis budget goes on it.
        //
        // The SAME preview class the standalone aiming screen uses.
        //
        // This screen is what the Receive button opens, so it is the one an operator actually looks
        // at -- and it was the last holdout still using a SurfaceView inside a self-sizing box. That
        // arrangement can shape a container but cannot turn the pixels inside it, so a landscape
        // sensor stream stayed squashed however the box was shaped. Four rounds of "fixed the
        // preview" landed on the standalone screen while the reported stretch was here, in the path
        // nobody was testing. Two implementations of one thing meant a fix could look verified and
        // change nothing the operator saw, so there is now exactly one (F43).
        val preview = LumaPreviewView(this)
        previewView = preview
        // `--ez overlay false` hides the schematic, so a screenshot shows the preview alone. See
        // the note in AimActivity: automated checks with the overlay present measured the overlay.
        val showOverlay = intent.getBooleanExtra("overlay", true)
        setContentView(FrameLayout(this).apply {
            setBackgroundColor(android.graphics.Color.BLACK)
            addView(preview, FrameLayout.LayoutParams(MATCH_PARENT, MATCH_PARENT))
            if (showOverlay) addView(v, FrameLayout.LayoutParams(MATCH_PARENT, MATCH_PARENT))
        })

        // No surface to wait for: the preview is painted from the frames the analyser already
        // reads, so aiming starts straight away.
        if (aimThread == null) runAiming(cols, rows, v)
    }

    private fun runAiming(cols: Int, rows: Int, v: AimView) {
        val analyser = AimAnalyser()
        val rec = CameraRecorder(this)
        aimRecorder = rec
        var readyStreak = 0
        var lastMs = 0L
        var handedOver = false

        aimThread = Thread {
            rec.record(
                bundleDir = "${filesDir.absolutePath}/aim-discard",
                frameCount = 0,
                targetFps = 30,
                maxWidth = intent.getIntExtra("maxWidth", 1920),
                notes = "aiming — not a dataset",
                writeFrames = false,
                rig = CameraRecorder.RigMetadata(gridCols = cols, gridRows = rows),
                onFrame = { buf, w, h, stride ->
                    // The schematic has to be drawn in DISPLAY space, and the rotation that puts it
                    // there is known only once the camera is open (F38).
                    if (v.sensorRotation != rec.sensorOrientation) {
                        v.sensorRotation = rec.sensorOrientation
                    }
                    // Paint the preview from this frame, then register the overlay to the
                    // rectangle the image actually occupies.
                    previewView?.let { pv ->
                        pv.submit(buf, w, h, stride, rec.sensorOrientation)
                        runOnUiThread { v.contentRect = pv.contentRect() }
                    }
                    val now = System.currentTimeMillis()
                    if (!handedOver && now - lastMs >= AIM_INTERVAL_MS) {
                        lastMs = now
                        val aim = analyser.analyse(buf, w, h, stride, cols, rows)
                        if (aim != null) {
                            // Require a STREAK rather than a single good frame. One Ready verdict
                            // between two bad ones is exactly what the Ready/TooDark oscillation
                            // looks like when the exposure window lands in the panel's blanking
                            // interval, and starting a capture on it would record the bad half.
                            readyStreak = if (aim.verdict == AimVerdict.Ready) readyStreak + 1 else 0
                            val left = (READY_STREAK - readyStreak).coerceAtLeast(0)
                            runOnUiThread {
                                v.update(aim, w, h)
                                v.setBanner(
                                    if (aim.verdict == AimVerdict.Ready) {
                                        "Steady — starting in $left…"
                                    } else "Lining up — recording starts when this is steady"
                                )
                            }
                            // Freeze the focus the aiming phase converged on, so the recording
                            // does not keep hunting and change the blur mid-dataset.
                            if (readyStreak >= READY_STREAK && lockedFocus < 0f) {
                                lockedFocus = rec.lastFocusDiopters
                            }
                            if (readyStreak >= READY_STREAK) {
                                handedOver = true
                                rec.cancel()
                            }
                        }
                    }
                },
            )
            if (handedOver) {
                runOnUiThread {
                    aimView?.setBanner("Recording…")
                    start()
                }
            }
        }.also { it.start() }
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
            begin()
        } else {
            showReport("Camera permission denied — nothing can be measured without it.")
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
        // Negative = continuous autofocus, which is the right default because it needs no knowledge
        // of the rig. Positive = reciprocal metres, locked (5.0 = 20 cm).
        // An explicit value wins; otherwise use whatever the aiming phase converged on, and only
        // fall back to continuous autofocus if aiming never ran. Locking matters because AF that
        // keeps hunting during a capture changes the optical blur mid-dataset, so frames seconds
        // apart stop being comparable -- and comparability is the entire point of a recorded bundle.
        val requestedFocus = intent.getFloatExtra("focusDiopters", -1f)
        val focusDiopters = if (requestedFocus >= 0f) requestedFocus else lockedFocus
        // 0 = derive from the frame period. Exposed so EXP-004/EXP-005 can sweep one axis at a time.
        val exposureNs = intent.getLongExtra("exposureNs", 0L)
        val iso = intent.getIntExtra("iso", CameraRecorder.DEFAULT_ISO)

        // Transmitter and rig facts, passed in because the receiver cannot discover them. `ffreplay`
        // refuses a bundle with no grid (F29), so `gridCols`/`gridRows` are what make a capture
        // decodable at all.
        val rig = CameraRecorder.RigMetadata(
            senderModel = intent.getStringExtra("senderModel") ?: "",
            displayMode = intent.getStringExtra("displayMode") ?: "",
            gridCols = intent.getIntExtra("gridCols", 0),
            gridRows = intent.getIntExtra("gridRows", 0),
            modulationProfile = intent.getStringExtra("profile") ?: "",
            screenBrightness = intent.getDoubleExtra("brightness", -1.0),
            distanceCm = intent.getDoubleExtra("distanceCm", -1.0),
            angleDeg = intent.getDoubleExtra("angleDeg", -1.0),
            ambientLux = intent.getDoubleExtra("ambientLux", -1.0),
            motionCondition = intent.getStringExtra("motion") ?: "",
            payloadSha256 = intent.getStringExtra("payloadSha256") ?: "",
            payloadBytes = intent.getLongExtra("payloadBytes", 0L),
        )

        rigEcho = if (rig.gridCols > 0 && rig.gridRows > 0) {
            "${rig.gridCols}x${rig.gridRows} from ${rig.senderModel.ifEmpty { "(sender unrecorded)" }}"
        } else {
            "(unset — ffreplay will refuse this bundle)"
        }
        aimView?.setBanner("Recording $frames frames…")
        if (aimView == null) {
            showReport("Capturing $frames frames at $fps fps (max width $maxWidth)…")
        }

        // The viewfinder stays LIVE through the recording.
        //
        // The aiming phase tears its camera session down and this one opens a fresh one, and the
        // preview used to stop dead at that handover: the operator lined the phones up, the picture
        // froze, and it stayed frozen for the whole capture until the report replaced it. Holding
        // two phones still for tens of seconds is exactly when someone needs to see that they are
        // STILL lined up, and a frozen frame is worse than no preview at all -- it looks like a
        // live one that is going well, so drift is invisible until the decode fails (F46).
        val liveAnalyser = AimAnalyser()
        val gc = intent.getIntExtra("gridCols", 120)
        val gr = intent.getIntExtra("gridRows", 260)
        var lastAimMs = 0L
        var seen = 0

        // Off the main thread: the capture blocks, and a frozen UI thread would also stall the
        // callbacks it is waiting for.
        Thread {
            val dir = File(filesDir, BUNDLE_DIR)
            dir.deleteRecursively()
            val rec = CameraRecorder(this)
            val outcome = rec.record(
                bundleDir = dir.absolutePath,
                frameCount = frames,
                targetFps = fps,
                maxWidth = maxWidth,
                notes = notes,
                writeFrames = writeFrames,
                focusDiopters = focusDiopters,
                exposureNs = exposureNs,
                iso = iso,
                rig = rig,
                onFrame = { buf, w, h, stride ->
                    // Repainting is throttled inside the view, so this is a cheap call per frame.
                    previewView?.submit(buf, w, h, stride, rec.sensorOrientation)
                    seen++
                    // Guidance as well, but far slower than the aiming phase used. The writer is
                    // the binding constraint on this path -- 32 fps with writes on against 59 with
                    // them off (F28) -- so analysis here competes directly with the thing being
                    // measured. Twice a second is enough to notice drift and correct it, and cheap
                    // enough not to change what the capture is measuring.
                    val now = System.currentTimeMillis()
                    if (now - lastAimMs >= RECORDING_AIM_INTERVAL_MS) {
                        lastAimMs = now
                        val aim = liveAnalyser.analyse(buf, w, h, stride, gc, gr)
                        val n = seen
                        runOnUiThread {
                            aimView?.let { av ->
                                if (aim != null) av.update(aim, w, h)
                                previewView?.let { av.contentRect = it.contentRect() }
                                av.setBanner(
                                    if (aim != null && aim.verdict != AimVerdict.Ready) {
                                        "Recording $n/$frames — ${aim.guidance}"
                                    } else "Recording $n/$frames — hold steady"
                                )
                            }
                        }
                    }
                },
            )
            val report = format(outcome, dir)
            runOnUiThread { showReport(report) }
            emit(report, outcome)
        }.start()
    }

    private var rigEcho: String = "(unset — ffreplay will refuse this bundle)"

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
        appendLine("  grid (from TX)   ${rigEcho}")
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
        appendLine("  focus distance   ${o.reportedFocusDistance} diopters" +
                   if (o.reportedFocusDistance > 0f) {
                       "  => ${"%.1f".format(100f / o.reportedFocusDistance)} cm"
                   } else if (o.reportedFocusDistance == 0f) "  => INFINITY" else "")
        appendLine("  lens min focus   ${o.minFocusDiopters} diopters" +
                   if (o.minFocusDiopters > 0f) {
                       "  => closest ${"%.1f".format(100f / o.minFocusDiopters)} cm"
                   } else "")
        appendLine("  AE mode          ${o.aeMode}  (0 = OFF, i.e. manual honoured)")
        appendLine("  frame duration   ${o.reportedFrameDurationNs} ns" +
                   if (o.reportedFrameDurationNs > 0) {
                       "  => ${"%.2f".format(1e9 / o.reportedFrameDurationNs)} fps ceiling"
                   } else "")
        appendLine("  EDGE_MODE        ${o.edgeMode}  (0 = OFF, requested; OQ-016)")
        appendLine("  NOISE_REDUCTION  ${o.noiseReductionMode}  (0 = OFF, requested; OQ-016)")
        // Requested ON, unlike the two above: the lens's vignette is a smooth multiplicative field
        // the receiver has to fight, and correcting it is the ISP doing something useful (RISK-025).
        appendLine("  SHADING_MODE     ${o.shadingMode}  (2 = HIGH_QUALITY, requested; RISK-025)")
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

    /** Swap to the report view. Dark and readable, because it is read on a phone in a room. */
    private fun showReport(s: String) {
        text.text = s
        setContentView(
            ScrollView(this).apply {
                setBackgroundColor(Color.parseColor("#101418"))
                addView(text)
            }
        )
        aimView = null
    }

    override fun onDestroy() {
        super.onDestroy()
        aimRecorder?.cancel()
        aimThread?.join(2000)
    }

    companion object {
        const val LOG_TAG = "FileFlow.Capture"
        const val BUNDLE_DIR = "capture-bundle"
        const val REPORT_FILE = "capture-report.txt"
        const val FRAMES_CSV = "capture-frames.csv"
        private const val REQ_CAMERA = 1
        private const val AIM_INTERVAL_MS = 150L
        /** Guidance cadence WHILE recording. Slower than aiming: see the note in [start]. */
        private const val RECORDING_AIM_INTERVAL_MS = 500L
        /** Consecutive Ready verdicts before recording starts. See the note in [startAiming]. */
        private const val READY_STREAK = 6
    }
}

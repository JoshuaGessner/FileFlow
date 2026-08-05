package dev.fileflow

import android.Manifest
import android.content.pm.PackageManager
import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.os.Bundle
import android.util.Log
import android.view.View
import android.view.WindowManager
import android.widget.FrameLayout
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat
import dev.fileflow.aim.Aim
import dev.fileflow.aim.AimAnalyser
import dev.fileflow.aim.AimVerdict
import dev.fileflow.capture.CameraRecorder

/**
 * Live aiming guidance (feature UI-02): point the receiver at the transmitter, with feedback.
 *
 * ```
 * adb shell am start -n dev.fileflow/.AimActivity --ei gridCols 120 --ei gridRows 260
 * ```
 *
 * ## Why a schematic and not a camera preview
 *
 * It draws the *analysis* — where the screen sits inside the frame, which edge is clipping, how big
 * it is — rather than the camera image. That is a deliberate choice, not a shortcut:
 *
 *  - It shows the thing that actually decides success. Five hardware failures were all framing, and
 *    every one is obvious in a schematic and easy to miss in a live image (F33). "The bright rectangle
 *    is touching the top edge" is precisely the fact a user needs.
 *  - It needs no YUV-to-RGB conversion of a multi-megapixel frame per displayed frame, so the
 *    analysis budget goes on analysis.
 *  - It cannot mislead by looking plausible. A preview of a badly overexposed screen looks like a
 *    bright screen; the schematic says "the dark cells are washing out".
 *
 * ## What it does NOT do
 *
 * It does not decode. A green verdict means the geometry is workable, not that a transfer will
 * succeed — `Fd`, `Pc` and the density limit are all still unmeasured on real hardware.
 */
class AimActivity : AppCompatActivity() {

    private var recorder: CameraRecorder? = null
    private var view: AimView? = null
    private var worker: Thread? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        // The receiver's own screen must stay awake, and camera access is refused to a background
        // process — an activity launched onto a locked device is exactly that (F33).
        setShowWhenLocked(true)
        setTurnScreenOn(true)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)

        val cols = intent.getIntExtra("gridCols", 120)
        val rows = intent.getIntExtra("gridRows", 260)
        val maxWidth = intent.getIntExtra("maxWidth", 1920)

        val v = AimView(this, cols, rows)
        view = v
        setContentView(FrameLayout(this).apply { addView(v) })

        if (ContextCompat.checkSelfPermission(this, Manifest.permission.CAMERA)
            != PackageManager.PERMISSION_GRANTED
        ) {
            requestPermissions(arrayOf(Manifest.permission.CAMERA), REQ_CAMERA)
            v.setMessage("Waiting for camera permission…")
            return
        }
        start(cols, rows, maxWidth)
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
            start(
                intent.getIntExtra("gridCols", 120),
                intent.getIntExtra("gridRows", 260),
                intent.getIntExtra("maxWidth", 1920),
            )
        } else {
            view?.setMessage("Camera permission denied — nothing can be measured without it.")
        }
    }

    private fun start(cols: Int, rows: Int, maxWidth: Int) {
        val analyser = AimAnalyser()
        val rec = CameraRecorder(this)
        recorder = rec

        worker = Thread {
            // Analysis is throttled rather than run on every frame: guidance a user can act on does
            // not need 60 Hz, and a full-frame pass at 60 Hz would compete with the capture it is
            // meant to be advising about.
            var lastAnalysisMs = 0L
            var lastVerdict: AimVerdict? = null
            val outcome = rec.record(
                bundleDir = "${filesDir.absolutePath}/aim-discard",
                frameCount = 0,                 // unbounded: runs until the activity stops it
                targetFps = 30,
                maxWidth = maxWidth,
                notes = "aiming session — not a dataset",
                writeFrames = false,            // nothing is recorded; this is guidance only
                rig = CameraRecorder.RigMetadata(gridCols = cols, gridRows = rows),
                onFrame = { buf, w, h, stride ->
                    val now = System.currentTimeMillis()
                    if (now - lastAnalysisMs >= ANALYSE_INTERVAL_MS) {
                        lastAnalysisMs = now
                        val aim = analyser.analyse(buf, w, h, stride, cols, rows)
                        if (aim != null) {
                            // Logged on CHANGE only. A line per analysed frame would bury the
                            // transitions, and the transitions are the diagnostic: what the aim was
                            // when it stopped being workable is the question a field report asks.
                            if (aim.verdict != lastVerdict) {
                                lastVerdict = aim.verdict
                                Log.i(
                                    TAG,
                                    "%s | px/cell %.1f rot %.0f lit %.3f mid %.2f mean %.0f | %s"
                                        .format(
                                            aim.verdict, aim.pxPerCell, aim.rotationDeg,
                                            aim.litFraction, aim.midFraction, aim.meanLuminance,
                                            aim.guidance,
                                        ),
                                )
                            }
                            runOnUiThread { view?.update(aim, w, h) }
                        }
                    }
                },
            )
            if (outcome.error != null) {
                Log.e(TAG, "aim session ended: ${outcome.error}")
                runOnUiThread { view?.setMessage("Camera error: ${outcome.error}") }
            }
        }.also { it.start() }
    }

    override fun onStop() {
        super.onStop()
        recorder?.cancel()
    }

    override fun onDestroy() {
        super.onDestroy()
        recorder?.cancel()
        worker?.join(2000)
    }

    companion object {
        private const val TAG = "FileFlow.Aim"
        private const val REQ_CAMERA = 2
        private const val ANALYSE_INTERVAL_MS = 150L  // ~6 verdicts a second
    }
}

/**
 * Draws the frame, the detected screen inside it, and the guidance.
 *
 * The colour is the verdict, so the state is readable at arm's length without reading words — which
 * is the situation a user is actually in while holding two phones.
 */
private class AimView(context: Context, private val cols: Int, private val rows: Int) :
    View(context) {

    private var aim: Aim? = null
    private var frameW = 0
    private var frameH = 0
    private var message: String? = "Starting camera…"

    private val framePaint = Paint().apply {
        style = Paint.Style.STROKE
        strokeWidth = 4f
        color = Color.DKGRAY
        isAntiAlias = true
    }
    private val screenPaint = Paint().apply { style = Paint.Style.FILL; isAntiAlias = true }
    private val edgePaint = Paint().apply {
        style = Paint.Style.STROKE
        strokeWidth = 12f
        color = Color.RED
        isAntiAlias = true
    }
    private val bigText = Paint().apply {
        color = Color.WHITE
        textSize = 54f
        isAntiAlias = true
        isFakeBoldText = true
    }
    private val smallText = Paint().apply { color = Color.LTGRAY; textSize = 32f; isAntiAlias = true }

    fun setMessage(m: String) {
        message = m
        aim = null
        postInvalidate()
    }

    fun update(a: Aim, w: Int, h: Int) {
        aim = a
        frameW = w
        frameH = h
        message = null
        invalidate()
    }

    override fun onDraw(canvas: Canvas) {
        canvas.drawColor(Color.BLACK)
        val a = aim
        if (a == null) {
            canvas.drawText(message ?: "", 40f, height / 2f, bigText)
            return
        }

        // Lay the camera frame out landscape-in-portrait, top half of the screen.
        val pad = 40f
        val availW = width - 2 * pad
        val availH = height * 0.52f - 2 * pad
        val scale = minOf(availW / frameW.coerceAtLeast(1), availH / frameH.coerceAtLeast(1))
        val fw = frameW * scale
        val fh = frameH * scale
        val fx = (width - fw) / 2f
        val fy = pad

        canvas.drawRect(fx, fy, fx + fw, fy + fh, framePaint)

        // The detected screen, coloured by verdict.
        screenPaint.color = when (a.verdict) {
            AimVerdict.Ready -> Color.parseColor("#2E7D32")
            AimVerdict.Clipped -> Color.parseColor("#C62828")
            AimVerdict.TooFar, AimVerdict.NoScreenFound -> Color.parseColor("#455A64")
            AimVerdict.TooBright, AimVerdict.TooDark -> Color.parseColor("#EF6C00")
            AimVerdict.Blurred -> Color.parseColor("#6A1B9A")
            AimVerdict.Unknown -> Color.DKGRAY
        }
        if (a.bboxW > 0 && a.bboxH > 0) {
            canvas.drawRect(
                fx + a.bboxX * scale, fy + a.bboxY * scale,
                fx + (a.bboxX + a.bboxW) * scale, fy + (a.bboxY + a.bboxH) * scale,
                screenPaint,
            )
        }

        // Mark the offending edges. This is the single most actionable thing on screen: it names
        // which way to move, which a decode log never could.
        if (a.clippedTop) canvas.drawLine(fx, fy, fx + fw, fy, edgePaint)
        if (a.clippedBottom) canvas.drawLine(fx, fy + fh, fx + fw, fy + fh, edgePaint)
        if (a.clippedLeft) canvas.drawLine(fx, fy, fx, fy + fh, edgePaint)
        if (a.clippedRight) canvas.drawLine(fx + fw, fy, fx + fw, fy + fh, edgePaint)

        // Guidance, wrapped by hand — a sentence is more useful than a code, and it must not run off.
        var ty = fy + fh + 80f
        for (line in wrap(a.guidance, 34)) {
            canvas.drawText(line, pad, ty, bigText)
            ty += 62f
        }

        ty += 24f
        // The evidence behind the verdict. Kept visible because a user who can see px/cell rise as
        // they move learns the rig far faster than one shown only a red light.
        val facts = listOf(
            "grid ${cols}x$rows   frame ${frameW}x$frameH",
            "px/cell %.1f   rotation %.0f°".format(a.pxPerCell, a.rotationDeg),
            "screen %.0f%% of frame   mid %.2f".format(a.litFraction * 100.0, a.midFraction),
            "mean luminance %.0f   threshold %d".format(a.meanLuminance, a.threshold),
            if (a.bboxInflation > 1.15) {
                "tilt costs %.2fx the frame area it needs".format(a.bboxInflation)
            } else "",
        )
        for (f in facts) {
            if (f.isEmpty()) continue
            canvas.drawText(f, pad, ty, smallText)
            ty += 42f
        }
    }

    private fun wrap(s: String, width: Int): List<String> {
        val out = ArrayList<String>()
        var line = StringBuilder()
        for (word in s.split(' ')) {
            if (line.isNotEmpty() && line.length + 1 + word.length > width) {
                out.add(line.toString())
                line = StringBuilder()
            }
            if (line.isNotEmpty()) line.append(' ')
            line.append(word)
        }
        if (line.isNotEmpty()) out.add(line.toString())
        return out
    }
}

package dev.fileflow.aim

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.view.View

/**
 * Draws the aiming analysis: the camera frame, the detected screen inside it, and what to change.
 *
 * ## Why a schematic and not a camera preview
 *
 * It draws the *analysis*, not the image. That is deliberate:
 *
 *  - It shows the fact that decides success. Six consecutive hardware failures were all framing or
 *    exposure, and each is obvious here and easy to miss in a live image (F33, F34).
 *  - No per-frame YUV-to-RGB conversion of a multi-megapixel frame, so the budget goes on analysis.
 *  - It cannot mislead by looking plausible. A preview of a badly overexposed screen just looks like a
 *    bright screen; this says the dark cells are washing out.
 *
 * Shared by the standalone aiming screen and the capture flow, so a user gets the same picture
 * whichever way they arrive at it.
 */
class AimView(context: Context, private var cols: Int, private var rows: Int) : View(context) {

    private var aim: Aim? = null
    private var frameW = 0
    private var frameH = 0
    private var message: String? = "Starting camera…"
    private var banner: String? = null

    private val framePaint = Paint().apply {
        style = Paint.Style.STROKE
        strokeWidth = 4f
        color = Color.parseColor("#555555")
        isAntiAlias = true
    }
    private val screenPaint = Paint().apply { style = Paint.Style.FILL; isAntiAlias = true }
    private val edgePaint = Paint().apply {
        style = Paint.Style.STROKE
        strokeWidth = 14f
        color = Color.parseColor("#FF5252")
        isAntiAlias = true
    }
    private val bigText = Paint().apply {
        color = Color.WHITE
        textSize = 52f
        isAntiAlias = true
        isFakeBoldText = true
    }
    private val bannerText = Paint().apply {
        color = Color.parseColor("#80CBC4")
        textSize = 40f
        isAntiAlias = true
        isFakeBoldText = true
    }
    private val smallText = Paint().apply {
        color = Color.parseColor("#B0BEC5")
        textSize = 31f
        isAntiAlias = true
    }

    fun setGrid(c: Int, r: Int) {
        cols = c
        rows = r
    }

    fun setMessage(m: String) {
        message = m
        aim = null
        postInvalidate()
    }

    /** A line above the guidance, for flow state such as "recording" or "hold still to start". */
    fun setBanner(b: String?) {
        banner = b
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
        canvas.drawColor(Color.parseColor("#101418"))
        val a = aim
        if (a == null) {
            canvas.drawText(message ?: "", 40f, height / 2f, bigText)
            return
        }

        val pad = 40f
        val availW = width - 2 * pad
        val availH = height * 0.46f
        val scale = minOf(availW / frameW.coerceAtLeast(1), availH / frameH.coerceAtLeast(1))
        val fw = frameW * scale
        val fh = frameH * scale
        val fx = (width - fw) / 2f
        val fy = pad + 60f

        banner?.let { canvas.drawText(it, pad, pad + 40f, bannerText) }
        canvas.drawRect(fx, fy, fx + fw, fy + fh, framePaint)

        screenPaint.color = when (a.verdict) {
            AimVerdict.Ready -> Color.parseColor("#2E7D32")
            AimVerdict.Clipped -> Color.parseColor("#C62828")
            AimVerdict.TooFar, AimVerdict.NoScreenFound -> Color.parseColor("#37474F")
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

        // Mark the offending edges. The single most actionable thing on screen: it names which way to
        // move, which a decode log never could.
        if (a.clippedTop) canvas.drawLine(fx, fy, fx + fw, fy, edgePaint)
        if (a.clippedBottom) canvas.drawLine(fx, fy + fh, fx + fw, fy + fh, edgePaint)
        if (a.clippedLeft) canvas.drawLine(fx, fy, fx, fy + fh, edgePaint)
        if (a.clippedRight) canvas.drawLine(fx + fw, fy, fx + fw, fy + fh, edgePaint)

        var ty = fy + fh + 76f
        for (line in wrap(a.guidance, 32)) {
            canvas.drawText(line, pad, ty, bigText)
            ty += 60f
        }

        ty += 20f
        // The evidence behind the verdict. Kept visible because a user who watches px/cell rise as
        // they move learns the rig far faster than one shown only a coloured light.
        val facts = listOf(
            "grid ${cols}x$rows    frame ${frameW}x$frameH",
            "px/cell %.1f    rotation %.0f°".format(a.pxPerCell, a.rotationDeg),
            "screen %.0f%% of frame    blur %.2f".format(a.litFraction * 100.0, a.midFraction),
            "brightness %.0f    threshold %d".format(a.meanLuminance, a.threshold),
            if (a.bboxInflation > 1.15) {
                "tilt needs %.2fx the frame area".format(a.bboxInflation)
            } else "",
        )
        for (f in facts) {
            if (f.isEmpty()) continue
            canvas.drawText(f, pad, ty, smallText)
            ty += 40f
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

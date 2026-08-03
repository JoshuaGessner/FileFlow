package dev.fileflow

import android.os.Bundle
import android.widget.ScrollView
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import dev.fileflow.probe.CapabilityProbe

/**
 * Runs the capability probe and shows its verdict. Deliberately the whole UI for now.
 *
 * The shortest path to real data is: Gradle/NDK skeleton → capability probe → camera recorder.
 * There is no transmitter, no UI flow and no live link here, and adding them before the probe has
 * run on hardware would be building on unmeasured ground (EXP-006, EXP-007).
 *
 * ⚠ This screen will report `UNSUPPORTED` on every device today, and that is the correct answer.
 * `CapabilityProbe.readClaims` gathers claims only; the probe's policy refuses to act on an
 * unverified claim (RISK-011), so no tier is granted until measurement upgrades the evidence. The
 * notes list explains it on screen rather than leaving the reader to guess.
 */
class ProbeActivity : AppCompatActivity() {

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        val text = TextView(this).apply {
            typeface = android.graphics.Typeface.MONOSPACE
            textSize = 11f
            setPadding(24, 24, 24, 24)
            setTextIsSelectable(true)  // so a tester can copy the verdict into a bug report
        }
        setContentView(ScrollView(this).apply { addView(text) })

        text.text = runCatching { report() }
            .getOrElse { e -> "probe failed: ${e::class.simpleName}: ${e.message}" }
    }

    private fun report(): String = buildString {
        appendLine("FileFlow capability probe (C02)")
        appendLine("=".repeat(52))
        appendLine()

        val (claims, modes) = CapabilityProbe.readClaims(this@ProbeActivity)
        appendLine("CLAIMED BY THE PLATFORM")
        appendLine("  model            ${claims.model}")
        appendLine("  soc              ${claims.soc}")
        appendLine("  max refresh      ${claims.maxRefreshHz} Hz")
        appendLine("  panel            ${claims.panelWidth}x${claims.panelHeight}")
        appendLine("  hardware level   ${claims.hardwareLevel}")
        appendLine("  manual sensor    ${claims.claimsManualSensor}")
        appendLine("  realtime clock   ${claims.timestampSourceRealtime}")
        appendLine("  RS skew          ${claims.rollingShutterSkewNs} ns")
        appendLine("  camera modes     ${modes.size}")
        modes.take(8).forEach {
            val kind = if (it.highSpeed) "high-speed" else if (it.cpuReadable) "cpu" else "?"
            appendLine("    ${it.width}x${it.height} @ ${"%.1f".format(it.maxFps)} ($kind)")
        }
        if (modes.size > 8) appendLine("    … and ${modes.size - 8} more")
        appendLine()

        appendLine("NATIVE VERDICT (core/device.cpp — ADR-0014)")
        appendLine(dev.fileflow.probe.NativeProbe.decide(claims, modes).toString())
        appendLine()

        appendLine("-".repeat(52))
        appendLine("UNSUPPORTED is expected here. Nothing above was VERIFIED — the probe")
        appendLine("gathers claims only, and a claim is not evidence (RISK-011). Fd in")
        appendLine("particular is NOT the refresh rate and must be measured: EXP-006.")
        appendLine("The camera path needs EXP-007. Until those run, no tier is earned.")
    }
}

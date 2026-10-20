// SPDX-License-Identifier: GPL-3.0-or-later
// Порт на Compose индикатора загрузки из мода MargyT (Loading.java, GPL-3.0).
// Форма и математика анимации — оттуда; этот файл остаётся под GPL-3.0-or-later.
// Остальной проект — MPL-2.0. This file is a port of GPL-3.0 code from MargyT.

package com.compose.sctool

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.layout.size
import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.runtime.withFrameMillis
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Outline
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.Shape
import androidx.compose.ui.unit.Density
import androidx.compose.ui.unit.LayoutDirection
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.unit.dp
import kotlin.math.PI
import kotlin.math.cos
import kotlin.math.exp
import kotlin.math.ln
import kotlin.math.min
import kotlin.math.sin
import kotlin.math.sqrt

/**
 * Material 3 Expressive loading indicator: one shape that turns and morphs
 * through seven shapes (spring easing) instead of a spinning arc.
 * Idea and shape math: @MargyT mod (friend's code), ported to Compose.
 */
@Composable
fun MdLoadingIndicator(
    modifier: Modifier = Modifier.size(48.dp),
    color: Color = MaterialTheme.colorScheme.primary
) {
    var now by remember { mutableStateOf(0L) }
    LaunchedEffect(Unit) {
        while (true) {
            withFrameMillis { t -> now = t }
        }
    }
    Canvas(modifier) {
        drawMorph(now, color)
    }
}

private const val MD_STEP = 650L
private const val MD_SHAPES = 7
private const val MD_POINTS = 96
private const val MD_TURN = 50f

private fun DrawScope.drawMorph(now: Long, color: Color) {
    val along = (now % (MD_STEP * MD_SHAPES)) / MD_STEP.toFloat()
    val from = along.toInt()
    val to = (from + 1) % MD_SHAPES
    val eased = mdSettle(along - from)

    val steps = now / MD_STEP
    val angle = Math.toRadians((((steps % 360) + eased) * MD_TURN).toDouble())

    val cx = size.width / 2f
    val cy = size.height / 2f
    val sz = min(size.width, size.height) * 0.46f

    val path = Path()
    for (i in 0 until MD_POINTS) {
        val around = 2 * PI * i / MD_POINTS
        val one = mdRadius(from, around)
        val r = (one + (mdRadius(to, around) - one) * eased) * sz
        val x = cx + r * cos(around + angle).toFloat()
        val y = cy + r * sin(around + angle).toFloat()
        if (i == 0) path.moveTo(x, y) else path.lineTo(x, y)
    }
    path.close()
    drawPath(path, color)
}

private fun mdSettle(t: Float): Float {
    if (t <= 0f) return 0f
    if (t >= 1f) return 1f
    return 1f - (exp(-6.0 * t) * cos(8.0 * t)).toFloat()
}

private fun mdRadius(shape: Int, a: Double): Float {
    if (shape == 0) return 0.89f + 0.11f * cos(10 * a).toFloat()
    if (shape == 1) return 0.91f + 0.09f * cos(9 * a).toFloat()
    if (shape == 2) return mdSided(5, a)
    if (shape == 3) return mdOval(0.56, a)
    if (shape == 4) return 0.86f + 0.14f * cos(8 * a).toFloat()
    if (shape == 5) return 0.83f + 0.17f * cos(4 * a).toFloat()
    return mdOval(0.74, a)
}

private fun mdSided(sides: Int, a: Double): Float {
    val slice = 2 * PI / sides
    var within = a % slice
    if (within < 0) within += slice
    val flat = cos(PI / sides) / cos(within - slice / 2)
    val cap = 0.92
    val sharp = 36.0
    val soft = -ln(exp(-sharp * flat) + exp(-sharp * cap)) / sharp
    return (soft / cap).toFloat()
}

private fun mdOval(narrow: Double, a: Double): Float {
    val x = narrow * cos(a)
    val y = sin(a)
    return (narrow / sqrt(x * x + y * y)).toFloat()
}

/** Scalloped "cookie" container shape (M3 Expressive). sides: 9 soft, 4 square-ish. */
class MdCookieShape(private val sides: Int = 9, private val depth: Float = 0.07f) : Shape {
    override fun createOutline(size: androidx.compose.ui.geometry.Size, layoutDirection: LayoutDirection, density: Density): Outline {
        val path = Path()
        val cx = size.width / 2f
        val cy = size.height / 2f
        val r0 = min(size.width, size.height) / 2f
        for (i in 0 until 120) {
            val a = 2 * PI * i / 120
            val r = r0 * (1f - depth + depth * cos(sides * a).toFloat())
            val x = cx + r * cos(a).toFloat()
            val y = cy + r * sin(a).toFloat()
            if (i == 0) path.moveTo(x, y) else path.lineTo(x, y)
        }
        path.close()
        return Outline.Generic(path)
    }
}

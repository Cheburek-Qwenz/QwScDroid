package com.compose.sctool

import android.os.Build
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.*
import androidx.compose.runtime.Composable
import androidx.compose.ui.platform.LocalContext
import androidx.compose.runtime.remember
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.Font
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp

// Более округлые формы в духе M3 Expressive
private val ScShapes = Shapes(
    extraSmall = RoundedCornerShape(10.dp),
    small      = RoundedCornerShape(14.dp),
    medium     = RoundedCornerShape(20.dp),
    large      = RoundedCornerShape(28.dp),
    extraLarge = RoundedCornerShape(36.dp)
)

// Заголовки жирнее, как в Expressive
private val base = Typography()
private val ScTypography = Typography(
    headlineLarge  = base.headlineLarge.copy(fontWeight = FontWeight.Bold),
    headlineMedium = base.headlineMedium.copy(fontWeight = FontWeight.Bold),
    headlineSmall  = base.headlineSmall.copy(fontWeight = FontWeight.Bold),
    titleLarge     = base.titleLarge.copy(fontWeight = FontWeight.Bold),
    titleMedium    = base.titleMedium.copy(fontWeight = FontWeight.SemiBold),
    labelLarge     = base.labelLarge.copy(fontWeight = FontWeight.SemiBold)
)

/**
 * Свой шрифт без правок кода: положи файлы в app/src/main/assets/fonts/
 *   app_regular.ttf  (обязателен, чтобы шрифт включился)
 *   app_medium.ttf   (необязательно)
 *   app_bold.ttf     (необязательно)
 * Нет файлов — остаётся системный шрифт.
 */
private fun loadFamily(ctx: android.content.Context): FontFamily? {
    return try {
        val have = ctx.assets.list("fonts")?.toSet() ?: emptySet()
        if (!have.contains("app_regular.ttf")) return null
        val list = ArrayList<Font>()
        list.add(Font("fonts/app_regular.ttf", ctx.assets, FontWeight.Normal))
        if (have.contains("app_medium.ttf")) list.add(Font("fonts/app_medium.ttf", ctx.assets, FontWeight.Medium))
        if (have.contains("app_bold.ttf")) {
            list.add(Font("fonts/app_bold.ttf", ctx.assets, FontWeight.SemiBold))
            list.add(Font("fonts/app_bold.ttf", ctx.assets, FontWeight.Bold))
        }
        FontFamily(list)
    } catch (e: Exception) {
        null
    }
}

private fun TextStyle.f(family: FontFamily): TextStyle = this.copy(fontFamily = family)

private fun Typography.withFamily(fam: FontFamily): Typography = Typography(
    displayLarge = displayLarge.f(fam), displayMedium = displayMedium.f(fam),
    displaySmall = displaySmall.f(fam), headlineLarge = headlineLarge.f(fam),
    headlineMedium = headlineMedium.f(fam), headlineSmall = headlineSmall.f(fam),
    titleLarge = titleLarge.f(fam), titleMedium = titleMedium.f(fam),
    titleSmall = titleSmall.f(fam), bodyLarge = bodyLarge.f(fam),
    bodyMedium = bodyMedium.f(fam), bodySmall = bodySmall.f(fam),
    labelLarge = labelLarge.f(fam), labelMedium = labelMedium.f(fam),
    labelSmall = labelSmall.f(fam)
)

@Composable
fun SCToolTheme(
    darkTheme: Boolean = isSystemInDarkTheme(),
    content: @Composable () -> Unit
) {
    val colorScheme = when {
        Build.VERSION.SDK_INT >= Build.VERSION_CODES.S -> {
            val ctx = LocalContext.current
            if (darkTheme) dynamicDarkColorScheme(ctx) else dynamicLightColorScheme(ctx)
        }
        darkTheme -> darkColorScheme()
        else      -> lightColorScheme()
    }
    val appCtx = LocalContext.current
    val family = remember { loadFamily(appCtx) }
    val typo = if (family != null) ScTypography.withFamily(family) else ScTypography
    MaterialTheme(
        colorScheme = colorScheme,
        typography  = typo,
        shapes      = ScShapes,
        content     = content
    )
}

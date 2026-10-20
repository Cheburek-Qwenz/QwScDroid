package com.compose.sctool

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.rounded.*
import androidx.compose.material3.*
import androidx.compose.animation.core.Spring
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.spring
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.animation.core.Animatable
import androidx.compose.animation.core.FastOutSlowInEasing
import androidx.compose.animation.core.tween
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.hapticfeedback.HapticFeedbackType
import androidx.compose.ui.platform.LocalHapticFeedback
import kotlin.math.PI
import kotlin.math.abs
import kotlin.math.cos
import kotlin.math.sin
import android.content.Intent
import android.net.Uri
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import android.widget.ImageView
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.viewinterop.AndroidView
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties

/** Экран настроек приложения. */
@Composable
fun SettingsDialog(
    isRu: Boolean,
    legacyEditor: Boolean,
    onLegacyEditorChange: (Boolean) -> Unit,
    autoUpdate: Boolean,
    onAutoUpdateChange: (Boolean) -> Unit,
    onCheckNow: () -> Unit,
    onLanguage: () -> Unit,
    onDismiss: () -> Unit
) {
    val cs = MaterialTheme.colorScheme

    Dialog(
        onDismissRequest = onDismiss,
        properties = DialogProperties(usePlatformDefaultWidth = false)
    ) {
        Surface(Modifier.fillMaxSize(), color = cs.surface) {
            Column(Modifier.fillMaxSize().statusBarsPadding().navigationBarsPadding()) {

                Row(
                    Modifier.fillMaxWidth().padding(horizontal = 8.dp, vertical = 8.dp),
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    IconButton(onClick = onDismiss) { Icon(Icons.Rounded.Close, null) }
                    Text(
                        tr("settings.settings"),
                        style = MaterialTheme.typography.titleLarge,
                        fontWeight = FontWeight.Bold
                    )
                }

                Column(
                    Modifier
                        .weight(1f)
                        .verticalScroll(rememberScrollState())
                        .padding(horizontal = 16.dp, vertical = 8.dp),
                    verticalArrangement = Arrangement.spacedBy(8.dp)
                ) {

                    SettingsSection(tr("settings.editor"))

                    SettingsSwitchRow(
                        icon = Icons.Rounded.Edit,
                        title = tr("settings.legacy_editor"),
                        subtitle =
                            tr("settings.previous_layout_list_on_the_left_a"),
                        checked = legacyEditor,
                        onChange = onLegacyEditorChange
                    )

                    SettingsSection(tr("settings.updates"))

                    SettingsSwitchRow(
                        icon = Icons.Rounded.SystemUpdate,
                        title = tr("settings.check_on_launch"),
                        subtitle =
                            tr("settings.at_most_once_every_6_hours_a_dialo"),
                        checked = autoUpdate,
                        onChange = onAutoUpdateChange
                    )

                    SettingsClickRow(
                        icon = Icons.Rounded.Refresh,
                        title = tr("settings.check_now"),
                        subtitle = null,
                        onClick = onCheckNow
                    )

                    SettingsSection(tr("settings.app"))

                    SettingsClickRow(
                        icon = Icons.Rounded.Language,
                        title = tr("settings.language"),
                        subtitle = tr("settings.english"),
                        onClick = onLanguage
                    )

                    SettingsSection(tr("settings.about"))

                    AboutCard(isRu)
                }
            }
        }
    }
}

@Composable
private fun SettingsSection(text: String) {
    Text(
        text,
        modifier = Modifier.padding(start = 8.dp, top = 12.dp, bottom = 2.dp),
        style = MaterialTheme.typography.labelMedium,
        fontWeight = FontWeight.Bold,
        color = MaterialTheme.colorScheme.primary
    )
}

@Composable
private fun SettingsSwitchRow(
    icon: androidx.compose.ui.graphics.vector.ImageVector,
    title: String,
    subtitle: String,
    checked: Boolean,
    onChange: (Boolean) -> Unit
) {
    Surface(
        shape = RoundedCornerShape(22.dp),
        color = MaterialTheme.colorScheme.surfaceContainerLow
    ) {
        Row(
            Modifier
                .fillMaxWidth()
                .clickable { onChange(!checked) }
                .padding(horizontal = 16.dp, vertical = 14.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            Icon(icon, null, tint = MaterialTheme.colorScheme.primary)
            Spacer(Modifier.width(16.dp))
            Column(Modifier.weight(1f)) {
                Text(title, style = MaterialTheme.typography.titleSmall)
                Text(
                    subtitle,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
            }
            Spacer(Modifier.width(12.dp))
            Switch(checked = checked, onCheckedChange = onChange)
        }
    }
}

@Composable
private fun SettingsClickRow(
    icon: androidx.compose.ui.graphics.vector.ImageVector,
    title: String,
    subtitle: String?,
    onClick: () -> Unit
) {
    Surface(
        shape = RoundedCornerShape(22.dp),
        color = MaterialTheme.colorScheme.surfaceContainerLow
    ) {
        Row(
            Modifier
                .fillMaxWidth()
                .clickable { onClick() }
                .padding(horizontal = 16.dp, vertical = 14.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            Icon(icon, null, tint = MaterialTheme.colorScheme.primary)
            Spacer(Modifier.width(16.dp))
            Column(Modifier.weight(1f)) {
                Text(title, style = MaterialTheme.typography.titleSmall)
                if (subtitle != null) Text(
                    subtitle,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
            }
            Icon(
                Icons.Rounded.ChevronRight, null,
                tint = MaterialTheme.colorScheme.onSurfaceVariant
            )
        }
    }
}

@Composable
private fun AboutCard(isRu: Boolean) {
    val ctx = LocalContext.current
    val cs = MaterialTheme.colorScheme
    val version = Updater.currentVersion(ctx)
    val versionText = (tr("settings.version")) + version
    var eggKey by remember { mutableIntStateOf(0) }
    val haptic = LocalHapticFeedback.current
    val prog = remember { Animatable(0f) }

    LaunchedEffect(eggKey) {
        if (eggKey > 0) {
            prog.snapTo(0f)
            prog.animateTo(1f, tween(2200, easing = FastOutSlowInEasing))
        }
    }

    // Частицы салюта: угол, скорость, размер, цвет (индекс), форма.
    val particles = remember {
        val r = kotlin.random.Random(11)
        List(36) {
            floatArrayOf(
                (r.nextFloat() * 2f * PI.toFloat()),
                (0.45f + r.nextFloat() * 0.75f),
                (3f + r.nextFloat() * 5f),
                r.nextInt(4).toFloat(),
                r.nextInt(2).toFloat()
            )
        }
    }
    val palette = listOf(cs.primary, cs.tertiary, cs.secondary, cs.error)

    fun open(url: String) {
        try {
            ctx.startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(url)))
        } catch (e: Exception) {
        }
    }

    val p = prog.value
    val eggActive = p > 0f && p < 1f
    val tagline =
        if (eggActive) (tr("settings.thank_you_for_using_the_app"))
        else (tr("settings.tools_for_supercell_files"))

    // Иконка: оборот, отскок с затуханием, лёгкое сжатие при ударе.
    val spin = 720f * FastOutSlowInEasing.transform(p.coerceIn(0f, 1f))
    val bounce = abs(sin(p * 3.5f * PI.toFloat())) * (1f - p) * 150f
    val squash = 1f - 0.16f * (1f - p) * abs(cos(p * 3.5f * PI.toFloat())) * (if (eggActive) 1f else 0f)

    Surface(
        shape = RoundedCornerShape(28.dp),
        color = cs.primaryContainer,
        modifier = Modifier.fillMaxWidth()
    ) {
        Column(
            Modifier
                .fillMaxWidth()
                .drawBehind {
                    if (p > 0f && p < 1f) {
                        val origin = Offset(size.width / 2f, 24.dp.toPx() + 38.dp.toPx())
                        val t = (p * 1.25f).coerceIn(0f, 1f)
                        for (pt in particles) {
                            val ang = pt[0]
                            val dist = pt[1] * 260.dp.toPx() * FastOutSlowInEasing.transform(t)
                            val x = origin.x + cos(ang) * dist
                            val y = origin.y + sin(ang) * dist + 220.dp.toPx() * t * t
                            val alpha = (1f - t).coerceIn(0f, 1f)
                            val col = palette[pt[3].toInt()]
                            val sz = pt[2].dp.toPx()
                            if (pt[4] > 0.5f) {
                                drawRect(
                                    color = col,
                                    topLeft = Offset(x - sz / 2f, y - sz / 2f),
                                    size = Size(sz, sz * 0.6f),
                                    alpha = alpha
                                )
                            } else {
                                drawCircle(color = col, radius = sz / 2f, center = Offset(x, y), alpha = alpha)
                            }
                        }
                    }
                }
                .padding(horizontal = 20.dp, vertical = 24.dp),
            horizontalAlignment = Alignment.CenterHorizontally
        ) {
            Surface(
                shape = CircleShape,
                color = cs.surface,
                modifier = Modifier
                    .size(76.dp)
                    .graphicsLayer {
                        rotationZ = spin
                        translationY = -bounce.dp.toPx()
                        scaleX = 2f - squash
                        scaleY = squash
                    }
            ) {
                AndroidView(
                    factory = { context ->
                        ImageView(context).apply {
                            scaleType = ImageView.ScaleType.CENTER_CROP
                            setImageDrawable(context.packageManager.getApplicationIcon(context.packageName))
                        }
                    },
                    modifier = Modifier
                        .fillMaxSize()
                        .clip(CircleShape)
                )
            }
            Spacer(Modifier.height(14.dp))
            Text(
                "QwScDroid",
                style = MaterialTheme.typography.headlineSmall,
                fontWeight = FontWeight.Bold,
                color = cs.onPrimaryContainer
            )
            Text(
                tagline,
                style = MaterialTheme.typography.bodyMedium,
                textAlign = TextAlign.Center,
                color = cs.onPrimaryContainer
            )
            Spacer(Modifier.height(14.dp))
            Surface(shape = RoundedCornerShape(50), color = cs.surface) {
                Text(
                    versionText,
                    modifier = Modifier.padding(horizontal = 14.dp, vertical = 6.dp),
                    style = MaterialTheme.typography.labelLarge,
                    color = cs.onSurface
                )
            }
        }
    }

    Surface(
        shape = RoundedCornerShape(22.dp),
        color = cs.surfaceContainerLow,
        modifier = Modifier.fillMaxWidth()
    ) {
        Column {
            AboutAuthorRow(isRu = isRu, onEasterEgg = {
                haptic.performHapticFeedback(HapticFeedbackType.LongPress)
                eggKey++
            })
            AboutRow(
                icon = Icons.Rounded.Link,
                title = tr("settings.telegram_channel"),
                value = "@Cheburek_dev",
                onClick = { open("https://t.me/Cheburek_dev") }
            )
            AboutRow(
                icon = Icons.Rounded.BugReport,
                title = tr("settings.report_a_bug"),
                value = "@asdfghjkl12345678",
                onClick = { open("https://t.me/asdfghjkl12345678") }
            )
            AboutRow(
                icon = Icons.Rounded.Code,
                title = tr("settings.sc_downgrade_library_ty"),
                value = "Invoker4k",
                onClick = null
            )
            AboutRow(
                icon = Icons.Rounded.Movie,
                title = tr("settings.gif_sc_tool_idea_and_format"),
                value = "@Ghoul1337i",
                onClick = null
            )
        }
    }
}

@Composable
private fun AboutAuthorRow(
    isRu: Boolean,
    onEasterEgg: () -> Unit
) {
    val cs = MaterialTheme.colorScheme
    var tapCount by remember { mutableIntStateOf(0) }
    var firstTapAt by remember { mutableLongStateOf(0L) }

    fun handleTap() {
        val now = android.os.SystemClock.elapsedRealtime()

        if (firstTapAt == 0L || now - firstTapAt > 1_500L) {
            firstTapAt = now
            tapCount = 1
            return
        }

        tapCount++
        if (tapCount >= 7) {
            onEasterEgg()
            tapCount = 0
            firstTapAt = 0L
        }
    }

    Row(
        Modifier
            .fillMaxWidth()
            .clickable { handleTap() }
            .padding(horizontal = 16.dp, vertical = 14.dp),
        verticalAlignment = Alignment.CenterVertically
    ) {
        Icon(Icons.Rounded.Person, null, tint = cs.primary)
        Spacer(Modifier.width(16.dp))
        Column(Modifier.weight(1f)) {
            Text(
                tr("settings.author"),
                style = MaterialTheme.typography.titleSmall
            )
            Text(
                "Чебурек asdf",
                style = MaterialTheme.typography.bodySmall,
                color = cs.onSurfaceVariant
            )
        }
    }
}

@Composable
private fun AboutRow(
    icon: androidx.compose.ui.graphics.vector.ImageVector,
    title: String,
    value: String,
    onClick: (() -> Unit)?
) {
    val cs = MaterialTheme.colorScheme
    val rowMod =
        if (onClick != null) Modifier.fillMaxWidth().clickable { onClick() }
        else Modifier.fillMaxWidth()

    Row(
        rowMod.padding(horizontal = 16.dp, vertical = 14.dp),
        verticalAlignment = Alignment.CenterVertically
    ) {
        Icon(icon, null, tint = cs.primary)
        Spacer(Modifier.width(16.dp))
        Column(Modifier.weight(1f)) {
            Text(title, style = MaterialTheme.typography.titleSmall)
            Text(
                value,
                style = MaterialTheme.typography.bodySmall,
                color = cs.onSurfaceVariant
            )
        }
        if (onClick != null) {
            Icon(Icons.Rounded.ChevronRight, null, tint = cs.onSurfaceVariant)
        }
    }
}

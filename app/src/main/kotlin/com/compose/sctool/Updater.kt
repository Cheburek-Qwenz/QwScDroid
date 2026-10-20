package com.compose.sctool

import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.Build
import android.provider.Settings
import androidx.compose.animation.AnimatedContent
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.togetherWith
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.rounded.*
import androidx.compose.material3.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.draw.clip
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import kotlinx.coroutines.Job
import androidx.compose.runtime.*
import androidx.compose.ui.platform.LocalContext
import androidx.core.content.FileProvider
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONArray
import java.io.File
import java.net.HttpURLConnection
import java.net.URL
import kotlin.coroutines.coroutineContext

/** Настройки автообновления. Впиши свой репозиторий. */
object UpdateConfig {
    const val GITHUB_REPO = "Cheburek-Qwenz/QwScDroid"          // например "cheburek/qwscdroid-releases"
    const val INCLUDE_PRERELEASES = true
    const val AUTO_CHECK_INTERVAL_MS = 6L * 60 * 60 * 1000

    /** Базы для API: сначала оригинал, потом зеркала (если провайдер блокирует github). */
    val API_BASES = listOf(
        "https://api.github.com",
        "https://api.kkgithub.com",
        "https://gh-proxy.com/https://api.github.com"
    )

    /** Как превратить прямую ссылку на APK в зеркальную. */
    fun downloadCandidates(url: String): List<String> = listOf(
        url,
        url.replace("https://github.com", "https://kkgithub.com"),
        "https://gh-proxy.com/$url"
    ).distinct()
}

data class UpdateInfo(val version: String, val notes: String, val apkUrl: String, val size: Long, val prerelease: Boolean = false)

sealed class CheckResult {
    data class Available(val info: UpdateInfo) : CheckResult()
    object UpToDate : CheckResult()
    object NotConfigured : CheckResult()
    data class Error(val msg: String) : CheckResult()
}

object Updater {
    fun currentVersion(ctx: Context): String = try {
        ctx.packageManager.getPackageInfo(ctx.packageName, 0).versionName ?: "0"
    } catch (_: Exception) { "0" }

    fun parseVersion(v: String): List<Int> =
        Regex("\\d+").findAll(v.removePrefix("v")).map { it.value.toIntOrNull() ?: 0 }.toList()

    fun isNewer(remote: String, local: String): Boolean {
        val r = parseVersion(remote); val l = parseVersion(local)
        for (i in 0 until maxOf(r.size, l.size)) {
            val a = r.getOrElse(i) { 0 }; val b = l.getOrElse(i) { 0 }
            if (a != b) return a > b
        }
        return false
    }

    suspend fun check(ctx: Context): CheckResult = withContext(Dispatchers.IO) {
        if (UpdateConfig.GITHUB_REPO.startsWith("OWNER/")) return@withContext CheckResult.NotConfigured
        try {
            val path = "/repos/${UpdateConfig.GITHUB_REPO}/releases?per_page=10"
            val prefs = ctx.getSharedPreferences("upd", Context.MODE_PRIVATE)
            val first = prefs.getInt("base", 0).coerceIn(0, UpdateConfig.API_BASES.size - 1)
            val order = listOf(first) + UpdateConfig.API_BASES.indices.filter { it != first }
            var body: String? = null
            var lastErr = tr("update.no_connection")
            for (idx in order) {
                try {
                    val c = URL(UpdateConfig.API_BASES[idx] + path).openConnection() as HttpURLConnection
                    c.connectTimeout = 6000; c.readTimeout = 10000
                    c.setRequestProperty("Accept", "application/vnd.github+json")
                    if (c.responseCode == 200) {
                        val t = c.inputStream.bufferedReader().readText()
                        c.disconnect()
                        if (t.trimStart().startsWith("[")) { body = t; prefs.edit().putInt("base", idx).apply(); break }
                        lastErr = tr("update.bad_mirror")
                    } else {
                        lastErr = "HTTP ${c.responseCode}"
                        c.disconnect()
                    }
                } catch (e: Exception) {
                    lastErr = e.message ?: e.javaClass.simpleName
                }
            }
            if (body == null) return@withContext CheckResult.Error(lastErr)
            val arr = JSONArray(body)
            val local = currentVersion(ctx)
            var best: UpdateInfo? = null
            for (i in 0 until arr.length()) {
                val o = arr.getJSONObject(i)
                if (o.optBoolean("draft")) continue
                if (o.optBoolean("prerelease") && !UpdateConfig.INCLUDE_PRERELEASES) continue
                val tag = o.optString("tag_name")
                val assets = o.optJSONArray("assets") ?: continue
                var apk: org.json.JSONObject? = null
                for (j in 0 until assets.length()) {
                    val a = assets.getJSONObject(j)
                    if (a.optString("name").endsWith(".apk", true)) { apk = a; break }
                }
                if (apk == null) continue
                if (best == null || isNewer(tag, best.version))
                    best = UpdateInfo(tag, o.optString("body"), apk.optString("browser_download_url"), apk.optLong("size"), o.optBoolean("prerelease"))
            }
            val result: CheckResult =
                if (best != null && isNewer(best.version, local)) CheckResult.Available(best)
                else CheckResult.UpToDate
            result
        } catch (e: Exception) {
            CheckResult.Error(e.message ?: e.javaClass.simpleName)
        }
    }

    suspend fun download(ctx: Context, info: UpdateInfo, onProgress: (Float) -> Unit): File =
        withContext(Dispatchers.IO) {
            val dir = File(ctx.cacheDir, "update").apply { deleteRecursively(); mkdirs() }
            val out = File(dir, "update.apk")
            var ok = false
            var lastErr = tr("update.no_connection")
            for (u in UpdateConfig.downloadCandidates(info.apkUrl)) {
                try {
                    val c = URL(u).openConnection() as HttpURLConnection
                    c.connectTimeout = 8000; c.readTimeout = 20000
                    c.instanceFollowRedirects = true
                    if (c.responseCode != 200) { lastErr = "HTTP ${c.responseCode}"; c.disconnect(); continue }
                    val total = if (c.contentLengthLong > 0) c.contentLengthLong else info.size
                    c.inputStream.use { inp ->
                        out.outputStream().use { os ->
                            val buf = ByteArray(64 * 1024); var done = 0L
                            while (true) {
                                coroutineContext.ensureActive()
                                val n = inp.read(buf); if (n < 0) break
                                os.write(buf, 0, n); done += n
                                if (total > 0) onProgress(done.toFloat() / total)
                            }
                        }
                    }
                    c.disconnect()
                    if (info.size > 0 && out.length() != info.size) { lastErr = tr("update.file_corrupt"); continue }
                    ok = true; break
                } catch (e: kotlinx.coroutines.CancellationException) {
                    throw e
                } catch (e: Exception) {
                    lastErr = e.message ?: e.javaClass.simpleName
                }
            }
            if (!ok) throw IllegalStateException(lastErr)
            val pi = ctx.packageManager.getPackageArchiveInfo(out.absolutePath, 0)
            if (pi == null || pi.packageName != ctx.packageName)
                throw IllegalStateException(tr("update.apk_wrong_package"))
            out
        }

    /** false — нужно сначала разрешить установку из этого источника. */
    fun install(ctx: Context, apk: File): Boolean {
        if (Build.VERSION.SDK_INT >= 26 && !ctx.packageManager.canRequestPackageInstalls()) {
            ctx.startActivity(Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES,
                Uri.parse("package:${ctx.packageName}")).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK))
            return false
        }
        val uri = FileProvider.getUriForFile(ctx, "${ctx.packageName}.fileprovider", apk)
        ctx.startActivity(Intent(Intent.ACTION_VIEW).setDataAndType(uri, "application/vnd.android.package-archive")
            .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_ACTIVITY_NEW_TASK))
        return true
    }
}

private enum class Phase { Idle, Checking, Available, Downloading, Ready, UpToDate, Error }

private fun fmtMb(b: Long) = if (b <= 0) "—" else String.format(java.util.Locale.US, "%.1f MB", b / 1048576.0)

/** Один раз в Scaffold. manualToken++ — открыть экран и проверить. Сам всплывает, если нашёл новую версию. */
@Composable
fun UpdateChecker(isRu: Boolean, manualToken: Int) {
    val ctx = LocalContext.current
    val scope = rememberCoroutineScope()
    var open by remember { mutableStateOf(false) }
    var phase by remember { mutableStateOf(Phase.Idle) }
    var info by remember { mutableStateOf<UpdateInfo?>(null) }
    var apk by remember { mutableStateOf<File?>(null) }
    var error by remember { mutableStateOf("") }
    var progress by remember { mutableStateOf(0f) }
    var needPerm by remember { mutableStateOf(false) }
    var job by remember { mutableStateOf<Job?>(null) }

    fun err(t: String) { error = t; phase = Phase.Error }

    suspend fun check(silent: Boolean) {
        if (!silent) phase = Phase.Checking
        when (val r = Updater.check(ctx)) {
            is CheckResult.Available -> {
                val skipped = ctx.getSharedPreferences("upd", Context.MODE_PRIVATE)
                    .getString("skip", "") == r.info.version
                if (!(silent && skipped)) {
                    info = r.info; phase = Phase.Available; open = true
                }
            }
            is CheckResult.UpToDate -> if (!silent) phase = Phase.UpToDate
            is CheckResult.NotConfigured -> if (!silent) err(tr("update.updates_are_not_configured"))
            is CheckResult.Error -> if (!silent) err(r.msg)
        }
    }

    LaunchedEffect(Unit) {
        val p = ctx.getSharedPreferences("upd", Context.MODE_PRIVATE)
        val now = System.currentTimeMillis()
        if (p.getBoolean("auto", true) && now - p.getLong("last", 0) > UpdateConfig.AUTO_CHECK_INTERVAL_MS) {
            p.edit().putLong("last", now).apply()
            check(true)
        }
    }
    LaunchedEffect(manualToken) {
        if (manualToken > 0) { open = true; needPerm = false; check(false) }
    }

    fun startDownload() {
        val u = info ?: return
        progress = 0f; phase = Phase.Downloading
        job = scope.launch {
            try {
                apk = Updater.download(ctx, u) { progress = it }
                phase = Phase.Ready
            } catch (e: kotlinx.coroutines.CancellationException) {
                throw e
            } catch (e: Exception) {
                err(e.message ?: e.javaClass.simpleName)
            }
        }
    }

    if (!open) return
    Dialog(
        onDismissRequest = { if (phase != Phase.Downloading) open = false },
        properties = DialogProperties(usePlatformDefaultWidth = false)
    ) {
        Surface(Modifier.fillMaxSize(), color = MaterialTheme.colorScheme.surface) {
            Column(Modifier.fillMaxSize().statusBarsPadding().navigationBarsPadding()) {
                Row(
                    Modifier.fillMaxWidth().padding(horizontal = 8.dp, vertical = 8.dp),
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    IconButton(onClick = { if (phase != Phase.Downloading) open = false }) {
                        Icon(Icons.Rounded.Close, null)
                    }
                    Text(
                        tr("update.updates"),
                        style = MaterialTheme.typography.titleLarge,
                        fontWeight = FontWeight.Bold,
                        modifier = Modifier.weight(1f)
                    )
                    IconButton(
                        enabled = phase != Phase.Checking && phase != Phase.Downloading,
                        onClick = { needPerm = false; scope.launch { check(false) } }
                    ) { Icon(Icons.Rounded.Refresh, null) }
                }

                Column(
                    Modifier.weight(1f).verticalScroll(rememberScrollState()).padding(horizontal = 16.dp, vertical = 8.dp),
                    verticalArrangement = Arrangement.spacedBy(16.dp)
                ) {
                    // Установленная версия
                    Surface(shape = RoundedCornerShape(28.dp), color = MaterialTheme.colorScheme.surfaceContainerLow) {
                        Row(
                            Modifier.fillMaxWidth().padding(20.dp),
                            verticalAlignment = Alignment.CenterVertically
                        ) {
                            Column(Modifier.weight(1f)) {
                                Text(
                                    tr("update.installed"),
                                    style = MaterialTheme.typography.labelSmall,
                                    color = MaterialTheme.colorScheme.onSurfaceVariant
                                )
                                Text(
                                    "v${Updater.currentVersion(ctx)}",
                                    style = MaterialTheme.typography.headlineMedium,
                                    fontWeight = FontWeight.ExtraBold,
                                    fontFamily = FontFamily.Monospace
                                )
                            }
                            Surface(shape = RoundedCornerShape(50), color = MaterialTheme.colorScheme.secondaryContainer) {
                                Text(
                                    "QwScDroid",
                                    Modifier.padding(horizontal = 12.dp, vertical = 6.dp),
                                    style = MaterialTheme.typography.labelMedium,
                                    color = MaterialTheme.colorScheme.onSecondaryContainer
                                )
                            }
                        }
                    }

                    AnimatedContent(
                        targetState = phase,
                        transitionSpec = { fadeIn() togetherWith fadeOut() },
                        label = "upd"
                    ) { ph ->
                        when (ph) {
                            Phase.Idle -> StateCard(
                                Icons.Rounded.SystemUpdate,
                                tr("update.check_for_updates"), null
                            ) {
                                Button(onClick = { scope.launch { check(false) } }, Modifier.fillMaxWidth().height(56.dp)) {
                                    Icon(Icons.Rounded.Search, null, Modifier.size(20.dp))
                                    Spacer(Modifier.width(8.dp))
                                    Text(tr("update.check_for_updates_2"))
                                }
                            }

                            Phase.Checking -> StateCard(
                                Icons.Rounded.CloudSync,
                                tr("update.looking_for_updates"), null
                            ) {
                                MdLoadingIndicator(Modifier.size(56.dp))
                            }

                            Phase.Available -> {
                                val u = info
                                Surface(shape = RoundedCornerShape(28.dp), color = MaterialTheme.colorScheme.surfaceContainerHigh) {
                                    Column(Modifier.padding(20.dp), verticalArrangement = Arrangement.spacedBy(14.dp)) {
                                        Row(verticalAlignment = Alignment.Top) {
                                            Column(Modifier.weight(1f)) {
                                                Text(
                                                    tr("update.new_version"),
                                                    style = MaterialTheme.typography.labelSmall,
                                                    color = MaterialTheme.colorScheme.primary,
                                                    fontWeight = FontWeight.Bold
                                                )
                                                Text(
                                                    u?.version ?: "",
                                                    style = MaterialTheme.typography.headlineMedium,
                                                    fontWeight = FontWeight.Black,
                                                    fontFamily = FontFamily.Monospace
                                                )
                                                if (u?.prerelease == true) Text(
                                                    "Pre-release",
                                                    style = MaterialTheme.typography.labelSmall,
                                                    color = MaterialTheme.colorScheme.tertiary
                                                )
                                            }
                                            Surface(shape = RoundedCornerShape(50), color = MaterialTheme.colorScheme.secondaryContainer) {
                                                Text(
                                                    fmtMb(u?.size ?: 0),
                                                    Modifier.padding(horizontal = 12.dp, vertical = 6.dp),
                                                    style = MaterialTheme.typography.labelMedium,
                                                    fontFamily = FontFamily.Monospace,
                                                    color = MaterialTheme.colorScheme.onSecondaryContainer
                                                )
                                            }
                                        }
                                        if (!u?.notes.isNullOrBlank()) Surface(
                                            shape = RoundedCornerShape(20.dp),
                                            color = MaterialTheme.colorScheme.surfaceContainerLow
                                        ) {
                                            Column(Modifier.fillMaxWidth().padding(14.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                                                Row(verticalAlignment = Alignment.CenterVertically) {
                                                    Icon(Icons.Rounded.Notes, null, Modifier.size(16.dp), tint = MaterialTheme.colorScheme.onSurfaceVariant)
                                                    Spacer(Modifier.width(6.dp))
                                                    Text(
                                                        tr("update.what_s_new"),
                                                        style = MaterialTheme.typography.labelMedium,
                                                        color = MaterialTheme.colorScheme.onSurfaceVariant
                                                    )
                                                }
                                                MarkdownText(u!!.notes.trim().take(6000))
                                            }
                                        }
                                        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.End) {
                                            TextButton(onClick = {
                                                val v = u?.version ?: ""
                                                ctx.getSharedPreferences("upd", Context.MODE_PRIVATE)
                                                    .edit().putString("skip", v).apply()
                                                open = false
                                            }) { Text(tr("update.skip")) }
                                            TextButton(onClick = { open = false }) { Text(tr("update.later")) }
                                            Spacer(Modifier.width(8.dp))
                                            Button(onClick = { startDownload() }) {
                                                Icon(Icons.Rounded.Download, null, Modifier.size(18.dp))
                                                Spacer(Modifier.width(6.dp))
                                                Text(tr("update.update"))
                                            }
                                        }
                                    }
                                }
                            }

                            Phase.Downloading -> {
                                val total = info?.size ?: 0L
                                StateCard(
                                    Icons.Rounded.Downloading,
                                    tr("update.downloading"),
                                    "${(progress * 100).toInt()}%" + if (total > 0) "  ·  ${fmtMb((progress * total).toLong())} / ${fmtMb(total)}" else ""
                                ) {
                                    LinearProgressIndicator(progress = { progress }, modifier = Modifier.fillMaxWidth().clip(RoundedCornerShape(50)))
                                    TextButton(onClick = { job?.cancel(); phase = Phase.Available }) {
                                        Text(tr("update.cancel"))
                                    }
                                }
                            }

                            Phase.Ready -> StateCard(
                                Icons.Rounded.Verified,
                                tr("update.ready_to_install"),
                                if (needPerm) (tr("update.allow_installs_from_this_app_in_se")) else null
                            ) {
                                Button(
                                    onClick = { apk?.let { needPerm = !Updater.install(ctx, it) } },
                                    Modifier.fillMaxWidth()
                                ) {
                                    Icon(Icons.Rounded.InstallMobile, null, Modifier.size(20.dp))
                                    Spacer(Modifier.width(8.dp))
                                    Text(tr("update.install"))
                                }
                            }

                            Phase.UpToDate -> StateCard(
                                Icons.Rounded.CheckCircle,
                                tr("update.you_re_up_to_date"),
                                tr("update.nothing_to_update")
                            ) {
                                FilledTonalButton(onClick = { scope.launch { check(false) } }) {
                                    Text(tr("update.check_again"))
                                }
                            }

                            Phase.Error -> StateCard(
                                Icons.Rounded.ErrorOutline,
                                tr("update.update_failed"),
                                error, isError = true
                            ) {
                                Button(onClick = { scope.launch { check(false) } }) {
                                    Text(tr("update.retry"))
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

@Composable
private fun StateCard(
    icon: ImageVector,
    title: String,
    subtitle: String?,
    isError: Boolean = false,
    content: @Composable ColumnScope.() -> Unit
) {
    val cs = MaterialTheme.colorScheme
    Surface(
        shape = RoundedCornerShape(28.dp),
        color = if (isError) cs.errorContainer.copy(alpha = 0.4f) else cs.surfaceContainerHigh
    ) {
        Column(
            Modifier.fillMaxWidth().padding(24.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.spacedBy(14.dp)
        ) {
            Surface(
                shape = MdCookieShape(9, 0.07f),
                color = if (isError) cs.errorContainer else cs.primaryContainer,
                modifier = Modifier.size(84.dp)
            ) {
                Box(contentAlignment = Alignment.Center) {
                    Icon(icon, null, Modifier.size(38.dp),
                        tint = if (isError) cs.onErrorContainer else cs.onPrimaryContainer)
                }
            }
            Text(title, style = MaterialTheme.typography.titleMedium, fontWeight = FontWeight.Bold, textAlign = TextAlign.Center)
            if (!subtitle.isNullOrBlank()) Text(
                subtitle,
                style = MaterialTheme.typography.bodySmall,
                color = if (isError) cs.error else cs.onSurfaceVariant,
                textAlign = TextAlign.Center
            )
            content()
        }
    }
}

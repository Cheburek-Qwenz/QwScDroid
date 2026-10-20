package com.compose.sctool.editor

import com.compose.sctool.tr

import com.compose.sctool.MdLoadingIndicator

import android.graphics.Bitmap
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.animation.*
import androidx.compose.foundation.*
import androidx.compose.foundation.gestures.*
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.*
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.rounded.*
import androidx.compose.material.icons.automirrored.rounded.Undo
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.*
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.*
import androidx.compose.ui.graphics.asAndroidBitmap
import androidx.compose.ui.graphics.drawscope.*
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.input.pointer.*
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.foundation.Image
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.unit.*
import com.compose.sctool.FileHelper
import com.compose.sctool.ScProcessor
import kotlinx.coroutines.*
import java.io.File
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.text.style.TextOverflow

// ── Модели ────────────────────────────────────────────────────────────────────

/*
 * Один полигон спрайта — реальный контур вершин
 * из ShapeDrawBitmapCommand (или Sc2Command для
 * SC2), а не прямоугольный bounding box.
 *
 * У одного спрайта может быть несколько полигонов
 * (несколько draw-команд, кусков текстуры).
 */
data class ScPolygon(
    val textureIndex: Int,
    val points: List<Offset>
)

data class ScSprite(
    val id: Int,
    val name: String,

    // UV в пикселях атласа
    val u0: Float,
    val v0: Float,
    val u1: Float,
    val v1: Float,

    /*
     * Реальная геометрия — может быть пустой, если
     * SC-файл почему-то не дал вершин (тогда rect
     * (u0,v0)-(u1,v1) остаётся единственным источником
     * формы, как раньше).
     */
    val polygons: List<ScPolygon> = emptyList()
) {
    val w: Float
        get() = u1 - u0

    val h: Float
        get() = v1 - v0

    val displayName: String
        get() = name.ifBlank { "sprite_$id" }
}

/*
 * Метаданные одной текстуры SC2-файла — только
 * индекс и размер, без декодированных пикселей.
 *
 * Используется в multi-texture selector: список
 * текстур приходит сразу при открытии файла
 * (дёшево, без декодирования), а конкретная
 * текстура декодируется только когда пользователь
 * её выбрал.
 */
data class ScTextureInfo(
    val index: Int,
    val width: Int,
    val height: Int
)

data class ScAtlas(
    val bitmap: ImageBitmap,
    val width: Int,
    val height: Int,
    val sprites: List<ScSprite>,

    /*
     * Список всех текстур файла (для SC1/v0 —
     * всегда список из одной записи, т.к. там
     * концепции multi-texture нет).
     */
    val textures: List<ScTextureInfo> =
        emptyList(),

    val currentTextureIndex: Int = 0
)

// ── Главный экран редактора ───────────────────────────────────────────────────

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun ScEditorScreen(
    isRu: Boolean,
    legacy: Boolean = false,
    onMenu: (() -> Unit)? = null
) {

    val ctx = LocalContext.current
    val scope = rememberCoroutineScope()

    var atlas by remember {
        mutableStateOf<ScAtlas?>(null)
    }

    var selectedSprite by remember {
        mutableStateOf<ScSprite?>(null)
    }

    var status by remember {
        mutableStateOf("")
    }

    var loading by remember {
        mutableStateOf(false)
    }

    var scFile by remember {
        mutableStateOf<File?>(null)
    }

    /*
     * История версий файла для Undo.
     *
     * Каждый успешный Replace добавляет сюда
     * предыдущую версию перед переключением
     * на результат. "Назад" снимает верхнюю
     * запись и возвращается к ней.
     */
    val fileHistory =
        remember {
            mutableStateListOf<File>()
        }

    /*
     * Спрайт, который пользователь хочет экспортировать.
     *
     * Он сохраняется отдельно, потому что CreateDocument
     * сначала показывает системный диалог сохранения.
     */
    var exportSprite by remember {
        mutableStateOf<ScSprite?>(null)
    }

    /*
     * Поиск по списку спрайтов.
     *
     * searchOpen переключает заголовок списка
     * между "Sprites (N) 🔍" и полем ввода.
     */
    var searchOpen by remember {
        mutableStateOf(false)
    }

    var searchQuery by remember {
        mutableStateOf("")
    }

    // ── Открытие SC ──────────────────────────────────────────────────────────

    val scLauncher =
        rememberLauncherForActivityResult(
            ActivityResultContracts.GetContent()
        ) { uri ->

            if (uri == null)
                return@rememberLauncherForActivityResult

            loading = true

            status =
                tr("editor.loading_sc")

            scope.launch(Dispatchers.IO) {

                val file =
                    FileHelper.copyToCache(
                        ctx,
                        uri,
                        FileHelper.getName(ctx, uri)
                    )

                scFile = file

                val result =
                    loadScAtlas(
                        ctx,
                        file
                    )

                withContext(Dispatchers.Main) {

                    loading = false

                    if (result != null) {

                        atlas = result
                        selectedSprite = null
                        searchOpen = false
                        searchQuery = ""
                        fileHistory.clear()

                        status =
                            tr("editor.loaded_0_sprites_1_x_2", result.sprites.size, result.width, result.height)

                    } else {

                        status =
                            tr("editor.failed_to_load_sc")
                    }
                }
            }
        }

    // ── Выбор PNG для замены ─────────────────────────────────────────────────

    val pngLauncher =
        rememberLauncherForActivityResult(
            ActivityResultContracts.GetContent()
        ) { uri ->

            val sp =
                selectedSprite
                    ?: return@rememberLauncherForActivityResult

            val sf =
                scFile
                    ?: return@rememberLauncherForActivityResult

            if (uri == null)
                return@rememberLauncherForActivityResult

            loading = true

            status =
                tr("editor.replacing_0", sp.displayName)

            scope.launch(Dispatchers.IO) {

                val png =
                    copyImageAsPng(
                        ctx,
                        uri,
                        FileHelper.getName(ctx, uri)
                    )

                if (png == null) {
                    withContext(Dispatchers.Main) {
                        loading = false
                        status =
                            tr("editor.cannot_read_image")
                    }
                    return@launch
                }

                val outDir =
                    FileHelper.getOutDir(ctx)

                /*
                 * ВАЖНО:
                 *
                 * sp.id = Shape ID.
                 *
                 * Диспетчеризация по версии файла (SC1/SC2)
                 * происходит уже внутри nativePng2Sc.
                 */
                val res =
                    ScProcessor.nativePng2Sc(
                        png.absolutePath,
                        sf.absolutePath,
                        sp.id,
                        "0",
                        outDir.absolutePath
                    )

                val success =
                    res.startsWith("OK")

                /*
                 * После успешной замены находим только что
                 * записанный файл (самый свежий .sc в outDir)
                 * и переключаем редактор на него, чтобы
                 * следующий Replace шёл поверх результата,
                 * а не поверх исходника.
                 *
                 * Старая версия уходит в историю для Undo.
                 */
                val newFile =
                    if (success)
                        outDir
                            .listFiles { f ->
                                f.extension == "sc"
                            }
                            ?.maxByOrNull {
                                it.lastModified()
                            }
                    else
                        null

                var reloadedAtlas: ScAtlas? = null

                if (success && newFile != null) {

                    reloadedAtlas =
                        loadScAtlas(
                            ctx,
                            newFile
                        )
                }

                withContext(Dispatchers.Main) {

                    loading = false
                    status = res

                    if (success &&
                        newFile != null &&
                        reloadedAtlas != null) {

                        fileHistory.add(sf)

                        scFile = newFile
                        atlas = reloadedAtlas

                        selectedSprite =
                            reloadedAtlas.sprites
                                .firstOrNull {
                                    it.id == sp.id
                                }
                    }
                }
            }
        }

    // ── Создание файла для экспорта ──────────────────────────────────────────

    val exportLauncher =
        rememberLauncherForActivityResult(
            ActivityResultContracts.CreateDocument("image/png")
        ) { uri ->

            val sp =
                exportSprite
                    ?: return@rememberLauncherForActivityResult

            val a =
                atlas
                    ?: return@rememberLauncherForActivityResult

            if (uri == null) {
                exportSprite = null
                return@rememberLauncherForActivityResult
            }

            loading = true

            status =
                tr("editor.exporting_0", sp.displayName)

            scope.launch(Dispatchers.IO) {

                try {

                    /*
                     * Кроп через общую функцию —
                     * та же логика используется
                     * и для превью в SpriteInfoPanel.
                     */
                    val (cropped, width, height) =
                        cropSpriteBitmapWithSize(
                            a,
                            sp
                        )
                            ?: throw Exception(
                                "Crop failed"
                            )

                    ctx.contentResolver
                        .openOutputStream(uri)
                        ?.use { stream ->

                            if (!cropped.compress(
                                    Bitmap.CompressFormat.PNG,
                                    100,
                                    stream
                                )
                            ) {
                                throw Exception(
                                    "PNG compression failed"
                                )
                            }
                        }
                        ?: throw Exception(
                            "Cannot open output file"
                        )

                    cropped.recycle()

                    withContext(Dispatchers.Main) {

                        loading = false

                        status =
                            tr("editor.saved_0_png_1_x_2", sp.displayName, width, height)

                        exportSprite = null
                    }

                } catch (e: Exception) {

                    withContext(Dispatchers.Main) {

                        loading = false

                        status =
                            tr("editor.export_error_0", e.message)

                        exportSprite = null
                    }
                }
            }
        }

    // ── Batch Export: все спрайты (или отфильтрованные) в zip ────────────────

    val exportAllLauncher =
        rememberLauncherForActivityResult(
            ActivityResultContracts.CreateDocument("application/zip")
        ) { uri ->

            val a =
                atlas
                    ?: return@rememberLauncherForActivityResult

            if (uri == null)
                return@rememberLauncherForActivityResult

            /*
             * Экспортируем ровно тот список, что видит
             * пользователь в данный момент — если открыт
             * поиск с фильтром, экспортируются только
             * совпавшие спрайты, а не все 227.
             */
            val toExport =
                if (searchQuery.isBlank())
                    a.sprites
                else
                    a.sprites.filter {
                        it.displayName.contains(
                            searchQuery,
                            ignoreCase = true
                        )
                    }

            loading = true

            status =
                tr("editor.exporting_0_sprites", toExport.size)

            scope.launch(Dispatchers.IO) {

                try {

                    var done = 0
                    var failed = 0

                    /*
                     * Имена файлов внутри zip могут
                     * повторяться (несколько спрайтов
                     * без export name дают "sprite_N",
                     * а совпадающие displayName у
                     * разных ID — на практике редкость,
                     * но подстраховываемся суффиксом ID
                     * при коллизии).
                     */
                    val usedNames =
                        mutableSetOf<String>()

                    ctx.contentResolver
                        .openOutputStream(uri)
                        ?.use { rawOut ->

                            java.util.zip.ZipOutputStream(
                                rawOut
                            ).use { zip ->

                                for (sp in toExport) {

                                    val cropResult =
                                        cropSpriteBitmapWithSize(
                                            a,
                                            sp
                                        )

                                    if (cropResult == null) {
                                        failed++
                                        continue
                                    }

                                    val cropped =
                                        cropResult.first

                                    var baseName =
                                        sanitizeFileName(
                                            sp.displayName
                                        )

                                    if (!usedNames.add(baseName)) {
                                        baseName =
                                            "${baseName}_${sp.id}"
                                        usedNames.add(baseName)
                                    }

                                    zip.putNextEntry(
                                        java.util.zip.ZipEntry(
                                            "$baseName.png"
                                        )
                                    )

                                    cropped.compress(
                                        Bitmap.CompressFormat.PNG,
                                        100,
                                        zip
                                    )

                                    zip.closeEntry()

                                    cropped.recycle()

                                    done++
                                }
                            }
                        }
                        ?: throw Exception(
                            "Cannot open output file"
                        )

                    withContext(Dispatchers.Main) {

                        loading = false

                        status = tr("editor.exported", done) +
                                (if (failed > 0) tr("editor.exported_failed", failed) else "")
                    }

                } catch (e: Exception) {

                    withContext(Dispatchers.Main) {

                        loading = false

                        status =
                            tr("editor.batch_export_error_0", e.message)
                    }
                }
            }
        }

    // ── Пакетная замена: выбрать PNG → проверить пары → применить ───────────

    val batchEntries = remember { mutableStateListOf<BatchEntry>() }
    var showBatch by remember { mutableStateOf(false) }

    val batchLauncher =
        rememberLauncherForActivityResult(
            ActivityResultContracts.GetMultipleContents()
        ) { uris ->

            val a = atlas
                ?: return@rememberLauncherForActivityResult
            if (uris.isNullOrEmpty())
                return@rememberLauncherForActivityResult

            // Автоподбор по имени файла; потом можно поправить вручную.
            val byName = HashMap<String, ScSprite>()
            for (sp in a.sprites)
                byName.putIfAbsent(sp.displayName.lowercase(), sp)

            batchEntries.clear()
            for (uri in uris) {
                val name = FileHelper.getName(ctx, uri)
                batchEntries.add(
                    BatchEntry(
                        uri, name,
                        byName[name.substringBeforeLast('.').lowercase()]
                    )
                )
            }
            showBatch = true
        }

    fun runBatch() {

        val sf = scFile ?: return

        // Один спрайт — один PNG (при дубликатах берётся первый).
        val entries =
            batchEntries
                .filter { it.sprite != null }
                .distinctBy { it.sprite!!.id }

        if (entries.isEmpty()) return

        showBatch = false
        loading = true
        status =
            tr("editor.batch_replace_0", entries.size)

        scope.launch(Dispatchers.IO) {

            try {
                val lines = StringBuilder()

                val badFiles = ArrayList<String>()
                var okCount = 0

                for ((i, e) in entries.withIndex()) {
                    val png =
                        copyImageAsPng(ctx, e.uri, "b${i}_${e.name}")
                    if (png == null) {
                        badFiles.add(e.name)
                        continue
                    }
                    lines.append(e.sprite!!.id).append('\t')
                        .append(png.absolutePath).append('\n')
                    okCount++
                }

                if (okCount == 0) {
                    withContext(Dispatchers.Main) {
                        loading = false
                        status =
                            tr("editor.cannot_read_images")
                    }
                    return@launch
                }

                val outDir = FileHelper.getOutDir(ctx)
                val listFile = File(ctx.cacheDir, "batch_replace.txt")
                listFile.writeText(lines.toString())

                val res =
                    ScProcessor.nativePng2ScBatch(
                        listFile.absolutePath,
                        sf.absolutePath,
                        outDir.absolutePath
                    )

                val success = res.startsWith("OK")

                val outName =
                    Regex("Output: (.+)")
                        .find(res)?.groupValues?.get(1)?.trim()

                val newFile =
                    if (success && outName != null)
                        File(outDir, outName)
                    else null

                val reloaded =
                    if (newFile != null && newFile.exists())
                        loadScAtlas(ctx, newFile)
                    else null

                withContext(Dispatchers.Main) {

                    loading = false
                    status =
                        if (badFiles.isEmpty()) res
                        else res + "\n" +
                            (tr("editor.unreadable")) +
                            badFiles.take(3).joinToString(", ")

                    if (success && newFile != null && reloaded != null) {
                        fileHistory.add(sf)
                        scFile = newFile
                        atlas = reloaded
                        selectedSprite = null
                    }
                }

            } catch (e: Throwable) {
                withContext(Dispatchers.Main) {
                    loading = false
                    status =
                        tr("editor.batch_replace_error_0", e.message)
                }
            }
        }
    }

    // ── Экспортнеймы: правка списка → новый файл → перезагрузка ──────────────

    var showExports by remember { mutableStateOf(false) }

    fun runExportEdit(opsText: String, done: (String?) -> Unit) {

        val sf = scFile
        if (sf == null) { done(null); return }

        scope.launch(Dispatchers.IO) {

            try {
                val outDir = FileHelper.getOutDir(ctx)
                val opsFile = File(ctx.cacheDir, "export_ops.txt")
                opsFile.writeText(opsText)

                val res =
                    ScProcessor.nativeEditExports(
                        opsFile.absolutePath,
                        sf.absolutePath,
                        outDir.absolutePath
                    )

                val success = res.startsWith("OK")

                val outName =
                    Regex("Output: (.+)")
                        .find(res)?.groupValues?.get(1)?.trim()

                val newFile =
                    if (success && outName != null)
                        File(outDir, outName)
                    else null

                val reloaded =
                    if (newFile != null && newFile.exists())
                        loadScAtlas(ctx, newFile)
                    else null

                withContext(Dispatchers.Main) {

                    if (success && newFile != null && reloaded != null) {
                        fileHistory.add(sf)
                        scFile = newFile
                        atlas = reloaded
                        selectedSprite = null
                        status = res
                        showExports = false
                        done(null)
                    } else {
                        done(res.removePrefix("Error: "))
                    }
                }

            } catch (e: Throwable) {
                withContext(Dispatchers.Main) {
                    done(e.message ?: "Error")
                }
            }
        }
    }

    // ── Общие действия (используются новым интерфейсом) ──────────────────────

    fun undoLast() {

        val prev = fileHistory.removeLastOrNull() ?: return

        loading = true

        scope.launch(Dispatchers.IO) {

            val reloaded = loadScAtlas(ctx, prev)

            withContext(Dispatchers.Main) {

                loading = false

                if (reloaded != null) {
                    scFile = prev
                    atlas = reloaded
                    selectedSprite = null
                    status =
                        tr("editor.undone_reverted_to_previous_versio")
                } else {
                    status =
                        tr("editor.undo_failed")
                }
            }
        }
    }

    fun selectTexture(index: Int) {

        if (loading) return
        val sf = scFile ?: return
        if (atlas?.currentTextureIndex == index) return

        loading = true

        scope.launch(Dispatchers.IO) {

            val reloaded = loadScTextureByIndex(ctx, sf, index)

            withContext(Dispatchers.Main) {

                loading = false

                if (reloaded != null) {
                    atlas = reloaded
                    selectedSprite = null
                    searchOpen = false
                    searchQuery = ""
                } else {
                    status =
                        tr("editor.failed_to_load_texture_0", index)
                }
            }
        }
    }

    val versionLabel: String? =
        remember(scFile) {
            try {
                scFile?.inputStream()?.use { st ->
                    val h = ByteArray(3)
                    if (st.read(h) == 3 &&
                        h[0] == 0x53.toByte() &&
                        h[1] == 0x43.toByte()
                    ) {
                        val v = h[2].toInt() and 0xFF
                        if (v >= 5) "SC2 v$v" else "SC"
                    } else null
                }
            } catch (_: Throwable) {
                null
            }
        }

    // ── UI ───────────────────────────────────────────────────────────────────

    if (!legacy) {

        ModernEditorUi(
            isRu = isRu,
            atlas = atlas,
            fileName = scFile?.name,
            versionLabel = versionLabel,
            loading = loading,
            status = status,
            canUndo = fileHistory.isNotEmpty(),
            selected = selectedSprite,
            searchQuery = searchQuery,
            onMenu = onMenu,
            onOpen = { scLauncher.launch("*/*") },
            onUndo = { undoLast() },
            onBatch = { batchLauncher.launch("image/*") },
            onExports = { showExports = true },
            onExportAll = {
                val baseName = scFile?.nameWithoutExtension ?: "sprites"
                exportAllLauncher.launch("${baseName}_export.zip")
            },
            onSelect = { selectedSprite = it },
            onSearchChange = { searchQuery = it },
            onReplace = { pngLauncher.launch("image/*") },
            onExportSprite = { sp ->
                exportSprite = sp
                exportLauncher.launch(sanitizeFileName(sp.displayName) + ".png")
            },
            onPickTexture = { selectTexture(it) },
            onStatusDismiss = { status = "" }
        )

        if (showBatch) {
            BatchReplaceDialog(
                isRu = isRu,
                sprites = atlas?.sprites ?: emptyList(),
                entries = batchEntries,
                onAssign = { i, sp ->
                    if (i in batchEntries.indices)
                        batchEntries[i] = batchEntries[i].copy(sprite = sp)
                },
                onRemove = { i ->
                    if (i in batchEntries.indices)
                        batchEntries.removeAt(i)
                    if (batchEntries.isEmpty()) showBatch = false
                },
                onApply = { runBatch() },
                onCancel = { showBatch = false }
            )
        }

        val exportsPath = scFile?.absolutePath
        if (showExports && exportsPath != null) {
            ExportNamesDialog(
                isRu = isRu,
                scPath = exportsPath,
                onApply = { ops, done -> runExportEdit(ops, done) },
                onDismiss = { showExports = false }
            )
        }

    } else Column(
        Modifier.fillMaxSize()
    ) {

        // TopBar
        Surface(
            color = MaterialTheme.colorScheme.surface,
            tonalElevation = 2.dp
        ) {

            Row(
                Modifier
                    .fillMaxWidth()
                    .padding(
                        horizontal = 16.dp,
                        vertical = 8.dp
                    ),
                verticalAlignment =
                    Alignment.CenterVertically
            ) {

                Text(
                    scFile?.name
                        ?: tr("editor.no_file"),
                    style = MaterialTheme.typography.titleMedium,
                    fontWeight = FontWeight.SemiBold,
                    maxLines = 1,
                    overflow = androidx.compose.ui.text.style.TextOverflow.Ellipsis,
                    modifier = Modifier.weight(1f)
                )

                if (loading) {

                    MdLoadingIndicator(Modifier.size(28.dp))

                    Spacer(
                        Modifier.width(8.dp)
                    )
                }

                if (fileHistory.isNotEmpty()) {

                    IconButton(
                        onClick = {

                            val prev =
                                fileHistory.removeLastOrNull()
                                    ?: return@IconButton

                            loading = true

                            scope.launch(Dispatchers.IO) {

                                val reloaded =
                                    loadScAtlas(
                                        ctx,
                                        prev
                                    )

                                withContext(Dispatchers.Main) {

                                    loading = false

                                    if (reloaded != null) {

                                        scFile = prev
                                        atlas = reloaded
                                        selectedSprite = null

                                        status =
                                            tr("editor.undone_reverted_to_previous_versio")

                                    } else {

                                        status =
                                            tr("editor.undo_failed")
                                    }
                                }
                            }
                        }
                    ) {

                        Icon(
                            Icons.AutoMirrored.Rounded.Undo,
                            contentDescription =
                                tr("editor.undo_last_replace"),

                            tint =
                                MaterialTheme
                                    .colorScheme
                                    .primary
                        )
                    }
                }

                if (atlas != null) FilledTonalButton(
                    onClick = {
                        scLauncher.launch("*/*")
                    },
                    shape =
                        RoundedCornerShape(10.dp)
                ) {

                    Icon(
                        Icons.Rounded.FolderOpen,
                        null,
                        Modifier.size(16.dp)
                    )

                    Spacer(
                        Modifier.width(4.dp)
                    )

                    Text(
                        tr("editor.open_sc")
                    )
                }
            }
        }

        // Быстрые действия (подписанные кнопки)
        if (atlas != null) {

            Row(
                Modifier
                    .fillMaxWidth()
                    .padding(horizontal = 16.dp, vertical = 4.dp),
                horizontalArrangement = Arrangement.spacedBy(8.dp)
            ) {

                FilledTonalButton(
                    onClick = { batchLauncher.launch("image/*") },
                    enabled = !loading,
                    contentPadding = PaddingValues(horizontal = 14.dp),
                    modifier = Modifier.height(38.dp)
                ) {
                    Icon(Icons.Rounded.PhotoLibrary, null, Modifier.size(18.dp))
                    Spacer(Modifier.width(6.dp))
                    Text(
                        tr("editor.batch_replace"),
                        style = MaterialTheme.typography.labelLarge
                    )
                }

                OutlinedButton(
                    onClick = { showExports = true },
                    enabled = !loading,
                    contentPadding = PaddingValues(horizontal = 14.dp),
                    modifier = Modifier.height(38.dp)
                ) {
                    Icon(Icons.Rounded.Edit, null, Modifier.size(18.dp))
                    Spacer(Modifier.width(6.dp))
                    Text(
                        tr("editor.export_names"),
                        style = MaterialTheme.typography.labelLarge
                    )
                }

                OutlinedButton(
                    onClick = {
                        val baseName =
                            scFile?.nameWithoutExtension ?: "sprites"
                        exportAllLauncher.launch("${baseName}_export.zip")
                    },
                    enabled = !loading,
                    contentPadding = PaddingValues(horizontal = 14.dp),
                    modifier = Modifier.height(38.dp)
                ) {
                    Icon(Icons.Rounded.Download, null, Modifier.size(18.dp))
                    Spacer(Modifier.width(6.dp))
                    Text(
                        tr("editor.export_all"),
                        style = MaterialTheme.typography.labelLarge
                    )
                }
            }
        }

        if (showBatch) {
            BatchReplaceDialog(
                isRu = isRu,
                sprites = atlas?.sprites ?: emptyList(),
                entries = batchEntries,
                onAssign = { i, sp ->
                    if (i in batchEntries.indices)
                        batchEntries[i] = batchEntries[i].copy(sprite = sp)
                },
                onRemove = { i ->
                    if (i in batchEntries.indices)
                        batchEntries.removeAt(i)
                    if (batchEntries.isEmpty()) showBatch = false
                },
                onApply = { runBatch() },
                onCancel = { showBatch = false }
            )
        }

        val exportsPath2 = scFile?.absolutePath
        if (showExports && exportsPath2 != null) {
            ExportNamesDialog(
                isRu = isRu,
                scPath = exportsPath2,
                onApply = { ops, done -> runExportEdit(ops, done) },
                onDismiss = { showExports = false }
            )
        }

        // Status
        if (status.isNotEmpty()) {

            Surface(
                Modifier
                    .fillMaxWidth()
                    .padding(
                        horizontal = 16.dp,
                        vertical = 4.dp
                    ),

                shape =
                    RoundedCornerShape(8.dp),

                color =
                    MaterialTheme
                        .colorScheme
                        .surfaceContainerHigh
            ) {

                Text(
                    status,

                    Modifier.padding(8.dp),

                    style =
                        MaterialTheme.typography.bodySmall,

                    fontFamily =
                        FontFamily.Monospace,

                    color =
                        MaterialTheme
                            .colorScheme
                            .onSurface
                )
            }
        }

        /*
         * Multi-texture selector.
         *
         * Виден только когда у файла реально больше
         * одной текстуры (SC1/v0 всегда даёт список
         * из одной записи — тогда селектор скрыт,
         * как и просили: "при одном texture selector
         * вообще можно скрывать").
         *
         * Переключение декодирует ТОЛЬКО выбранную
         * текстуру на native-стороне — остальные не
         * трогаются, что и даёт экономию памяти на
         * файлах вроде ui.sc с несколькими текстурами
         * 4096x4096.
         */
        atlas?.let { a ->

            if (a.textures.size > 1) {

                LazyRow(
                    Modifier
                        .fillMaxWidth()
                        .padding(
                            horizontal = 16.dp,
                            vertical = 4.dp
                        ),

                    horizontalArrangement =
                        Arrangement.spacedBy(6.dp)
                ) {

                    items(
                        a.textures,
                        key = { it.index }
                    ) { tex ->

                        val selected =
                            tex.index ==
                                    a.currentTextureIndex

                        FilterChip(
                            selected = selected,

                            onClick = {

                                if (selected ||
                                    loading)
                                    return@FilterChip

                                val sf =
                                    scFile
                                        ?: return@FilterChip

                                loading = true

                                scope.launch(
                                    Dispatchers.IO
                                ) {

                                    val reloaded =
                                        loadScTextureByIndex(
                                            ctx,
                                            sf,
                                            tex.index
                                        )

                                    withContext(
                                        Dispatchers.Main
                                    ) {

                                        loading = false

                                        if (reloaded != null) {

                                            atlas = reloaded
                                            selectedSprite = null
                                            searchOpen = false
                                            searchQuery = ""

                                        } else {

                                            status =
                                                tr("editor.failed_to_load_texture_0", tex.index)
                                        }
                                    }
                                }
                            },

                            label = {

                                Text(
                                    "Texture ${tex.index} · " +
                                            "${tex.width}×${tex.height}",

                                    style =
                                        MaterialTheme
                                            .typography
                                            .labelSmall
                                )
                            }
                        )
                    }
                }
            }
        }

        if (atlas == null) {

            // Пустой экран
            Box(
                Modifier.fillMaxSize(),
                contentAlignment =
                    Alignment.Center
            ) {

                Column(
                    horizontalAlignment =
                        Alignment.CenterHorizontally,

                    verticalArrangement =
                        Arrangement.spacedBy(12.dp)
                ) {

                    Icon(
                        Icons.Rounded.Image,
                        null,
                        Modifier.size(64.dp),

                        tint =
                            MaterialTheme
                                .colorScheme
                                .outlineVariant
                    )

                    Text(
                        tr("editor.open_an_sc_file_to_edit"),

                        style =
                            MaterialTheme.typography.bodyLarge,

                        color =
                            MaterialTheme
                                .colorScheme
                                .onSurfaceVariant
                    )

                    Button(onClick = { scLauncher.launch("*/*") }) {
                        Icon(Icons.Rounded.FolderOpen, null, Modifier.size(18.dp))
                        Spacer(Modifier.width(6.dp))
                        Text(tr("editor.open_sc"))
                    }
                }
            }

        } else {

            val a = atlas!!

            /*
             * Отфильтрованный список по search query.
             *
             * Пересчитывается только когда меняется
             * запрос или сам атлас — не на каждый кадр.
             */
            val filteredSprites by remember(
                a,
                searchQuery
            ) {

                derivedStateOf {

                    if (searchQuery.isBlank())
                        a.sprites
                    else
                        a.sprites.filter {
                            it.displayName.contains(
                                searchQuery,
                                ignoreCase = true
                            )
                        }
                }
            }

            BoxWithConstraints(
                Modifier.fillMaxSize()
            ) {

                /*
                 * Landscape/широкий экран:
                 * sprites | atlas | info.
                 *
                 * Portrait: как раньше —
                 * sprites | atlas (сверху), info (снизу).
                 *
                 * Решаем по доступной ширине, а не
                 * по жёсткой проверке orientation.
                 */
                val isWide =
                    maxWidth >= 560.dp

                Row(
                    Modifier.fillMaxSize()
                ) {

                    // ── Левая панель: список спрайтов ────────────────────

                    Surface(
                        Modifier
                            .width(180.dp)
                            .fillMaxHeight(),

                        color =
                            MaterialTheme
                                .colorScheme
                                .surfaceContainerLow
                    ) {

                        Column {

                            SpriteListHeader(
                                isRu = isRu,

                                totalCount =
                                    a.sprites.size,

                                searchOpen =
                                    searchOpen,

                                searchQuery =
                                    searchQuery,

                                onSearchToggle = {

                                    searchOpen =
                                        !searchOpen

                                    if (!searchOpen)
                                        searchQuery = ""
                                },

                                onQueryChange = {
                                    searchQuery = it
                                },

                                onExportAll = {

                                    val baseName =
                                        scFile
                                            ?.nameWithoutExtension
                                            ?: "sprites"

                                    exportAllLauncher.launch(
                                        "${baseName}_export.zip"
                                    )
                                }
                            )

                            val listState =
                                rememberLazyListState()

                            LaunchedEffect(
                                selectedSprite
                            ) {

                                selectedSprite?.let { sp ->

                                    val idx =
                                        filteredSprites
                                            .indexOfFirst {
                                                it.id == sp.id
                                            }

                                    if (idx >= 0)
                                        listState
                                            .animateScrollToItem(idx)
                                }
                            }

                            LazyColumn(
                                state = listState
                            ) {

                                items(
                                    filteredSprites,
                                    key = { it.id }
                                ) { sprite ->

                                    SpriteListItem(
                                        sprite = sprite,

                                        selected =
                                            selectedSprite?.id ==
                                                    sprite.id,

                                        onClick = {
                                            selectedSprite =
                                                sprite
                                        }
                                    )
                                }
                            }
                        }
                    }

                    VerticalDivider()

                    // ── Атлас + инфо ──────────────────────────────────────

                    if (isWide) {

                        // Landscape: atlas | info рядом.

                        AtlasView(
                            atlas = a,

                            selected =
                                selectedSprite,

                            modifier =
                                Modifier
                                    .weight(1f)
                                    .fillMaxHeight(),

                            onSpriteClick = { sp ->
                                selectedSprite = sp
                            }
                        )

                        if (selectedSprite != null) {

                            VerticalDivider()

                            SpriteInfoPanel(
                                sprite =
                                    selectedSprite!!,

                                atlas = a,

                                isRu = isRu,

                                wide = true,

                                onExport = { sp ->

                                    exportSprite = sp

                                    val baseName =
                                        sanitizeFileName(
                                            sp.displayName
                                        )

                                    exportLauncher.launch(
                                        "$baseName.png"
                                    )
                                },

                                onReplace = {
                                    pngLauncher.launch(
                                        "image/*"
                                    )
                                }
                            )
                        }

                    } else {

                        // Portrait: atlas сверху, info снизу.

                        Column(
                            Modifier
                                .weight(1f)
                                .fillMaxHeight()
                        ) {

                            AtlasView(
                                atlas = a,

                                selected =
                                    selectedSprite,

                                modifier =
                                    Modifier
                                        .weight(1f)
                                        .fillMaxWidth(),

                                onSpriteClick = { sp ->
                                    selectedSprite = sp
                                }
                            )

                            selectedSprite?.let { sp ->

                                HorizontalDivider()

                                SpriteInfoPanel(
                                    sprite = sp,

                                    atlas = a,

                                    isRu = isRu,

                                    wide = false,

                                    onExport = { s ->

                                        exportSprite = s

                                        val baseName =
                                            sanitizeFileName(
                                                s.displayName
                                            )

                                        exportLauncher.launch(
                                            "$baseName.png"
                                        )
                                    },

                                    onReplace = {
                                        pngLauncher.launch(
                                            "image/*"
                                        )
                                    }
                                )
                            }
                        }
                    }
                }
            }
        }
    }
}

// ── Заголовок списка спрайтов с поиском ────────────────────────────────────────

@Composable
fun SpriteListHeader(
    isRu: Boolean,
    totalCount: Int,
    searchOpen: Boolean,
    searchQuery: String,
    onSearchToggle: () -> Unit,
    onQueryChange: (String) -> Unit,
    onExportAll: () -> Unit
) {

    if (searchOpen) {

        val focusRequester =
            remember { FocusRequester() }

        LaunchedEffect(Unit) {
            focusRequester.requestFocus()
        }

        Row(
            Modifier
                .fillMaxWidth()
                .padding(
                    horizontal = 8.dp,
                    vertical = 6.dp
                ),

            verticalAlignment =
                Alignment.CenterVertically
        ) {

            OutlinedTextField(
                value = searchQuery,

                onValueChange = onQueryChange,

                modifier =
                    Modifier
                        .weight(1f)
                        .focusRequester(focusRequester),

                singleLine = true,

                textStyle =
                    MaterialTheme.typography.bodySmall,

                placeholder = {
                    Text(
                        tr("editor.search"),

                        style =
                            MaterialTheme
                                .typography
                                .bodySmall
                    )
                },

                shape =
                    RoundedCornerShape(8.dp)
            )

            IconButton(
                onClick = onExportAll,

                modifier =
                    Modifier.size(36.dp)
            ) {

                Icon(
                    Icons.Rounded.Download,

                    tr("editor.export_all_matching"),

                    Modifier.size(16.dp)
                )
            }

            IconButton(
                onClick = onSearchToggle,

                modifier =
                    Modifier.size(36.dp)
            ) {

                Icon(
                    Icons.Rounded.Close,
                    null,
                    Modifier.size(18.dp)
                )
            }
        }

    } else {

        Row(
            Modifier
                .fillMaxWidth()
                .padding(
                    start = 12.dp,
                    end = 4.dp
                ),

            verticalAlignment =
                Alignment.CenterVertically
        ) {

            Text(
                tr("editor.sprites_0", totalCount),

                Modifier
                    .weight(1f)
                    .padding(vertical = 12.dp),

                style =
                    MaterialTheme
                        .typography
                        .labelMedium,

                color =
                    MaterialTheme
                        .colorScheme
                        .primary
            )

            IconButton(
                onClick = onExportAll,

                modifier =
                    Modifier.size(32.dp)
            ) {

                Icon(
                    Icons.Rounded.Download,

                    tr("editor.export_all_2"),

                    Modifier.size(16.dp),

                    tint =
                        MaterialTheme
                            .colorScheme
                            .primary
                )
            }

            IconButton(
                onClick = onSearchToggle,

                modifier =
                    Modifier.size(32.dp)
            ) {

                Icon(
                    Icons.Rounded.Search,

                    null,

                    Modifier.size(16.dp),

                    tint =
                        MaterialTheme
                            .colorScheme
                            .primary
                )
            }
        }
    }
}

// ── Компактная информация о выбранном спрайте ───────────────────────────────────

@Composable
fun SpriteInfoPanel(
    sprite: ScSprite,
    atlas: ScAtlas,
    isRu: Boolean,
    wide: Boolean,
    onExport: (ScSprite) -> Unit,
    onReplace: () -> Unit
) {

    /*
     * Превью спрайта отдельно от атласа.
     *
     * Кропаем асинхронно и кэшируем по sprite.id —
     * без этого Bitmap.createBitmap пересчитывался бы
     * на каждую recomposition (например, при вводе
     * в поиске), что дорого на слабых устройствах.
     */
    var previewBitmap by remember(sprite.id) {
        mutableStateOf<ImageBitmap?>(null)
    }

    LaunchedEffect(sprite.id, atlas) {

        val bmp =
            withContext(Dispatchers.Default) {

                try {

                    cropSpriteBitmap(
                        atlas,
                        sprite
                    )

                } catch (_: Exception) {
                    null
                }
            }

        previewBitmap = bmp
    }

    val content: @Composable ColumnScope.() -> Unit = {

        Row(
            verticalAlignment =
                Alignment.CenterVertically,

            horizontalArrangement =
                Arrangement.spacedBy(8.dp)
        ) {

            // Превью спрайта, отдельно от атласа.
            Box(
                Modifier
                    .size(40.dp)
                    .clip(RoundedCornerShape(6.dp))
                    .background(
                        checkerBrushColor(
                            MaterialTheme.colorScheme
                        )
                    ),

                contentAlignment =
                    Alignment.Center
            ) {

                previewBitmap?.let { pb ->

                    Image(
                        bitmap = pb,
                        contentDescription = null,

                        modifier =
                            Modifier.fillMaxSize(),

                        contentScale =
                            ContentScale.Fit
                    )
                }
            }

            /*
             * Компактная строка:
             * rt · ID 1488 · 64x64
             */
            Text(
                "${sprite.displayName} · ID ${sprite.id} · " +
                        "${sprite.w.toInt()}×${sprite.h.toInt()}",

                style =
                    MaterialTheme
                        .typography
                        .bodySmall,

                fontWeight =
                    FontWeight.Bold,

                maxLines = 2,

                modifier =
                    Modifier.weight(1f),

                color =
                    MaterialTheme
                        .colorScheme
                        .onSurface
            )
        }

        Text(
            "UV (${sprite.u0.toInt()},${sprite.v0.toInt()}) – " +
                    "(${sprite.u1.toInt()},${sprite.v1.toInt()})",

            style =
                MaterialTheme
                    .typography
                    .labelSmall,

            fontFamily =
                FontFamily.Monospace,

            color =
                MaterialTheme
                    .colorScheme
                    .onSurfaceVariant
        )

        val buttonModifier =
            if (wide)
                Modifier.fillMaxWidth()
            else
                Modifier.weight(1f)

        val buttonsArrangement =
            if (wide)
                Arrangement.spacedBy(6.dp)
            else
                Arrangement.spacedBy(8.dp)

        @Composable
        fun Buttons() {

            FilledTonalButton(
                onClick = {
                    onExport(sprite)
                },

                contentPadding =
                    PaddingValues(
                        horizontal = 10.dp,
                        vertical = 10.dp
                    ),

                shape =
                    RoundedCornerShape(10.dp),

                modifier = buttonModifier
            ) {

                Icon(
                    Icons.Rounded.FileDownload,
                    null,
                    Modifier.size(16.dp)
                )

                Spacer(Modifier.width(4.dp))

                Text(
                    tr("editor.save_png"),

                    maxLines = 1,

                    style =
                        MaterialTheme
                            .typography
                            .labelMedium
                )
            }

            FilledTonalButton(
                onClick = onReplace,

                contentPadding =
                    PaddingValues(
                        horizontal = 10.dp,
                        vertical = 10.dp
                    ),

                shape =
                    RoundedCornerShape(10.dp),

                modifier = buttonModifier
            ) {

                Icon(
                    Icons.Rounded.Edit,
                    null,
                    Modifier.size(16.dp)
                )

                Spacer(Modifier.width(4.dp))

                Text(
                    tr("editor.replace"),

                    maxLines = 1,

                    style =
                        MaterialTheme
                            .typography
                            .labelMedium
                )
            }
        }

        if (wide) {

            Column(
                verticalArrangement =
                    buttonsArrangement
            ) {
                Buttons()
            }

        } else {

            Row(
                Modifier.fillMaxWidth(),

                horizontalArrangement =
                    buttonsArrangement
            ) {
                Buttons()
            }
        }
    }

    if (wide) {

        Surface(
            Modifier
                .width(200.dp)
                .fillMaxHeight(),

            color =
                MaterialTheme
                    .colorScheme
                    .surfaceContainerHigh
        ) {

            Column(
                Modifier
                    .padding(12.dp)
                    .verticalScroll(
                        rememberScrollState()
                    ),

                verticalArrangement =
                    Arrangement.spacedBy(8.dp),

                content = content
            )
        }

    } else {

        Surface(
            color =
                MaterialTheme
                    .colorScheme
                    .surfaceContainerHigh
        ) {

            Column(
                Modifier.padding(12.dp),

                verticalArrangement =
                    Arrangement.spacedBy(6.dp),

                content = content
            )
        }
    }
}

// ── Безопасное имя файла ──────────────────────────────────────────────────────

fun sanitizeFileName(name: String): String {

    if (name.isBlank())
        return "sprite"

    return name
        .replace(Regex("[\\\\/:*?\"<>|]"), "_")
        .trim()
        .ifBlank { "sprite" }
}

// ── Polygon geometry: hit-test через triangle fan ───────────────────────────────

/*
 * Барицентрический тест "точка внутри треугольника".
 */
fun pointInTriangle(
    px: Float, py: Float,
    ax: Float, ay: Float,
    bx: Float, by: Float,
    cx: Float, cy: Float
): Boolean {

    /*
     * Площадь треугольника (удвоенная, со знаком)
     * через векторное произведение (B-A)×(C-A).
     *
     * БАГ, который тут был: если треугольник
     * вырожден (площадь ≈ 0 — все три точки совпадают
     * или лежат на одной линии, что реально бывает
     * у некоторых spritе'ов с нулевым bbox), то
     * d1/d2/d3 ниже все равны нулю для ЛЮБОЙ точки
     * плоскости — hasNeg=false и hasPos=false, и
     * !(false && false) = true, то есть функция
     * ошибочно говорит "внутри" вообще для всех
     * координат экрана. Это и вызывало "любой тап
     * выбирает один и тот же спрайт" — у него был
     * вырожденный (нулевого размера) полигон.
     */
    val area =
        (bx - ax) * (cy - ay) -
                (cx - ax) * (by - ay)

    if (kotlin.math.abs(area) < 1e-6f)
        return false

    val d1 =
        (px - bx) * (ay - by) -
                (ax - bx) * (py - by)

    val d2 =
        (px - cx) * (by - cy) -
                (bx - cx) * (py - cy)

    val d3 =
        (px - ax) * (cy - ay) -
                (cx - ax) * (py - ay)

    val hasNeg =
        (d1 < 0) || (d2 < 0) || (d3 < 0)

    val hasPos =
        (d1 > 0) || (d2 > 0) || (d3 > 0)

    return !(hasNeg && hasPos)
}

/*
 * Разбивает полигон на треугольники через triangle
 * fan (от первой вершины) и проверяет попадание
 * точки в любой из них.
 *
 * polygon [p0,p1,p2,p3] =
 * triangle(p0,p1,p2) + triangle(p0,p2,p3)
 */
fun polygonContainsPoint(
    points: List<Offset>,
    x: Float,
    y: Float
): Boolean {

    if (points.size < 3)
        return false

    for (i in 1 until points.size - 1) {

        val a = points[0]
        val b = points[i]
        val c = points[i + 1]

        if (pointInTriangle(
                x, y,
                a.x, a.y,
                b.x, b.y,
                c.x, c.y
            ))
            return true
    }

    return false
}

/*
 * Проверяет попадание точки в спрайт — по любому
 * из его полигонов (у спрайта может быть несколько
 * draw-команд/кусков).
 */
fun spriteContainsPoint(
    sprite: ScSprite,
    x: Float,
    y: Float
): Boolean {

    for (poly in sprite.polygons) {

        if (polygonContainsPoint(
                poly.points,
                x,
                y
            ))
            return true
    }

    return false
}

/*
 * Упорядочивает вершины полигона по углу вокруг
 * их центроида — превращает произвольный набор
 * точек на границе выпуклой фигуры в простой
 * (не самопересекающийся) контур.
 *
 * Зачем это нужно: порядок вершин, как они
 * записаны в SC-файле (ShapeDrawBitmapCommand),
 * не является порядком обхода контура — это
 * какой-то внутренний порядок, оптимальный для
 * GPU-рендеринга (fan/strip с переиспользованием),
 * а не последовательность "иди по границе".
 * Если строить полигон/triangle-fan прямо в этом
 * исходном порядке, получаются самопересекающиеся,
 * вырожденные треугольники — то и была причина
 * "нарезанных на куски" силуэтов при экспорте.
 *
 * Сортировка по углу от центроида надёжно
 * восстанавливает простой контур для выпуклых
 * силуэтов (подавляющее большинство мелких
 * UI/hero-иконок в SC-файлах). Для существенно
 * невыпуклых форм (со впадинами) этот метод
 * может дать не идеальный результат — но исходный
 * порядок вершин всё равно не давал верного
 * контура вообще, так что regression тут нет.
 */
fun sortPointsByAngle(
    points: List<Offset>
): List<Offset> {

    if (points.size < 3)
        return points

    val cx =
        points.sumOf { it.x.toDouble() }
            .toFloat() / points.size

    val cy =
        points.sumOf { it.y.toDouble() }
            .toFloat() / points.size

    return points.sortedBy { p ->

        kotlin.math.atan2(
            (p.y - cy).toDouble(),
            (p.x - cx).toDouble()
        )
    }
}

// ── Кроп спрайта из атласа ────────────────────────────────────────────────────

/*
 * Вырезает Bitmap спрайта из атласа по его UV.
 *
 * Общая логика для Export PNG и для превью
 * в SpriteInfoPanel — раньше была продублирована.
 *
 * Возвращает null при ошибке вместо исключения,
 * чтобы вызывающий код сам решал, как реагировать.
 */
fun cropSpriteBitmapWithSize(
    atlas: ScAtlas,
    sprite: ScSprite
): Triple<Bitmap, Int, Int>? {

    return try {

        val source =
            atlas.bitmap.asAndroidBitmap()

        /*
         * Координаты спрайта в исходном PNG.
         *
         * Округляем наружу, чтобы не потерять крайний
         * пиксель из-за float UV.
         */
        val left =
            sprite.u0
                .toInt()
                .coerceIn(0, source.width - 1)

        val top =
            sprite.v0
                .toInt()
                .coerceIn(0, source.height - 1)

        val right =
            kotlin.math.ceil(sprite.u1.toDouble())
                .toInt()
                .coerceIn(left + 1, source.width)

        val bottom =
            kotlin.math.ceil(sprite.v1.toDouble())
                .toInt()
                .coerceIn(top + 1, source.height)

        val width =
            (right - left)
                .coerceAtLeast(1)

        val height =
            (bottom - top)
                .coerceAtLeast(1)

        val cropped =
            Bitmap.createBitmap(
                source,
                left,
                top,
                width,
                height
            )

        /*
         * Маскируем по реальному контуру, если он
         * есть — пиксели вне полигона становятся
         * прозрачными, а не остаются частью
         * прямоугольного bbox.
         *
         * Несколько полигонов объединяются (union) —
         * пиксель остаётся видимым, если попадает
         * в любой из них.
         */
        val masked =
            if (sprite.polygons.isNotEmpty())
                maskBitmapByPolygons(
                    cropped,
                    sprite.polygons,
                    left,
                    top
                )
            else
                cropped

        Triple(masked, width, height)

    } catch (_: Exception) {
        null
    }
}

/*
 * Обнуляет альфа-канал пикселей, не попадающих ни
 * в один из полигонов спрайта.
 *
 * Полигоны хранятся в координатах атласа — offsetX/
 * offsetY переводят их в локальные координаты уже
 * вырезанного bbox-Bitmap (left/top кропа).
 *
 * Использует Canvas + Path + PorterDuff DST_IN —
 * это быстрая, надёжная растеризация через Android
 * Skia backend, а не ручной pixel-by-pixel цикл
 * (медленнее и подвержен ошибкам на границах).
 */
fun maskBitmapByPolygons(
    source: Bitmap,
    polygons: List<ScPolygon>,
    offsetX: Int,
    offsetY: Int
): Bitmap {

    val maskBitmap =
        Bitmap.createBitmap(
            source.width,
            source.height,
            Bitmap.Config.ARGB_8888
        )

    val maskCanvas =
        android.graphics.Canvas(maskBitmap)

    val maskPaint =
        android.graphics.Paint(
            android.graphics.Paint.ANTI_ALIAS_FLAG
        ).apply {
            color =
                android.graphics.Color.WHITE

            style =
                android.graphics.Paint.Style.FILL
        }

    for (poly in polygons) {

        if (poly.points.size < 3)
            continue

        val path =
            android.graphics.Path()

        val first =
            poly.points[0]

        path.moveTo(
            first.x - offsetX,
            first.y - offsetY
        )

        for (i in 1 until poly.points.size) {

            val pt =
                poly.points[i]

            path.lineTo(
                pt.x - offsetX,
                pt.y - offsetY
            )
        }

        path.close()

        maskCanvas.drawPath(path, maskPaint)
    }

    val result =
        source.copy(
            Bitmap.Config.ARGB_8888,
            true
        )

    val resultCanvas =
        android.graphics.Canvas(result)

    val dstInPaint =
        android.graphics.Paint().apply {

            xfermode =
                android.graphics.PorterDuffXfermode(
                    android.graphics.PorterDuff.Mode.DST_IN
                )
        }

    resultCanvas.drawBitmap(
        maskBitmap,
        0f,
        0f,
        dstInPaint
    )

    maskBitmap.recycle()

    return result
}

/*
 * Обёртка для превью: сразу ImageBitmap,
 * без промежуточных размеров.
 *
 * Вызывать вне Main thread — используется
 * из LaunchedEffect с Dispatchers.Default.
 */
fun cropSpriteBitmap(
    atlas: ScAtlas,
    sprite: ScSprite
): ImageBitmap? {

    val result =
        cropSpriteBitmapWithSize(
            atlas,
            sprite
        )
            ?: return null

    return result.first.asImageBitmap()
}

// ── Цвет фона превью (лёгкая шахматка через сплошной тон) ───────────────────────

/*
 * Компактная замена полноценной шахматки под
 * маленькое превью 40dp — визуально почти
 * неотличимо, но не требует рисования клеток.
 */
fun checkerBrushColor(
    colorScheme: ColorScheme
): Color {

    return colorScheme.surfaceContainerHighest
}

// ── Список спрайтов ───────────────────────────────────────────────────────────

@Composable
fun SpriteListItem(
    sprite: ScSprite,
    selected: Boolean,
    onClick: () -> Unit
) {

    Surface(
        modifier =
            Modifier
                .fillMaxWidth()
                .clickable(onClick = onClick),

        color =
            if (selected)
                MaterialTheme
                    .colorScheme
                    .primaryContainer
            else
                Color.Transparent
    ) {

        Column(
            Modifier.padding(
                horizontal = 12.dp,
                vertical = 8.dp
            )
        ) {

            Text(
                sprite.displayName,

                style =
                    MaterialTheme
                        .typography
                        .bodySmall,

                fontWeight =
                    if (selected)
                        FontWeight.Bold
                    else
                        FontWeight.Normal,

                color =
                    if (selected)
                        MaterialTheme
                            .colorScheme
                            .onPrimaryContainer
                    else
                        MaterialTheme
                            .colorScheme
                            .onSurface,

                maxLines = 1
            )

            Text(
                "${sprite.w.toInt()}x${sprite.h.toInt()}px",

                style =
                    MaterialTheme
                        .typography
                        .labelSmall,

                color =
                    MaterialTheme
                        .colorScheme
                        .onSurfaceVariant
            )
        }
    }
}

// ── Просмотр атласа ───────────────────────────────────────────────────────────

/*
 * Внешнее управление зумом AtlasView (кнопки +/−/сброс в новом интерфейсе).
 * Жесты щипком работают как раньше.
 */
class AtlasZoomState {
    var tick by mutableStateOf(0)
    var action by mutableStateOf(0)
    fun zoomIn() { action = 1; tick++ }
    fun zoomOut() { action = 2; tick++ }
    fun reset() { action = 3; tick++ }
}

@Composable
fun AtlasView(
    atlas: ScAtlas,
    selected: ScSprite?,
    modifier: Modifier = Modifier,
    onSpriteClick: (ScSprite?) -> Unit = {},
    zoomState: AtlasZoomState? = null
) {

    var scale by remember {
        mutableStateOf(1f)
    }

    var offset by remember {
        mutableStateOf(Offset.Zero)
    }

    var lastFitScale by remember {
        mutableStateOf(1f)
    }

    var lastOx by remember {
        mutableStateOf(0f)
    }

    var lastOy by remember {
        mutableStateOf(0f)
    }

    val zoomTick = zoomState?.tick ?: 0
    LaunchedEffect(zoomTick) {
        val zs = zoomState
        if (zs != null && zoomTick > 0) {
            if (zs.action == 1) scale = (scale * 1.3f).coerceIn(0.2f, 8f)
            else if (zs.action == 2) scale = (scale / 1.3f).coerceIn(0.2f, 8f)
            else if (zs.action == 3) {
                scale = 1f
                offset = Offset.Zero
            }
        }
    }

    Box(

        modifier
            .background(
                MaterialTheme
                    .colorScheme
                    .surfaceContainerLowest
            )

            .clip(
                RoundedCornerShape(0.dp)
            )

            .pointerInput(Unit) {

                detectTransformGestures {
                    _,
                    pan,
                    zoom,
                    _ ->

                    scale =
                        (scale * zoom)
                            .coerceIn(0.2f, 8f)

                    offset += pan
                }
            }

            .pointerInput(atlas) {

                detectTapGestures { tapOffset ->

                    val fs =
                        lastFitScale

                    val ox =
                        lastOx

                    val oy =
                        lastOy

                    /*
                     * Точка тапа в координатах атласа
                     * (пиксели, не экрана) — полигоны
                     * хранятся в пикселях атласа.
                     */
                    val localX =
                        (tapOffset.x - ox) / fs

                    val localY =
                        (tapOffset.y - oy) / fs

                    val hit =
                        atlas.sprites
                            .lastOrNull { sp ->

                                if (sp.polygons.isNotEmpty()) {

                                    spriteContainsPoint(
                                        sp,
                                        localX,
                                        localY
                                    )

                                } else {

                                    // Fallback: bbox,
                                    // как раньше, для
                                    // спрайтов без
                                    // геометрии.
                                    localX in sp.u0..sp.u1 &&
                                            localY in sp.v0..sp.v1
                                }
                            }

                    /*
                     * Тап мимо всех спрайтов
                     * снимает выделение.
                     */
                    onSpriteClick(hit)
                }
            }

    ) {

        Canvas(
            Modifier.fillMaxSize()
        ) {

            val canvasW =
                size.width

            val canvasH =
                size.height

            val fitScale =
                minOf(
                    canvasW / atlas.width,
                    canvasH / atlas.height
                ) * scale

            val drawW =
                atlas.width *
                        fitScale

            val drawH =
                atlas.height *
                        fitScale

            val ox =
                (canvasW - drawW) / 2 +
                        offset.x

            val oy =
                (canvasH - drawH) / 2 +
                        offset.y

            lastFitScale =
                fitScale

            lastOx =
                ox

            lastOy =
                oy

            /*
             * Шахматный фон — только в видимой
             * области экрана, а не по всему атласу.
             *
             * При zoom drawW/drawH могут быть
             * в разы больше canvas — без этого
             * ограничения цикл рисует десятки тысяч
             * ячеек на кадр и лагает на слабых
             * устройствах.
             */
            val cell = 16f

            val visLeft =
                maxOf(ox, 0f)

            val visTop =
                maxOf(oy, 0f)

            val visRight =
                minOf(ox + drawW, canvasW)

            val visBottom =
                minOf(oy + drawH, canvasH)

            if (visRight > visLeft &&
                visBottom > visTop) {

                var cy =
                    (
                        kotlin.math.floor((visTop - oy) / cell) * cell + oy
                    )

                var row =
                    kotlin.math.floor((visTop - oy) / cell)
                        .toInt()

                while (cy < visBottom) {

                    var cx =
                        (
                            kotlin.math.floor((visLeft - ox) / cell) * cell + ox
                        )

                    var col =
                        kotlin.math.floor((visLeft - ox) / cell)
                            .toInt()

                    while (cx < visRight) {

                        val rectLeft =
                            cx.coerceAtLeast(visLeft)

                        val rectTop =
                            cy.coerceAtLeast(visTop)

                        val rectRight =
                            (cx + cell).coerceAtMost(visRight)

                        val rectBottom =
                            (cy + cell).coerceAtMost(visBottom)

                        drawRect(

                            color =
                                if ((row + col) % 2 == 0)
                                    Color(0xFFCCCCCC)
                                else
                                    Color.White,

                            topLeft =
                                Offset(
                                    rectLeft,
                                    rectTop
                                ),

                            size =
                                androidx.compose.ui.geometry
                                    .Size(
                                        rectRight - rectLeft,
                                        rectBottom - rectTop
                                    )
                        )

                        cx += cell
                        col++
                    }

                    cy += cell
                    row++
                }
            }

            // Атлас
            drawImage(

                image =
                    atlas.bitmap,

                dstOffset =
                    androidx.compose.ui.unit.IntOffset(
                        ox.toInt(),
                        oy.toInt()
                    ),

                dstSize =
                    androidx.compose.ui.unit.IntSize(
                        drawW.toInt(),
                        drawH.toInt()
                    )
            )

            /*
             * Рамки спрайтов — только для тех,
             * что реально видны в текущем viewport,
             * и только если их не слишком много.
             *
             * На плотных UI-атласах (тысячи мелких
             * элементов вроде icon/popup/offer)
             * рамки каждого видимого спрайта сливаются
             * в нечитаемую сетку и только мешают
             * смотреть на картинку — выше порога просто
             * не рисуем их вовсе, оставляя только
             * рамку выбранного спрайта (см. ниже).
             */
            val visibleSprites =
                atlas.sprites.filter { sp ->

                    val sx =
                        ox + sp.u0 * fitScale

                    val sy =
                        oy + sp.v0 * fitScale

                    val sw =
                        sp.w * fitScale

                    val sh =
                        sp.h * fitScale

                    sw >= 1f && sh >= 1f &&
                            sx + sw >= 0f &&
                            sy + sh >= 0f &&
                            sx <= canvasW &&
                            sy <= canvasH
                }

            if (visibleSprites.size <= 150) {

                for (sp in visibleSprites) {

                    val sx =
                        ox +
                                sp.u0 *
                                fitScale

                    val sy =
                        oy +
                                sp.v0 *
                                fitScale

                    val sw =
                        sp.w *
                                fitScale

                    val sh =
                        sp.h *
                                fitScale

                    drawRect(

                        color =
                            Color(0x6600CC44),

                        topLeft =
                            Offset(
                                sx,
                                sy
                            ),

                        size =
                            androidx.compose.ui.geometry
                                .Size(
                                    sw,
                                    sh
                                ),

                        style =
                            Stroke(1f)
                    )
                }
            }

            // Выбранный спрайт
            selected?.let { sp ->

                val sx =
                    ox +
                            sp.u0 *
                            fitScale

                val sy =
                    oy +
                            sp.v0 *
                            fitScale

                val sw =
                    sp.w *
                            fitScale

                val sh =
                    sp.h *
                            fitScale

                /*
                 * Лёгкое затемнение остального atlas
                 * вокруг выбранного спрайта.
                 *
                 * Дёшево: 4 прямоугольника-маски
                 * вместо блюра или нового Bitmap,
                 * не влияет на плавность zoom/pan.
                 */
                val dim =
                    Color(0x66000000)

                // Сверху
                drawRect(
                    color = dim,
                    topLeft = Offset(ox, oy),
                    size =
                        androidx.compose.ui.geometry
                            .Size(drawW, (sy - oy).coerceAtLeast(0f))
                )

                // Снизу
                drawRect(
                    color = dim,
                    topLeft = Offset(ox, sy + sh),
                    size =
                        androidx.compose.ui.geometry
                            .Size(
                                drawW,
                                (oy + drawH - (sy + sh))
                                    .coerceAtLeast(0f)
                            )
                )

                // Слева
                drawRect(
                    color = dim,
                    topLeft = Offset(ox, sy),
                    size =
                        androidx.compose.ui.geometry
                            .Size((sx - ox).coerceAtLeast(0f), sh)
                )

                // Справа
                drawRect(
                    color = dim,
                    topLeft = Offset(sx + sw, sy),
                    size =
                        androidx.compose.ui.geometry
                            .Size(
                                (ox + drawW - (sx + sw))
                                    .coerceAtLeast(0f),
                                sh
                            )
                )

                if (sp.polygons.isNotEmpty()) {

                    /*
                     * Реальный контур спрайта —
                     * рисуем Path по вершинам каждого
                     * полигона (может быть несколько
                     * кусков у одного спрайта).
                     */
                    for (poly in sp.polygons) {

                        if (poly.points.size < 3)
                            continue

                        val path =
                            androidx.compose.ui.graphics.Path()

                        val first =
                            poly.points[0]

                        path.moveTo(
                            ox + first.x * fitScale,
                            oy + first.y * fitScale
                        )

                        for (i in 1 until poly.points.size) {

                            val pt =
                                poly.points[i]

                            path.lineTo(
                                ox + pt.x * fitScale,
                                oy + pt.y * fitScale
                            )
                        }

                        path.close()

                        drawPath(
                            path = path,
                            color = Color(0x33FF6600)
                        )

                        drawPath(
                            path = path,
                            color = Color(0xFFFF6600),
                            style = Stroke(2.5f)
                        )
                    }

                } else {

                    drawRect(

                        color =
                            Color(0x33FF6600),

                        topLeft =
                            Offset(
                                sx,
                                sy
                            ),

                        size =
                            androidx.compose.ui.geometry
                                .Size(
                                    sw,
                                    sh
                                )
                    )

                    drawRect(

                        color =
                            Color(0xFFFF6600),

                        topLeft =
                            Offset(
                                sx,
                                sy
                            ),

                        size =
                            androidx.compose.ui.geometry
                                .Size(
                                    sw,
                                    sh
                                ),

                        style =
                            Stroke(2.5f)
                    )
                }
            }
        }

        Text(
            "%.0f%%".format(
                scale * 100
            ),

            modifier =
                Modifier
                    .align(
                        Alignment.BottomEnd
                    )
                    .padding(8.dp),

            style =
                MaterialTheme
                    .typography
                    .labelSmall,

            color =
                MaterialTheme
                    .colorScheme
                    .onSurfaceVariant
        )
    }
}

// ── Загрузка SC ───────────────────────────────────────────────────────────────

/*
 * Читает raw RGBA atlas-файл (8-байтный заголовок
 * width/height + пиксели) и собирает Bitmap напрямую.
 *
 * Вынесено отдельно от loadScAtlas, т.к. используется
 * также при переключении текстуры в multi-texture
 * selector (loadScTextureByIndex) — там SC-файл заново
 * не читается и не парсится целиком, только конкретная
 * текстура декодируется native-стороной и результат
 * читается этой же функцией.
 */
fun readRawAtlasBitmap(
    rawFile: File
): ImageBitmap? {

    if (!rawFile.exists())
        return null

    return rawFile.inputStream().use { stream ->

        val header =
            ByteArray(8)

        var read = 0

        while (read < 8) {

            val n =
                stream.read(
                    header,
                    read,
                    8 - read
                )

            if (n < 0)
                return null

            read += n
        }

        val headerBuf =
            java.nio.ByteBuffer
                .wrap(header)
                .order(
                    java.nio.ByteOrder.LITTLE_ENDIAN
                )

        val w =
            headerBuf.getInt(0)

        val h =
            headerBuf.getInt(4)

        if (w <= 0 || h <= 0 ||
            w.toLong() * h.toLong() > 64_000_000L)
            return null

        val pixelBytes =
            w.toLong() * h.toLong() * 4L

        val pixelBuf =
            java.nio.ByteBuffer
                .allocateDirect(
                    pixelBytes.toInt()
                )

        val chunk =
            ByteArray(1 shl 16)

        var remaining =
            pixelBytes.toInt()

        while (remaining > 0) {

            val n =
                stream.read(
                    chunk,
                    0,
                    minOf(
                        chunk.size,
                        remaining
                    )
                )

            if (n < 0)
                return null

            pixelBuf.put(
                chunk,
                0,
                n
            )

            remaining -= n
        }

        pixelBuf.rewind()

        val bitmap =
            Bitmap.createBitmap(
                w,
                h,
                Bitmap.Config.ARGB_8888
            )

        bitmap.copyPixelsFromBuffer(
            pixelBuf
        )

        bitmap.asImageBitmap()
    }
}

/*
 * Парсит опциональную первую строку ответа native
 * стороны вида "TEXTURES:0=4096x4096,1=..." —
 * присутствует только для SC2 (v5/v6) файлов.
 *
 * Возвращает список текстур и остаток строки
 * (без этой служебной первой строки) для
 * дальнейшего parseSpritesJson.
 */
fun parseTexturesHeader(
    raw: String
): Pair<List<ScTextureInfo>, String> {

    if (!raw.startsWith("TEXTURES:"))
        return emptyList<ScTextureInfo>() to raw

    val newlineIdx =
        raw.indexOf('\n')

    if (newlineIdx < 0)
        return emptyList<ScTextureInfo>() to raw

    val headerLine =
        raw.substring(
            "TEXTURES:".length,
            newlineIdx
        )

    val rest =
        raw.substring(newlineIdx + 1)

    val textures =
        headerLine
            .split(",")
            .mapNotNull { entry ->

                val parts =
                    entry.split("=")

                if (parts.size != 2)
                    return@mapNotNull null

                val idx =
                    parts[0]
                        .toIntOrNull()
                        ?: return@mapNotNull null

                val dims =
                    parts[1].split("x")

                if (dims.size != 2)
                    return@mapNotNull null

                val w =
                    dims[0]
                        .toIntOrNull()
                        ?: return@mapNotNull null

                val h =
                    dims[1]
                        .toIntOrNull()
                        ?: return@mapNotNull null

                ScTextureInfo(idx, w, h)
            }

    return textures to rest
}

fun loadScAtlas(
    ctx: android.content.Context,
    scFile: File
): ScAtlas? {

    return try {

        val outDir =
            ctx.cacheDir

        val rawResponse =
            ScProcessor.nativeGetScInfo(
                scFile.absolutePath,
                outDir.absolutePath
            )

        val (textures, json) =
            parseTexturesHeader(rawResponse)

        val rawFile =
            File(
                outDir,
                scFile.nameWithoutExtension +
                        "_atlas.raw"
            )

        val imgBmp =
            readRawAtlasBitmap(rawFile)
                ?: return null

        val sprites =
            parseSpritesJson(
                json,
                imgBmp.width,
                imgBmp.height
            )

        ScAtlas(
            bitmap = imgBmp,
            width = imgBmp.width,
            height = imgBmp.height,
            sprites = sprites,
            textures = textures,
            currentTextureIndex =
                if (textures.isNotEmpty())
                    textures.first().index
                else
                    0
        )

    } catch (_: Exception) {

        null
    }
}

/*
 * Переключает атлас на другую текстуру того же
 * SC2-файла — вызывается из multi-texture selector.
 *
 * В отличие от loadScAtlas, не читает SC-файл заново
 * целиком: nativeGetScTexture декодирует ТОЛЬКО
 * запрошенную текстуру, остальные не трогает — это
 * и есть экономия памяти на файлах с множеством
 * крупных текстур.
 */
fun loadScTextureByIndex(
    ctx: android.content.Context,
    scFile: File,
    textureIndex: Int
): ScAtlas? {

    return try {

        val outDir =
            ctx.cacheDir

        val rawResponse =
            ScProcessor.nativeGetScTexture(
                scFile.absolutePath,
                outDir.absolutePath,
                textureIndex
            )

        if (rawResponse.startsWith("Error"))
            return null

        val (textures, json) =
            parseTexturesHeader(rawResponse)

        val rawFile =
            File(
                outDir,
                scFile.nameWithoutExtension +
                        "_atlas.raw"
            )

        val imgBmp =
            readRawAtlasBitmap(rawFile)
                ?: return null

        val sprites =
            parseSpritesJson(
                json,
                imgBmp.width,
                imgBmp.height
            )

        ScAtlas(
            bitmap = imgBmp,
            width = imgBmp.width,
            height = imgBmp.height,
            sprites = sprites,
            textures = textures,
            currentTextureIndex = textureIndex
        )

    } catch (_: Exception) {

        null
    }
}

// ── Парсер списка спрайтов ────────────────────────────────────────────────────

fun parseSpritesJson(
    json: String,
    atlasW: Int,
    atlasH: Int
): List<ScSprite> {

    val sprites =
        mutableListOf<ScSprite>()

    /*
     * Формат:
     * id|name|u0|v0|u1|v1|w|h
     *
     * 9-е поле (опционально) — geometry:
     * textureIndex:x,y;x,y;...~textureIndex:x,y;...
     *
     * Несколько полигонов разделены '~'. Строки без
     * 9-го поля (старый формат, или спрайт без
     * вершин) остаются с пустым polygons — тогда UI
     * падает обратно на прямоугольный bbox, как раньше.
     */
    json.lines().forEach { line ->

        if (line.isBlank())
            return@forEach

        if (line.startsWith("TEXTURES:"))
            return@forEach

        val p =
            line.split("|")

        if (p.size >= 8) {

            try {

                val polygons =
                    if (p.size >= 9)
                        parseSpriteGeometry(p[8])
                    else
                        emptyList()

                sprites.add(
                    ScSprite(

                        id =
                            p[0].toInt(),

                        name =
                            p[1],

                        u0 =
                            p[2].toFloat(),

                        v0 =
                            p[3].toFloat(),

                        u1 =
                            p[4].toFloat(),

                        v1 =
                            p[5].toFloat(),

                        polygons =
                            polygons
                    )
                )

            } catch (_: Exception) {
                // Пропускаем битую строку.
            }
        }
    }

    return sprites
}

/*
 * Парсит geometry-поле:
 * textureIndex:x,y;x,y;...~textureIndex:x,y;...
 *
 * Битые/нечитаемые полигоны просто пропускаются —
 * не должны ронять парсинг всего спрайта, у него
 * останется хотя бы прямоугольный bbox.
 */
fun parseSpriteGeometry(
    field: String
): List<ScPolygon> {

    if (field.isBlank())
        return emptyList()

    return field
        .split("~")
        .mapNotNull { polyStr ->

            try {

                val colonIdx =
                    polyStr.indexOf(':')

                if (colonIdx < 0)
                    return@mapNotNull null

                val textureIndex =
                    polyStr
                        .substring(0, colonIdx)
                        .toInt()

                val pointsStr =
                    polyStr.substring(
                        colonIdx + 1
                    )

                val points =
                    pointsStr
                        .split(";")
                        .mapNotNull { pt ->

                            val xy =
                                pt.split(",")

                            if (xy.size != 2)
                                return@mapNotNull null

                            val x =
                                xy[0]
                                    .toFloatOrNull()
                                    ?: return@mapNotNull null

                            val y =
                                xy[1]
                                    .toFloatOrNull()
                                    ?: return@mapNotNull null

                            Offset(x, y)
                        }

                if (points.size < 3)
                    return@mapNotNull null

                ScPolygon(
                    textureIndex =
                        textureIndex,

                    points =
                        sortPointsByAngle(points)
                )

            } catch (_: Exception) {
                null
            }
        }
}

// ── Пакетная замена: модель и экраны ─────────────────────────────────────────

data class BatchEntry(
    val uri: android.net.Uri,
    val name: String,
    val sprite: ScSprite?
)

private fun loadBatchThumb(
    ctx: android.content.Context,
    uri: android.net.Uri,
    target: Int = 128
): ImageBitmap? = try {
    val bounds = android.graphics.BitmapFactory.Options().apply { inJustDecodeBounds = true }
    ctx.contentResolver.openInputStream(uri)?.use {
        android.graphics.BitmapFactory.decodeStream(it, null, bounds)
    }
    var sample = 1
    while (bounds.outWidth / (sample * 2) >= target && bounds.outHeight / (sample * 2) >= target)
        sample *= 2
    val opts = android.graphics.BitmapFactory.Options().apply { inSampleSize = sample }
    ctx.contentResolver.openInputStream(uri)?.use {
        android.graphics.BitmapFactory.decodeStream(it, null, opts)
    }?.asImageBitmap()
} catch (_: Throwable) {
    null
}

@Composable
private fun BatchReplaceDialog(
    isRu: Boolean,
    sprites: List<ScSprite>,
    entries: List<BatchEntry>,
    onAssign: (Int, ScSprite?) -> Unit,
    onRemove: (Int) -> Unit,
    onApply: () -> Unit,
    onCancel: () -> Unit
) {
    var pickFor by remember { mutableStateOf<Int?>(null) }

    val ready = entries.mapNotNull { it.sprite?.id }.distinct().size
    val dupIds = entries.mapNotNull { it.sprite?.id }
        .groupingBy { it }.eachCount().filter { it.value > 1 }.keys

    Dialog(
        onDismissRequest = onCancel,
        properties = DialogProperties(usePlatformDefaultWidth = false)
    ) {
        Surface(Modifier.fillMaxSize(), color = MaterialTheme.colorScheme.surface) {
            Column(Modifier.fillMaxSize().statusBarsPadding().navigationBarsPadding()) {

                Row(
                    Modifier.fillMaxWidth().padding(horizontal = 8.dp, vertical = 8.dp),
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    IconButton(onClick = onCancel) { Icon(Icons.Rounded.Close, null) }
                    Column(Modifier.weight(1f)) {
                        Text(
                            tr("editor.batch_replace_2"),
                            style = MaterialTheme.typography.titleLarge,
                            fontWeight = FontWeight.Bold
                        )
                        Text(
                            tr("editor.ready_0_of_1_tap_a_row_to_choose_a", ready, entries.size),
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant
                        )
                    }
                }

                LazyColumn(
                    Modifier.weight(1f),
                    contentPadding = PaddingValues(horizontal = 16.dp, vertical = 8.dp),
                    verticalArrangement = Arrangement.spacedBy(8.dp)
                ) {
                    itemsIndexed(entries) { i, e ->

                        val ctx = LocalContext.current
                        val thumb by produceState<ImageBitmap?>(null, e.uri) {
                            value = withContext(Dispatchers.IO) { loadBatchThumb(ctx, e.uri) }
                        }
                        val sp = e.sprite
                        val dup = sp != null && sp.id in dupIds
                        val okRow = sp != null && !dup
                        val spName: String = sp?.displayName ?: ""
                        val rowLabel: String =
                            if (sp == null) {
                                tr("editor.choose_a_sprite")
                            } else if (dup) {
                                (tr("editor.duplicate")) + spName
                            } else {
                                "→ " + spName
                            }
                        val rowColor =
                            if (okRow) MaterialTheme.colorScheme.primary
                            else MaterialTheme.colorScheme.error

                        Surface(
                            shape = RoundedCornerShape(18.dp),
                            color = MaterialTheme.colorScheme.surfaceContainerLow
                        ) {
                            Row(
                                Modifier
                                    .fillMaxWidth()
                                    .clickable { pickFor = i }
                                    .padding(10.dp),
                                verticalAlignment = Alignment.CenterVertically
                            ) {
                                Box(
                                    Modifier
                                        .size(52.dp)
                                        .clip(RoundedCornerShape(12.dp))
                                        .background(MaterialTheme.colorScheme.surfaceVariant),
                                    contentAlignment = Alignment.Center
                                ) {
                                    thumb?.let {
                                        Image(
                                            it, null,
                                            Modifier.fillMaxSize(),
                                            contentScale = ContentScale.Fit
                                        )
                                    }
                                }

                                Spacer(Modifier.width(12.dp))

                                Column(Modifier.weight(1f)) {
                                    Text(
                                        e.name,
                                        style = MaterialTheme.typography.bodyMedium,
                                        maxLines = 1,
                                        overflow = TextOverflow.Ellipsis
                                    )
                                    Spacer(Modifier.height(2.dp))
                                    Row(verticalAlignment = Alignment.CenterVertically) {
                                        Icon(
                                            imageVector =
                                                if (okRow) Icons.Rounded.CheckCircle
                                                else Icons.Rounded.Warning,
                                            contentDescription = null,
                                            modifier = Modifier.size(14.dp),
                                            tint = rowColor
                                        )
                                        Spacer(Modifier.width(4.dp))
                                        Text(
                                            text = rowLabel,
                                            style = MaterialTheme.typography.labelMedium,
                                            color = rowColor,
                                            maxLines = 1,
                                            overflow = TextOverflow.Ellipsis
                                        )
                                    }
                                }

                                IconButton(onClick = { onRemove(i) }) {
                                    Icon(
                                        Icons.Rounded.Close,
                                        tr("editor.remove"),
                                        tint = MaterialTheme.colorScheme.onSurfaceVariant
                                    )
                                }
                            }
                        }
                    }
                }

                Row(
                    Modifier.fillMaxWidth().padding(16.dp),
                    horizontalArrangement = Arrangement.End,
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    TextButton(onClick = onCancel) {
                        Text(tr("editor.cancel"))
                    }
                    Spacer(Modifier.width(8.dp))
                    Button(onClick = onApply, enabled = ready > 0) {
                        Icon(Icons.Rounded.PhotoLibrary, null, Modifier.size(18.dp))
                        Spacer(Modifier.width(6.dp))
                        Text(
                            tr("editor.replace_0", ready)
                        )
                    }
                }
            }
        }
    }

    pickFor?.let { idx ->
        val taken = entries.withIndex()
            .filter { it.index != idx }
            .mapNotNull { it.value.sprite?.id }
            .toSet()

        BatchSpritePicker(
            isRu = isRu,
            sprites = sprites,
            currentId = entries.getOrNull(idx)?.sprite?.id,
            takenIds = taken,
            onPick = { onAssign(idx, it); pickFor = null },
            onDismiss = { pickFor = null }
        )
    }
}

@Composable
private fun BatchSpritePicker(
    isRu: Boolean,
    sprites: List<ScSprite>,
    currentId: Int?,
    takenIds: Set<Int>,
    onPick: (ScSprite?) -> Unit,
    onDismiss: () -> Unit
) {
    var q by remember { mutableStateOf("") }
    val filtered = remember(q, sprites) {
        if (q.isBlank()) sprites
        else sprites.filter { it.displayName.contains(q.trim(), ignoreCase = true) }
    }

    Dialog(onDismissRequest = onDismiss) {
        Surface(
            shape = RoundedCornerShape(28.dp),
            color = MaterialTheme.colorScheme.surfaceContainerHigh
        ) {
            Column(Modifier.padding(16.dp).heightIn(max = 520.dp)) {

                Text(
                    tr("editor.target_sprite"),
                    style = MaterialTheme.typography.titleMedium,
                    fontWeight = FontWeight.Bold
                )

                OutlinedTextField(
                    value = q,
                    onValueChange = { q = it },
                    singleLine = true,
                    leadingIcon = { Icon(Icons.Rounded.Search, null) },
                    placeholder = { Text(tr("editor.search_by_name")) },
                    modifier = Modifier.fillMaxWidth().padding(vertical = 8.dp)
                )

                LazyColumn(Modifier.weight(1f, fill = false)) {

                    item {
                        TextButton(onClick = { onPick(null) }) {
                            Text(tr("editor.skip"))
                        }
                    }

                    items(filtered, key = { it.id }) { sp ->
                        Row(
                            Modifier
                                .fillMaxWidth()
                                .clickable { onPick(sp) }
                                .padding(horizontal = 8.dp, vertical = 10.dp),
                            verticalAlignment = Alignment.CenterVertically
                        ) {
                            Column(Modifier.weight(1f)) {
                                Text(
                                    sp.displayName,
                                    style = MaterialTheme.typography.bodyMedium,
                                    maxLines = 1,
                                    overflow = TextOverflow.Ellipsis
                                )
                                Text(
                                    buildString {
                                        append("${sp.w.toInt()}×${sp.h.toInt()}")
                                        if (sp.id in takenIds)
                                            append(tr("editor.already_used"))
                                    },
                                    style = MaterialTheme.typography.labelSmall,
                                    color =
                                        if (sp.id in takenIds) MaterialTheme.colorScheme.error
                                        else MaterialTheme.colorScheme.onSurfaceVariant
                                )
                            }
                            if (sp.id == currentId)
                                Icon(
                                    Icons.Rounded.CheckCircle, null,
                                    tint = MaterialTheme.colorScheme.primary
                                )
                        }
                    }
                }
            }
        }
    }
}


// ── Любая картинка → PNG в cache (WebP/JPG/BMP/GIF/HEIC) ─────────────────────

/*
 * Нативный код читает PNG/JPG, но не WebP и не всё остальное.
 * Поэтому всё, что не PNG, декодируется средствами Android и
 * сохраняется как PNG; EXIF-поворот у фото применяется.
 * null — файл не удалось прочитать.
 */
private fun copyImageAsPng(
    ctx: android.content.Context,
    uri: android.net.Uri,
    name: String
): File? {

    return try {

        val head = ByteArray(8)
        val n =
            ctx.contentResolver.openInputStream(uri)?.use { it.read(head) }
                ?: return null

        val isPng =
            n == 8 &&
                    head[0] == 0x89.toByte() &&
                    head[1] == 0x50.toByte() &&
                    head[2] == 0x4E.toByte() &&
                    head[3] == 0x47.toByte()

        val out = File(ctx.cacheDir, name.substringBeforeLast('.') + ".png")

        if (isPng) {
            ctx.contentResolver.openInputStream(uri)?.use { input ->
                out.outputStream().use { o -> input.copyTo(o) }
            } ?: return null
            return out
        }

        val opts = android.graphics.BitmapFactory.Options()
        opts.inPreferredConfig = Bitmap.Config.ARGB_8888

        var bmp =
            ctx.contentResolver.openInputStream(uri)?.use {
                android.graphics.BitmapFactory.decodeStream(it, null, opts)
            } ?: return null

        val orientation =
            try {
                ctx.contentResolver.openInputStream(uri)?.use {
                    android.media.ExifInterface(it).getAttributeInt(
                        android.media.ExifInterface.TAG_ORIENTATION,
                        android.media.ExifInterface.ORIENTATION_NORMAL
                    )
                } ?: android.media.ExifInterface.ORIENTATION_NORMAL
            } catch (_: Throwable) {
                android.media.ExifInterface.ORIENTATION_NORMAL
            }

        val matrix = android.graphics.Matrix()
        when (orientation) {
            android.media.ExifInterface.ORIENTATION_ROTATE_90 -> matrix.postRotate(90f)
            android.media.ExifInterface.ORIENTATION_ROTATE_180 -> matrix.postRotate(180f)
            android.media.ExifInterface.ORIENTATION_ROTATE_270 -> matrix.postRotate(270f)
            android.media.ExifInterface.ORIENTATION_FLIP_HORIZONTAL -> matrix.postScale(-1f, 1f)
            android.media.ExifInterface.ORIENTATION_FLIP_VERTICAL -> matrix.postScale(1f, -1f)
            else -> {}
        }
        if (!matrix.isIdentity) {
            val rotated =
                Bitmap.createBitmap(bmp, 0, 0, bmp.width, bmp.height, matrix, true)
            if (rotated !== bmp) {
                bmp.recycle()
                bmp = rotated
            }
        }

        out.outputStream().use { bmp.compress(Bitmap.CompressFormat.PNG, 100, it) }
        bmp.recycle()
        out

    } catch (_: Throwable) {
        null
    }
}

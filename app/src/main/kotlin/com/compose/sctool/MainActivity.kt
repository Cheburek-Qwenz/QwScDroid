package com.compose.sctool

import android.app.Activity
import android.content.Context
import android.content.Intent
import android.graphics.BitmapFactory
import android.net.Uri
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.animation.*
import androidx.compose.foundation.*
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.rounded.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.window.Dialog
import androidx.core.content.FileProvider
import kotlinx.coroutines.*
import java.io.File
import com.compose.sctool.editor.ScEditorScreen

class MainActivity : ComponentActivity() {
    override fun attachBaseContext(base: Context) =
        super.attachBaseContext(LangHelper.apply(base).also { I18n.init(it) })

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContent { SCToolTheme { SCToolApp() } }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun SCToolApp() {
    val ctx   = LocalContext.current
    val prefs = ctx.getSharedPreferences("settings", 0)
    val isRu  = LangHelper.getSaved(ctx) == "ru"

    var showLangPicker by remember { mutableStateOf(LangHelper.isFirst(ctx)) }
    var showWelcome    by remember { mutableStateOf(prefs.getBoolean("show_welcome", true)) }
    var selectedTool   by remember { mutableStateOf<ToolDef?>(null) }
    var showEditor     by remember { mutableStateOf(false) }
    val drawerState    = rememberDrawerState(DrawerValue.Closed)
    val scope          = rememberCoroutineScope()
    var updateToken    by remember { mutableStateOf(0) }
    var legacyEditor   by remember { mutableStateOf(prefs.getBoolean("legacy_editor", false)) }
    var showSettings   by remember { mutableStateOf(false) }
    var autoUpdate     by remember {
        mutableStateOf(ctx.getSharedPreferences("upd", Context.MODE_PRIVATE).getBoolean("auto", true))
    }

    var saveDir by remember { mutableStateOf(FileHelper.getOutDir(ctx)) }
    val dirLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.OpenDocumentTree()
    ) { uri ->
        if (uri != null) {
            ctx.contentResolver.takePersistableUriPermission(
                uri, Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_GRANT_WRITE_URI_PERMISSION)
            prefs.edit().putString("save_dir_uri", uri.toString()).apply()
            val path = uri.path?.replace("/tree/primary:", "/storage/emulated/0/")
                ?.replace("/tree/", "/storage/") ?: FileHelper.getOutDir(ctx).absolutePath
            saveDir = File(path).also { it.mkdirs() }
        }
    }

    if (showLangPicker) {
        LangPickerDialog { lang ->
            LangHelper.save(ctx, lang)
            showLangPicker = false
            ctx.startActivity(Intent(ctx, MainActivity::class.java))
            (ctx as? Activity)?.finish()
        }
        return
    }

    ModalNavigationDrawer(
        drawerState = drawerState,
        drawerContent = {
            ModalDrawerSheet(modifier = Modifier.width(300.dp)) {
                Box(Modifier.fillMaxWidth()
                    .background(MaterialTheme.colorScheme.secondaryContainer)
                    .padding(24.dp).padding(top = 16.dp)) {
                    Column {
                        Text("QwScDroid",
                            style = MaterialTheme.typography.headlineSmall,
                            fontWeight = FontWeight.Bold,
                            color = MaterialTheme.colorScheme.onSecondaryContainer)
                        Text("v${Updater.currentVersion(ctx)}",
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSecondaryContainer.copy(alpha = 0.7f))
                        Spacer(Modifier.height(8.dp))
                    }
                }
                Spacer(Modifier.height(8.dp))

                Column(Modifier.verticalScroll(rememberScrollState()).weight(1f)) {
                    (ALL_SECTIONS + GIF_SECTIONS).forEach { section ->
                        Text(section.name,
                            style = MaterialTheme.typography.labelMedium,
                            color = MaterialTheme.colorScheme.primary,
                            modifier = Modifier.padding(start = 28.dp, top = 16.dp, bottom = 4.dp))
                        section.tools.forEach { tool ->
                            NavigationDrawerItem(
                                icon     = { Icon(tool.icon, null) },
                                label    = { Text(tool.name) },
                                selected = selectedTool?.id == tool.id,
                                onClick  = {
                                    selectedTool = tool
                                    showEditor = false
                                    scope.launch(block = { drawerState.close() })
                                },
                                modifier = Modifier.padding(horizontal = 12.dp)
                            )
                        }
                        HorizontalDivider(Modifier.padding(horizontal = 16.dp, vertical = 4.dp),
                            color = MaterialTheme.colorScheme.outlineVariant.copy(alpha = 0.4f))
                    }

                    HorizontalDivider(Modifier.padding(horizontal = 16.dp, vertical = 4.dp),
                        color = MaterialTheme.colorScheme.outlineVariant.copy(alpha = 0.4f))
                    NavigationDrawerItem(
                        icon     = { Icon(Icons.Rounded.Edit, null) },
                        label    = { Text(tr("main.sc_editor")) },
                        selected = showEditor,
                        onClick  = {
                            showEditor = true
                            selectedTool = null
                            scope.launch(block = { drawerState.close() })
                        },
                        modifier = Modifier.padding(horizontal = 12.dp)
                    )
                    NavigationDrawerItem(
                        icon     = { Icon(Icons.Rounded.FolderOpen, null) },
                        label    = { Text(tr("main.save_folder")) },
                        selected = false,
                        onClick  = { dirLauncher.launch(null) },
                        modifier = Modifier.padding(horizontal = 12.dp)
                    )

                    NavigationDrawerItem(
                        icon     = { Icon(Icons.Rounded.Settings, null) },
                        label    = { Text(tr("main.settings")) },
                        selected = false,
                        onClick  = {
                            showSettings = true
                            scope.launch(block = { drawerState.close() })
                        },
                        modifier = Modifier.padding(horizontal = 12.dp)
                    )
                    NavigationDrawerItem(
                        icon     = { Icon(Icons.Rounded.Download, null) },
                        label    = { Text(tr("main.check_for_updates")) },
                        selected = false,
                        onClick  = {
                            updateToken++
                            scope.launch(block = { drawerState.close() })
                        },
                        modifier = Modifier.padding(horizontal = 12.dp)
                    )
                }
            }
        }
    ) {
        Scaffold(
            topBar = {
                if (!(showEditor && !legacyEditor)) TopAppBar(
                    title = {
                        val titleText = if (showEditor) {
                            tr("main.sc_editor")
                        } else if (selectedTool != null) {
                            selectedTool!!.name
                        } else {
                            "QwScDroid"
                        }
                        Text(titleText)
                    },
                    navigationIcon = {
                        IconButton(onClick = { scope.launch(block = { drawerState.open() }) }) {
                            Icon(Icons.Rounded.Menu, null)
                        }
                    },
                    colors = TopAppBarDefaults.topAppBarColors(
                        containerColor = MaterialTheme.colorScheme.surface,
                        titleContentColor = MaterialTheme.colorScheme.onSurface,
                        navigationIconContentColor = MaterialTheme.colorScheme.onSurface
                    )
                )
            }
        ) { padding ->
            UpdateChecker(isRu, updateToken)
            if (showSettings) {
                SettingsDialog(
                    isRu = isRu,
                    legacyEditor = legacyEditor,
                    onLegacyEditorChange = {
                        legacyEditor = it
                        prefs.edit().putBoolean("legacy_editor", it).apply()
                    },
                    autoUpdate = autoUpdate,
                    onAutoUpdateChange = {
                        autoUpdate = it
                        ctx.getSharedPreferences("upd", Context.MODE_PRIVATE)
                            .edit().putBoolean("auto", it).apply()
                    },
                    onCheckNow = { showSettings = false; updateToken++ },
                    onLanguage = { showSettings = false; showLangPicker = true },
                    onDismiss = { showSettings = false }
                )
            }
            Box(Modifier.padding(padding).fillMaxSize()) {
                if (showEditor) {
                    ScEditorScreen(
                        isRu   = isRu,
                        legacy = legacyEditor,
                        onMenu = { scope.launch(block = { drawerState.open() }) }
                    )
                } else {
                    AnimatedContent(
                        targetState = selectedTool,
                        transitionSpec = { fadeIn() + slideInHorizontally { it / 4 } togetherWith
                            fadeOut() + slideOutHorizontally { -it / 4 } }
                    ) { tool ->
                        if (tool == null) {
                            GreetingScreen(isRu, saveDir)
                        } else {
                            ToolScreen(tool = tool, isRu = isRu, saveDir = saveDir)
                        }
                    }
                }
            }
        }
    }
}

@Composable
fun ToolScreen(tool: ToolDef, isRu: Boolean, saveDir: File) {
    val ctx   = LocalContext.current
    val scope = rememberCoroutineScope()

    val fileInputs   = remember(tool.id) {
        tool.inputs.filter { it.type == InputType.FILE }
            .associate { it.id to mutableStateOf<Uri?>(null) }.toMutableMap()
    }
    val fileNames    = remember(tool.id) {
        tool.inputs.filter { it.type == InputType.FILE }
            .associate { it.id to mutableStateOf<String?>(null) }.toMutableMap()
    }
    val numberInputs = remember(tool.id) {
        tool.inputs.filter { it.type == InputType.NUMBER }
            .associate { it.id to mutableStateOf("") }.toMutableMap()
    }
    val checkInputs  = remember(tool.id) {
        tool.inputs.filter { it.type == InputType.CHECKBOX }
            .associate { it.id to mutableStateOf(false) }.toMutableMap()
    }
    val textInputs   = remember(tool.id) {
        tool.inputs.filter { it.type == InputType.TEXT }
            .associate { it.id to mutableStateOf("") }.toMutableMap()
    }
    val selectInputs = remember(tool.id) {
        tool.inputs.filter { it.type == InputType.SELECT }
            .associate { it.id to mutableStateOf(it.options.firstOrNull()?.value ?: "") }.toMutableMap()
    }
    val multiFiles   = remember(tool.id) { mutableStateListOf<Uri>() }
    val multiNames   = remember(tool.id) { mutableStateListOf<String>() }

    var pendingFileId by remember { mutableStateOf("") }
    var output        by remember(tool.id) { mutableStateOf("") }
    var running       by remember(tool.id) { mutableStateOf(false) }
    var previewBmp    by remember(tool.id) { mutableStateOf<ImageBitmap?>(null) }
    var savedFile     by remember(tool.id) { mutableStateOf<File?>(null) }

    val launcher = rememberLauncherForActivityResult(ActivityResultContracts.GetContent()) { uri ->
        if (uri != null && pendingFileId.isNotEmpty()) {
            fileInputs[pendingFileId]?.value = uri
            fileNames[pendingFileId]?.value  = FileHelper.getName(ctx, uri)
        }
    }

    val multiLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.GetMultipleContents()
    ) { uris ->
        multiFiles.clear(); multiNames.clear()
        uris.forEach { uri ->
            multiFiles.add(uri)
            multiNames.add(FileHelper.getName(ctx, uri))
        }
    }

    Column(
        Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp)
    ) {
        Text(tool.name,
            style = MaterialTheme.typography.titleLarge, fontWeight = FontWeight.SemiBold)

        tool.inputs.forEach { inp ->
            when (inp.type) {
                InputType.FILE -> FilePickerCard(
                    label       = inp.label,
                    fileName    = fileNames[inp.id]?.value,
                    browseLabel = tr("main.browse"),
                    onClick     = { pendingFileId = inp.id; launcher.launch("*/*") }
                )
                InputType.NUMBER -> {
                    val st = numberInputs[inp.id]!!
                    OutlinedTextField(
                        value = st.value, onValueChange = { st.value = it },
                        label = { Text(inp.label) },
                        placeholder = { Text(inp.hint) },
                        modifier = Modifier.fillMaxWidth(),
                        shape = RoundedCornerShape(12.dp)
                    )
                }
                InputType.CHECKBOX -> {
                    val st = checkInputs[inp.id]!!
                    Row(verticalAlignment = Alignment.CenterVertically,
                        modifier = Modifier.clickable { st.value = !st.value }) {
                        Checkbox(checked = st.value, onCheckedChange = { st.value = it })
                        Spacer(Modifier.width(8.dp))
                        Text(inp.label,
                            style = MaterialTheme.typography.bodyMedium)
                    }
                }
                InputType.TEXT -> {
                    val st = textInputs[inp.id]!!
                    OutlinedTextField(
                        value = st.value, onValueChange = { st.value = it },
                        label = { Text(inp.label) },
                        placeholder = { Text(inp.hint, style = MaterialTheme.typography.bodySmall) },
                        modifier = Modifier.fillMaxWidth(),
                        shape = RoundedCornerShape(12.dp)
                    )
                }
                InputType.SELECT -> {
                    val st = selectInputs[inp.id]!!
                    var expanded by remember { mutableStateOf(false) }
                    val selLabel = inp.options.find { it.value == st.value }?.label ?: st.value
                    Box(Modifier.fillMaxWidth()) {
                        Surface(
                            Modifier.fillMaxWidth().clip(RoundedCornerShape(16.dp))
                                .clickable { expanded = true },
                            color = MaterialTheme.colorScheme.surfaceContainerHigh,
                            shape = RoundedCornerShape(16.dp)
                        ) {
                            Row(Modifier.padding(horizontal = 16.dp, vertical = 12.dp),
                                verticalAlignment = Alignment.CenterVertically) {
                                Column(Modifier.weight(1f)) {
                                    Text(inp.label,
                                        style = MaterialTheme.typography.labelMedium,
                                        color = MaterialTheme.colorScheme.onSurfaceVariant)
                                    Text(selLabel, style = MaterialTheme.typography.bodyLarge)
                                }
                                Icon(Icons.Rounded.ArrowDropDown, null,
                                    tint = MaterialTheme.colorScheme.onSurfaceVariant)
                            }
                        }
                        DropdownMenu(expanded = expanded, onDismissRequest = { expanded = false }) {
                            inp.options.forEach { opt ->
                                DropdownMenuItem(
                                    text = { Text(opt.label) },
                                    onClick = { st.value = opt.value; expanded = false }
                                )
                            }
                        }
                    }
                }
                InputType.MULTI_FILE -> {
                    Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
                        Text(inp.label,
                            style = MaterialTheme.typography.labelMedium,
                            color = MaterialTheme.colorScheme.onSurfaceVariant)
                        Surface(Modifier.fillMaxWidth(), shape = RoundedCornerShape(12.dp),
                            color = MaterialTheme.colorScheme.surfaceContainerHighest) {
                            Column(Modifier.padding(12.dp)) {
                                if (multiNames.isEmpty()) {
                                    Text("...", color = MaterialTheme.colorScheme.onSurfaceVariant,
                                        fontFamily = FontFamily.Monospace)
                                } else {
                                    multiNames.forEach { n ->
                                        Text(n, style = MaterialTheme.typography.bodySmall,
                                            fontFamily = FontFamily.Monospace,
                                            color = MaterialTheme.colorScheme.onSurface)
                                    }
                                }
                                Spacer(Modifier.height(8.dp))
                                FilledTonalButton(
                                    onClick = { multiLauncher.launch("image/png") },
                                    shape = RoundedCornerShape(10.dp)
                                ) {
                                    Text(tr("main.select_png_files"))
                                }
                            }
                        }
                    }
                }
            }
        }

        // Папка сохранения
        Surface(shape = RoundedCornerShape(12.dp), color = MaterialTheme.colorScheme.surfaceContainerLow) {
            Row(Modifier.padding(horizontal = 12.dp, vertical = 8.dp),
                verticalAlignment = Alignment.CenterVertically) {
                Icon(Icons.Rounded.FolderOpen, null, Modifier.size(16.dp),
                    tint = MaterialTheme.colorScheme.onSurfaceVariant)
                Spacer(Modifier.width(6.dp))
                Text(saveDir.absolutePath,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    fontFamily = FontFamily.Monospace, maxLines = 2)
            }
        }

        Button(
            onClick = {
                val missing = tool.inputs.filter { it.type == InputType.FILE }
                    .firstOrNull { fileInputs[it.id]?.value == null }
                if (missing != null) {
                    output = tr("main.pick_file", missing.label)
                    return@Button
                }
                if (tool.inputs.any { it.type == InputType.MULTI_FILE } && multiFiles.isEmpty()) {
                    output = tr("main.select_png_files_2")
                    return@Button
                }
                running    = true
                previewBmp = null
                savedFile  = null
                output     = tr("main.running")

                scope.launch(Dispatchers.IO) {
                    val files = tool.inputs.filter { it.type == InputType.FILE }
                        .associate { inp ->
                            val uri  = fileInputs[inp.id]!!.value!!
                            val name = fileNames[inp.id]?.value ?: "file"
                            inp.id to FileHelper.copyToCache(ctx, uri, name)
                        }
                    val mFiles = multiFiles.mapIndexed { i, uri ->
                        FileHelper.copyToCache(ctx, uri, multiNames.getOrElse(i) { "file_$i" })
                    }
                    val nums    = numberInputs.mapValues { it.value.value }
                    val checks  = checkInputs.mapValues { it.value.value }
                    val texts   = textInputs.mapValues { it.value.value }
                    val selects = selectInputs.mapValues { it.value.value }

                    // Размер входных файлов
                    val inputSize = files.values.sumOf { it.length() } + mFiles.sumOf { it.length() }

                    val startTime = System.currentTimeMillis()
                    val result = runTool(tool, files, mFiles, nums, checks, texts, selects, saveDir)
                    val elapsed = System.currentTimeMillis() - startTime

                    // Размер выходных файлов
                    val outputSize = saveDir.listFiles()
                        ?.filter { it.lastModified() >= startTime }
                        ?.sumOf { it.length() } ?: 0L

                    fun formatSize(b: Long): String = when {
                        b >= 1024*1024 -> "%.1f MB".format(b/1024.0/1024.0)
                        b >= 1024      -> "%.1f KB".format(b/1024.0)
                        else           -> "$b B"
                    }
                    fun formatTime(ms: Long): String = when {
                        ms >= 1000 -> "%.1f s".format(ms/1000.0)
                        else       -> "${ms} ms"
                    }

                    val statsLine = buildString {
                        if (inputSize > 0) append("In: ${formatSize(inputSize)}")
                        if (outputSize > 0) append("  Out: ${formatSize(outputSize)}")
                        append("  Time: ${formatTime(elapsed)}")
                    }

                    val finalResult = if (result.isNotEmpty()) "$result\n\n$statsLine" else statsLine

                    var bmp: ImageBitmap? = null
                    var saved: File?      = null
                    if (tool.outputExt == "png" && result.startsWith("OK")) {
                        saveDir.listFiles()
                            ?.filter { it.extension == "png" }
                            ?.maxByOrNull { it.lastModified() }
                            ?.let { f ->
                                saved = f
                                try { bmp = BitmapFactory.decodeFile(f.absolutePath)?.asImageBitmap() }
                                catch (_: Exception) {}
                            }
                    }
                    withContext(Dispatchers.Main) {
                        output     = finalResult
                        running    = false
                        previewBmp = bmp
                        savedFile  = saved
                    }
                }
            },
            modifier = Modifier.fillMaxWidth().height(60.dp),
            shape = androidx.compose.foundation.shape.CircleShape,
            enabled = !running
        ) {
            Text(tr("main.run"), fontSize = 16.sp, fontWeight = FontWeight.SemiBold)
        }

        if (output.isNotEmpty()) {
            val isErr = !running && output.contains("Error")
            val resColor = if (isErr) MaterialTheme.colorScheme.errorContainer
                           else MaterialTheme.colorScheme.surfaceContainerHigh
            val resOn = if (isErr) MaterialTheme.colorScheme.onErrorContainer
                        else MaterialTheme.colorScheme.onSurface
            val resIcon = if (isErr) Icons.Rounded.ErrorOutline else Icons.Rounded.CheckCircle
            val resTitle = if (running) (tr("main.running_2"))
                           else if (isErr) (tr("main.error"))
                           else if (savedFile != null) (tr("main.file_saved"))
                           else (tr("main.result"))
            Surface(Modifier.fillMaxWidth(), shape = RoundedCornerShape(28.dp),
                color = resColor) {
                Column {
                    Row(Modifier.padding(start = 16.dp, end = 16.dp, top = 14.dp),
                        verticalAlignment = Alignment.CenterVertically) {
                        if (!running) {
                            Icon(resIcon, null, Modifier.size(20.dp),
                                tint = if (isErr) resOn else MaterialTheme.colorScheme.primary)
                            Spacer(Modifier.width(8.dp))
                        }
                        Text(resTitle, style = MaterialTheme.typography.titleSmall, color = resOn)
                    }
                    if (running) {
                        Box(Modifier.fillMaxWidth().padding(top = 12.dp), contentAlignment = Alignment.Center) {
                            MdLoadingIndicator(Modifier.size(88.dp), MaterialTheme.colorScheme.primary)
                        }
                    }
                    Text(output, Modifier.padding(16.dp),
                        fontFamily = FontFamily.Monospace,
                        style = MaterialTheme.typography.bodySmall,
                        color = resOn)
                    savedFile?.let { file ->
                        HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant.copy(alpha = 0.3f))
                        Row(Modifier.fillMaxWidth().padding(8.dp), horizontalArrangement = Arrangement.End) {
                            TextButton(onClick = {
                                try {
                                    val uri = FileProvider.getUriForFile(
                                        ctx, "${ctx.packageName}.fileprovider", file)
                                    val mime = ctx.contentResolver.getType(uri) ?: "*/*"
                                    val intent = Intent(Intent.ACTION_SEND).apply {
                                        type = mime
                                        putExtra(Intent.EXTRA_STREAM, uri)
                                        addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
                                    }
                                    ctx.startActivity(Intent.createChooser(intent,
                                        tr("main.share")))
                                } catch (e: Exception) {
                                    // fallback — открыть файл
                                    try {
                                        val uri = FileProvider.getUriForFile(
                                            ctx, "${ctx.packageName}.fileprovider", file)
                                        val intent = Intent(Intent.ACTION_VIEW).apply {
                                            setDataAndType(uri, "*/*")
                                            addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
                                        }
                                        ctx.startActivity(intent)
                                    } catch (_: Exception) {}
                                }
                            }) {
                                Icon(Icons.Rounded.Share, null, Modifier.size(16.dp))
                                Spacer(Modifier.width(4.dp))
                                Text(tr("main.share"))
                            }
                        }
                    }
                }
            }
        }

        previewBmp?.let { bmp ->
            Text(tr("main.preview"),
                style = MaterialTheme.typography.labelMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant)
            Surface(Modifier.fillMaxWidth().clip(RoundedCornerShape(24.dp)),
                color = MaterialTheme.colorScheme.surfaceContainerHighest) {
                Image(bmp, null, Modifier.fillMaxWidth().heightIn(max = 400.dp),
                    contentScale = ContentScale.Fit)
            }
        }
    }
}

@Composable
fun FilePickerCard(label: String, fileName: String?, browseLabel: String, onClick: () -> Unit) {
    val picked = fileName != null
    val cardColor = if (picked) MaterialTheme.colorScheme.secondaryContainer
                    else MaterialTheme.colorScheme.surfaceContainerHigh
    val textColor = if (picked) MaterialTheme.colorScheme.onSecondaryContainer
                    else MaterialTheme.colorScheme.onSurface
    val icon = if (picked) Icons.Rounded.InsertDriveFile else Icons.Rounded.UploadFile
    Surface(
        Modifier.fillMaxWidth().clip(RoundedCornerShape(20.dp)).clickable { onClick() },
        shape = RoundedCornerShape(20.dp),
        color = cardColor
    ) {
        Row(Modifier.padding(horizontal = 16.dp, vertical = 14.dp),
            verticalAlignment = Alignment.CenterVertically) {
            Icon(icon, null, Modifier.size(24.dp), tint = textColor)
            Spacer(Modifier.width(14.dp))
            Column(Modifier.weight(1f)) {
                Text(label, style = MaterialTheme.typography.labelMedium,
                    color = textColor.copy(alpha = 0.75f), maxLines = 2)
                Text(fileName ?: browseLabel, style = MaterialTheme.typography.bodyLarge,
                    color = textColor, maxLines = 1,
                    overflow = androidx.compose.ui.text.style.TextOverflow.Ellipsis)
            }
            Icon(Icons.Rounded.ChevronRight, null, tint = textColor.copy(alpha = 0.6f))
        }
    }
}

@Composable
fun LangPickerDialog(onSelect: (String) -> Unit) {
    val ctx = LocalContext.current
    val langs = remember { I18n.languages(ctx) }
    Dialog(onDismissRequest = {}) {
        Surface(shape = RoundedCornerShape(28.dp), color = MaterialTheme.colorScheme.surface,
            tonalElevation = 6.dp) {
            Column(Modifier.padding(24.dp).verticalScroll(rememberScrollState())) {
                Text("Выберите язык / Choose language",
                    style = MaterialTheme.typography.titleLarge, fontWeight = FontWeight.Bold)
                Spacer(Modifier.height(20.dp))
                langs.forEach { (code, label) ->
                    Surface(Modifier.fillMaxWidth().clip(RoundedCornerShape(12.dp))
                        .clickable { onSelect(code) },
                        color = MaterialTheme.colorScheme.surfaceContainerHigh,
                        shape = RoundedCornerShape(12.dp), tonalElevation = 2.dp) {
                        Text(label, Modifier.padding(16.dp), style = MaterialTheme.typography.bodyLarge)
                    }
                    Spacer(Modifier.height(8.dp))
                }
            }
        }
    }
}

/** Имя экспорта для gif2sc: латиница/цифры/подчёркивание, кириллица транслитерируется. */
fun exportNameFrom(raw: String): String {
    val map = mapOf(
        'а' to "a", 'б' to "b", 'в' to "v", 'г' to "g", 'д' to "d", 'е' to "e", 'ё' to "yo", 'ж' to "zh",
        'з' to "z", 'и' to "i", 'й' to "y", 'к' to "k", 'л' to "l", 'м' to "m", 'н' to "n", 'о' to "o",
        'п' to "p", 'р' to "r", 'с' to "s", 'т' to "t", 'у' to "u", 'ф' to "f", 'х' to "h", 'ц' to "ts",
        'ч' to "ch", 'ш' to "sh", 'щ' to "sch", 'ъ' to "", 'ы' to "y", 'ь' to "", 'э' to "e", 'ю' to "yu",
        'я' to "ya"
    )
    val sb = StringBuilder()
    for (ch in raw.trim()) {
        val t = map[ch.lowercaseChar()]
        if (t != null) sb.append(t) else sb.append(ch)
    }
    var name = sb.toString().replace(Regex("[^A-Za-z0-9_]+"), "_").trim('_').replace(Regex("_+"), "_")
    if (name.isEmpty()) return "bgr_anim"
    if (name[0].isDigit()) name = "skin_" + name
    return name
}

fun runTool(
    tool: ToolDef,
    files: Map<String, File>,
    multiFiles: List<File>,
    nums: Map<String, String>,
    checks: Map<String, Boolean>,
    texts: Map<String, String>,
    selects: Map<String, String>,
    outDir: File
): String {
    fun getFile(id: String) = files[id]?.absolutePath ?: ""
    fun getNum(id: String) = nums[id]?.toIntOrNull() ?: -1
    fun getText(id: String) = texts[id] ?: ""
    fun getSel(id: String) = selects[id] ?: "8x8"
    val out = outDir.absolutePath
    return try {
        when (tool.id) {
            "info"           -> ScProcessor.nativeScInfo(getFile("file"))
            "split"          -> ScProcessor.nativeSplitSc(getFile("file"), out)
            "join"           -> ScProcessor.nativeJoinSc(getFile("logic"), getFile("tex"), out)
            "extract"        -> ScProcessor.nativeExtractKtx(getFile("file"), getNum("idx"), out)
            "downgrade"      -> {
                val ver = getSel("version")
                val result = ScProcessor.nativeDowngrade(getFile("file"), ver, out)
                "SC Downgrade port by @Invoker4k\n\n$result"
            }
            "gif2sc"         -> {
                val fps = getNum("fps")
                val screen = if (getSel("screen") == "2") 2 else 1
                val result = ScProcessor.nativeGif2Sc(
                    getFile("file"), exportNameFrom(getText("name")),
                    if (fps > 0) fps else 0, screen,
                    getText("bg").trim().trimStart('#'),
                    if (getSel("tex") == "rgba") 1 else 0, out
                )
                "GIF to SC: tool by @Ghoul1337i\n\n$result"
            }
            "sc2gif"         -> {
                val size = getNum("size")
                val fps = getNum("fps")
                val result = ScProcessor.nativeSc2Gif(
                    getFile("file"), getText("name").trim(),
                    if (size > 0) size else 640,
                    (if (fps > 0) fps else 0) or (if (getSel("masks") == "ignore") 0x10000 else 0), out
                )
                "SC to GIF: tool by @Ghoul1337i\n\n$result"
            }
            "movieclips"     -> {
                val raw = ScProcessor.nativeGetMovieClips(getFile("file"))
                if (raw.startsWith("Error")) raw else {
                    val sb = StringBuilder()
                    var total = 0
                    var named = 0
                    for (line in raw.split('\n')) {
                        val parts = line.split('\t')
                        if (parts.size < 3) continue
                        total++
                        if (parts[1].isNotEmpty()) named++
                        sb.append(parts[0]).append("  ").append(if (parts[1].isEmpty()) "—" else parts[1])
                            .append("  (").append(parts[2]).append(")\n")
                    }
                    "MovieClips: $total, with export name: $named\nID  name  (frames)\n\n$sb"
                }
            }
            "ktx2png"        -> ScProcessor.nativeKtx2Png(getFile("file"), out)
            "png2ktx"        -> ScProcessor.nativePng2Ktx(getFile("file"), getSel("block"), out)
            "png2sc"         -> ScProcessor.nativePng2Sc(getFile("png"), getFile("orig"), getNum("idx"), "0", out)
            "atlas2sc"       -> {
                // старый режим: правишь атлас целиком, затем заменяешь текстуру (индекс -1 = по размеру PNG)
                val head = try { File(getFile("orig")).inputStream().use { ins -> ByteArray(3).also { ins.read(it) } } }
                           catch (e: Exception) { ByteArray(3) }
                if (head[2].toInt() >= 5)
                    "Error: " + tr("main.atlas_sc1_only")
                else ScProcessor.nativePng2Sc(getFile("png"), getFile("orig"), -1, "0", out)
            }
            "sctx2png"       -> ScProcessor.nativeSctx2Png(getFile("file"), out)
            "png2sctx"       -> ScProcessor.nativePng2Sctx(getFile("png"), out)
            "cut"            -> ScProcessor.nativeCutSprites(getFile("logic"), getFile("tex"), out)
            "sc2png"         -> ScProcessor.nativeSc2Png(getFile("file"), getNum("idx"), out)
            "exports"        -> ScProcessor.nativeExports(getFile("file"), out)
            "inject_preview" -> ScProcessor.nativeInjectPreview(getFile("file"), out)
            "inject_put"     -> ScProcessor.nativeInjectPut(getFile("file"), getFile("sprite"), getText("name"), out)
            "inject_list"    -> ScProcessor.nativeInjectList(getFile("file"))
            "anim2static"    -> ScProcessor.nativeAnim2Static(getFile("file"), out)
            "build_sc"       -> {
                val paths = multiFiles.joinToString("|") { it.absolutePath }
                ScProcessor.nativeBuildSc(paths, getText("names"), getSel("block"), out)
            }
            "mc_copy"        -> ScProcessor.nativeMcCopy(getFile("src"), getFile("dst"), getText("names"), out)
            else             -> "Unknown: ${tool.id}"
        }
    } catch (e: Exception) { "Error: ${e.message}" }
}

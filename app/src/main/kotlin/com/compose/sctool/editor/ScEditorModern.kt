package com.compose.sctool.editor

import com.compose.sctool.tr

import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.EnterTransition
import androidx.compose.animation.ExitTransition
import androidx.compose.animation.core.animateDpAsState
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.slideInVertically
import androidx.compose.animation.slideOutVertically
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.LazyRow
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.rounded.Undo
import androidx.compose.material.icons.rounded.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.input.pointer.pointerInput
import com.compose.sctool.MdLoadingIndicator
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.withContext

/*
 * Новый (Material 3) интерфейс редактора.
 *
 * Вся логика (загрузка, Replace, Undo, экспорт) остаётся в ScEditorScreen —
 * здесь только раскладка: верхняя панель, атлас на весь экран, плавающий
 * инспектор, список спрайтов (боковая панель / нижняя шторка), тост-уведомления.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
internal fun ModernEditorUi(
    isRu: Boolean,
    atlas: ScAtlas?,
    fileName: String?,
    versionLabel: String?,
    loading: Boolean,
    status: String,
    canUndo: Boolean,
    selected: ScSprite?,
    searchQuery: String,
    onMenu: (() -> Unit)?,
    onOpen: () -> Unit,
    onUndo: () -> Unit,
    onBatch: () -> Unit,
    onExports: () -> Unit,
    onExportAll: () -> Unit,
    onSelect: (ScSprite?) -> Unit,
    onSearchChange: (String) -> Unit,
    onReplace: () -> Unit,
    onExportSprite: (ScSprite) -> Unit,
    onPickTexture: (Int) -> Unit,
    onStatusDismiss: () -> Unit
) {
    val cs = MaterialTheme.colorScheme

    var searchOpen by remember { mutableStateOf(false) }
    var sheetOpen by remember { mutableStateOf(false) }
    var menuOpen by remember { mutableStateOf(false) }
    val zoom = remember { AtlasZoomState() }

    // последний выбранный спрайт остаётся на экране на время анимации скрытия инспектора
    var lastSel by remember { mutableStateOf<ScSprite?>(null) }
    LaunchedEffect(selected) { if (selected != null) lastSel = selected }

    BoxWithConstraints(
        Modifier
            .fillMaxSize()
            .background(cs.surface)
    ) {

        val wide = maxWidth >= 600.dp
        val sheetHeight by animateDpAsState(
            targetValue = if (sheetOpen) maxHeight * 0.6f else 56.dp,
            label = "sheet"
        )

        Column(Modifier.fillMaxSize()) {

            // ── Верхняя панель ───────────────────────────────────────────

            Surface(color = cs.surfaceContainerLow) {
                Row(
                    Modifier
                        .fillMaxWidth()
                        .padding(horizontal = 4.dp, vertical = 6.dp),
                    verticalAlignment = Alignment.CenterVertically
                ) {

                    if (onMenu != null) {
                        IconButton(onClick = onMenu) {
                            Icon(Icons.Rounded.Menu, null)
                        }
                    } else {
                        Spacer(Modifier.width(12.dp))
                    }

                    Column(Modifier.weight(1f)) {
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            Text(
                                text = fileName
                                    ?: (tr("modern.sc_editor")),
                                style = MaterialTheme.typography.titleMedium,
                                fontWeight = FontWeight.Medium,
                                maxLines = 1,
                                overflow = TextOverflow.Ellipsis,
                                modifier = Modifier.weight(1f, fill = false)
                            )
                            if (versionLabel != null) {
                                Spacer(Modifier.width(8.dp))
                                Surface(
                                    shape = RoundedCornerShape(50),
                                    color = cs.primaryContainer
                                ) {
                                    Text(
                                        versionLabel,
                                        Modifier.padding(horizontal = 8.dp, vertical = 2.dp),
                                        style = MaterialTheme.typography.labelSmall,
                                        fontFamily = FontFamily.Monospace,
                                        color = cs.onPrimaryContainer,
                                        maxLines = 1
                                    )
                                }
                            }
                        }
                        Text(
                            text =
                                if (atlas != null)
                                    (tr("modern.0_sprites", atlas.sprites.size)) +
                                            " • ${atlas.width}×${atlas.height} px"
                                else
                                    (tr("modern.no_file")),
                            style = MaterialTheme.typography.bodySmall,
                            color = cs.onSurfaceVariant,
                            maxLines = 1,
                            overflow = TextOverflow.Ellipsis
                        )
                    }

                    if (canUndo) {
                        IconButton(onClick = onUndo, enabled = !loading) {
                            Icon(
                                Icons.AutoMirrored.Rounded.Undo,
                                tr("modern.undo"),
                                tint = cs.primary
                            )
                        }
                    }

                    if (atlas != null) {
                        IconButton(onClick = {
                            searchOpen = !searchOpen
                            if (!searchOpen) onSearchChange("")
                        }) {
                            Icon(
                                Icons.Rounded.Search,
                                tr("modern.search"),
                                tint = if (searchOpen) cs.primary else cs.onSurfaceVariant
                            )
                        }

                        if (wide) {
                            FilledTonalButton(
                                onClick = onBatch,
                                enabled = !loading,
                                contentPadding = PaddingValues(horizontal = 12.dp),
                                modifier = Modifier.height(36.dp)
                            ) {
                                Icon(Icons.Rounded.PhotoLibrary, null, Modifier.size(16.dp))
                                Spacer(Modifier.width(6.dp))
                                Text(
                                    tr("modern.batch_replace"),
                                    style = MaterialTheme.typography.labelMedium
                                )
                            }
                            Spacer(Modifier.width(6.dp))
                            OutlinedButton(
                                onClick = onExports,
                                enabled = !loading,
                                contentPadding = PaddingValues(horizontal = 12.dp),
                                modifier = Modifier.height(36.dp)
                            ) {
                                Icon(Icons.Rounded.Edit, null, Modifier.size(16.dp))
                                Spacer(Modifier.width(6.dp))
                                Text(
                                    tr("modern.export_names"),
                                    style = MaterialTheme.typography.labelMedium
                                )
                            }
                            Spacer(Modifier.width(6.dp))
                            OutlinedButton(
                                onClick = onExportAll,
                                enabled = !loading,
                                contentPadding = PaddingValues(horizontal = 12.dp),
                                modifier = Modifier.height(36.dp)
                            ) {
                                Icon(Icons.Rounded.Download, null, Modifier.size(16.dp))
                                Spacer(Modifier.width(6.dp))
                                Text(
                                    tr("modern.export_zip"),
                                    style = MaterialTheme.typography.labelMedium
                                )
                            }
                            Spacer(Modifier.width(6.dp))
                        } else {
                            Box {
                                IconButton(onClick = { menuOpen = true }) {
                                    Icon(Icons.Rounded.MoreVert, null)
                                }
                                DropdownMenu(
                                    expanded = menuOpen,
                                    onDismissRequest = { menuOpen = false }
                                ) {
                                    DropdownMenuItem(
                                        text = { Text(tr("modern.batch_replace")) },
                                        leadingIcon = { Icon(Icons.Rounded.PhotoLibrary, null) },
                                        onClick = { menuOpen = false; onBatch() }
                                    )
                                    DropdownMenuItem(
                                        text = { Text(tr("modern.export_names")) },
                                        leadingIcon = { Icon(Icons.Rounded.Edit, null) },
                                        onClick = { menuOpen = false; onExports() }
                                    )
                                    DropdownMenuItem(
                                        text = { Text(tr("modern.export_zip")) },
                                        leadingIcon = { Icon(Icons.Rounded.Download, null) },
                                        onClick = { menuOpen = false; onExportAll() }
                                    )
                                }
                            }
                        }
                    }

                    if (wide) {
                        Button(
                            onClick = onOpen,
                            contentPadding = PaddingValues(horizontal = 14.dp),
                            modifier = Modifier.height(36.dp)
                        ) {
                            Icon(Icons.Rounded.FolderOpen, null, Modifier.size(16.dp))
                            Spacer(Modifier.width(6.dp))
                            Text(
                                tr("modern.open_sc"),
                                style = MaterialTheme.typography.labelMedium
                            )
                        }
                        Spacer(Modifier.width(8.dp))
                    } else {
                        IconButton(
                            onClick = onOpen,
                            colors = IconButtonDefaults.iconButtonColors(
                                containerColor = cs.primary,
                                contentColor = cs.onPrimary
                            )
                        ) {
                            Icon(
                                Icons.Rounded.FolderOpen,
                                tr("modern.open_sc")
                            )
                        }
                        Spacer(Modifier.width(4.dp))
                    }
                }
            }

            // Индикатор загрузки (место занято всегда — без «прыжков»)
            Box(Modifier.fillMaxWidth().height(3.dp)) {
            }

            // ── Поиск ────────────────────────────────────────────────────

            AnimatedVisibility(visible = searchOpen && atlas != null) {
                val focus = remember { FocusRequester() }
                LaunchedEffect(Unit) { focus.requestFocus() }

                Surface(color = cs.surfaceContainerHigh) {
                    Row(
                        Modifier
                            .fillMaxWidth()
                            .padding(horizontal = 16.dp, vertical = 8.dp),
                        verticalAlignment = Alignment.CenterVertically
                    ) {
                        Icon(Icons.Rounded.Search, null, tint = cs.onSurfaceVariant)
                        Spacer(Modifier.width(12.dp))
                        Box(Modifier.weight(1f)) {
                            if (searchQuery.isEmpty()) {
                                Text(
                                    tr("modern.search_sprite_by_name"),
                                    style = MaterialTheme.typography.bodyMedium,
                                    color = cs.onSurfaceVariant
                                )
                            }
                            BasicTextField(
                                value = searchQuery,
                                onValueChange = onSearchChange,
                                singleLine = true,
                                textStyle = MaterialTheme.typography.bodyMedium.copy(color = cs.onSurface),
                                cursorBrush = SolidColor(cs.primary),
                                modifier = Modifier.fillMaxWidth().focusRequester(focus)
                            )
                        }
                        IconButton(onClick = {
                            searchOpen = false
                            onSearchChange("")
                        }) {
                            Icon(Icons.Rounded.Close, null, tint = cs.onSurfaceVariant)
                        }
                    }
                }
            }

            // ── Выбор текстуры (multi-texture) ───────────────────────────

            if (atlas != null && atlas.textures.size > 1) {
                LazyRow(
                    Modifier
                        .fillMaxWidth()
                        .padding(vertical = 4.dp),
                    contentPadding = PaddingValues(horizontal = 12.dp),
                    horizontalArrangement = Arrangement.spacedBy(6.dp)
                ) {
                    items(atlas.textures, key = { it.index }) { tex ->
                        FilterChip(
                            selected = tex.index == atlas.currentTextureIndex,
                            onClick = { onPickTexture(tex.index) },
                            label = {
                                Text(
                                    "Texture ${tex.index} · ${tex.width}×${tex.height}",
                                    style = MaterialTheme.typography.labelSmall
                                )
                            }
                        )
                    }
                }
            }

            // ── Рабочая область ──────────────────────────────────────────

            Box(
                Modifier
                    .weight(1f)
                    .fillMaxWidth()
            ) {

                if (atlas == null) {

                    Column(
                        Modifier.align(Alignment.Center).padding(32.dp),
                        horizontalAlignment = Alignment.CenterHorizontally,
                        verticalArrangement = Arrangement.spacedBy(14.dp)
                    ) {
                        Surface(
                            shape = RoundedCornerShape(28.dp),
                            color = cs.primaryContainer,
                            modifier = Modifier.size(84.dp)
                        ) {
                            Box(contentAlignment = Alignment.Center) {
                                Icon(
                                    Icons.Rounded.Image, null,
                                    Modifier.size(40.dp),
                                    tint = cs.onPrimaryContainer
                                )
                            }
                        }
                        Text(
                            tr("modern.open_an_sc_file"),
                            style = MaterialTheme.typography.titleMedium,
                            fontWeight = FontWeight.Bold
                        )
                        Text(
                            tr("modern.atlas_sprites_replace_and_export"),
                            style = MaterialTheme.typography.bodySmall,
                            color = cs.onSurfaceVariant
                        )
                        Button(onClick = onOpen) {
                            Icon(Icons.Rounded.FolderOpen, null, Modifier.size(18.dp))
                            Spacer(Modifier.width(8.dp))
                            Text(tr("modern.open_sc"))
                        }
                    }

                } else {

                    val filtered = remember(atlas, searchQuery) {
                        if (searchQuery.isBlank()) atlas.sprites
                        else atlas.sprites.filter {
                            it.displayName.contains(searchQuery.trim(), ignoreCase = true)
                        }
                    }

                    Row(Modifier.fillMaxSize()) {

                        Box(
                            Modifier
                                .weight(1f)
                                .fillMaxHeight()
                        ) {

                            AtlasView(
                                atlas = atlas,
                                selected = selected,
                                modifier = Modifier.fillMaxSize(),
                                onSpriteClick = { onSelect(it) },
                                zoomState = zoom
                            )

                            // Зум
                            Surface(
                                modifier = Modifier
                                    .align(Alignment.TopStart)
                                    .padding(12.dp),
                                shape = RoundedCornerShape(20.dp),
                                color = cs.surfaceContainerHigh.copy(alpha = 0.92f),
                                tonalElevation = 3.dp
                            ) {
                                Column(Modifier.padding(2.dp)) {
                                    IconButton(onClick = { zoom.zoomIn() }) {
                                        Icon(Icons.Rounded.Add, tr("modern.zoom_in"))
                                    }
                                    IconButton(onClick = { zoom.zoomOut() }) {
                                        Icon(Icons.Rounded.Remove, tr("modern.zoom_out"))
                                    }
                                    IconButton(onClick = { zoom.reset() }) {
                                        Icon(
                                            Icons.Rounded.CenterFocusStrong,
                                            tr("modern.reset_zoom")
                                        )
                                    }
                                }
                            }

                            // Плавающий инспектор
                            PopVisibility(
                                visible = selected != null,
                                modifier = Modifier
                                    .align(Alignment.BottomCenter)
                                    .padding(
                                        start = 12.dp,
                                        end = 12.dp,
                                        bottom = if (wide) 12.dp else 68.dp
                                    ),
                                enter = fadeIn() + slideInVertically { it / 2 },
                                exit = fadeOut() + slideOutVertically { it / 2 }
                            ) {
                                val sp = selected ?: lastSel

                                if (sp != null) {
                                    Surface(
                                        shape = RoundedCornerShape(26.dp),
                                        color = cs.surfaceContainerHighest.copy(alpha = 0.96f),
                                        tonalElevation = 6.dp,
                                        shadowElevation = 8.dp,
                                        modifier = Modifier.widthIn(max = 460.dp)
                                    ) {
                                        Row(
                                            Modifier.padding(12.dp),
                                            verticalAlignment = Alignment.CenterVertically
                                        ) {
                                            ModernSpriteThumb(atlas, sp, 52)

                                            Spacer(Modifier.width(12.dp))

                                            Column(Modifier.weight(1f)) {
                                                Row(verticalAlignment = Alignment.CenterVertically) {
                                                    Text(
                                                        sp.displayName,
                                                        style = MaterialTheme.typography.titleSmall,
                                                        fontWeight = FontWeight.Bold,
                                                        maxLines = 1,
                                                        overflow = TextOverflow.Ellipsis,
                                                        modifier = Modifier.weight(1f, fill = false)
                                                    )
                                                    Spacer(Modifier.width(6.dp))
                                                    Surface(
                                                        shape = RoundedCornerShape(6.dp),
                                                        color = cs.primaryContainer
                                                    ) {
                                                        Text(
                                                            "ID ${sp.id}",
                                                            Modifier.padding(horizontal = 6.dp, vertical = 1.dp),
                                                            style = MaterialTheme.typography.labelSmall,
                                                            fontFamily = FontFamily.Monospace,
                                                            color = cs.onPrimaryContainer
                                                        )
                                                    }
                                                }
                                                Text(
                                                    "${sp.w.toInt()}×${sp.h.toInt()} px • UV (${sp.u0.toInt()},${sp.v0.toInt()})-(${sp.u1.toInt()},${sp.v1.toInt()})",
                                                    style = MaterialTheme.typography.labelSmall,
                                                    fontFamily = FontFamily.Monospace,
                                                    color = cs.onSurfaceVariant,
                                                    maxLines = 1,
                                                    overflow = TextOverflow.Ellipsis
                                                )
                                            }

                                            Spacer(Modifier.width(8.dp))

                                            IconButton(
                                                onClick = onReplace,
                                                enabled = !loading,
                                                colors = IconButtonDefaults.iconButtonColors(
                                                    containerColor = cs.primary,
                                                    contentColor = cs.onPrimary
                                                )
                                            ) {
                                                Icon(
                                                    Icons.Rounded.Edit,
                                                    tr("modern.replace_png")
                                                )
                                            }
                                            Spacer(Modifier.width(6.dp))
                                            IconButton(
                                                onClick = { onExportSprite(sp) },
                                                enabled = !loading,
                                                colors = IconButtonDefaults.iconButtonColors(
                                                    containerColor = cs.secondaryContainer,
                                                    contentColor = cs.onSecondaryContainer
                                                )
                                            ) {
                                                Icon(
                                                    Icons.Rounded.Download,
                                                    tr("modern.save_png")
                                                )
                                            }
                                        }
                                    }
                                }
                            }

                            // Нижняя шторка со списком (телефон)
                            if (!wide) {
                                Surface(
                                    modifier = Modifier
                                        .align(Alignment.BottomCenter)
                                        .fillMaxWidth()
                                        .height(sheetHeight),
                                    shape = RoundedCornerShape(topStart = 28.dp, topEnd = 28.dp),
                                    color = cs.surfaceContainerLow,
                                    tonalElevation = 3.dp,
                                    shadowElevation = 10.dp
                                ) {
                                    Column(Modifier.fillMaxSize()) {
                                        Row(
                                            Modifier
                                                .fillMaxWidth()
                                                .height(56.dp)
                                                .clickable { sheetOpen = !sheetOpen }
                                                .padding(horizontal = 20.dp),
                                            verticalAlignment = Alignment.CenterVertically
                                        ) {
                                            Icon(Icons.Rounded.ViewList, null, tint = cs.primary)
                                            Spacer(Modifier.width(12.dp))
                                            Text(
                                                (tr("modern.sprites")) +
                                                        " (${filtered.size})",
                                                style = MaterialTheme.typography.titleSmall,
                                                modifier = Modifier.weight(1f)
                                            )
                                            Icon(
                                                if (sheetOpen) Icons.Rounded.ExpandMore
                                                else Icons.Rounded.ExpandLess,
                                                null,
                                                tint = cs.onSurfaceVariant
                                            )
                                        }
                                        if (sheetOpen) {
                                            ModernSpriteList(
                                                sprites = filtered,
                                                selected = selected,
                                                onClick = {
                                                    onSelect(it)
                                                    sheetOpen = false
                                                }
                                            )
                                        }
                                    }
                                }
                            }
                        }

                        // Боковая панель со списком (планшет/ландшафт)
                        if (wide) {
                            VerticalDivider()
                            Surface(
                                Modifier
                                    .width(320.dp)
                                    .fillMaxHeight(),
                                color = cs.surfaceContainerLow
                            ) {
                                Column {
                                    Row(
                                        Modifier
                                            .fillMaxWidth()
                                            .padding(horizontal = 16.dp, vertical = 12.dp),
                                        verticalAlignment = Alignment.CenterVertically
                                    ) {
                                        Text(
                                            (tr("modern.sprites_2")) + " (${filtered.size})",
                                            style = MaterialTheme.typography.labelMedium,
                                            fontWeight = FontWeight.Bold,
                                            color = cs.primary
                                        )
                                    }
                                    HorizontalDivider()
                                    ModernSpriteList(
                                        sprites = filtered,
                                        selected = selected,
                                        onClick = { onSelect(it) }
                                    )
                                }
                            }
                        }
                    }
                }
            }
        }

        // ── Тост ─────────────────────────────────────────────────────────

        var shown by remember { mutableStateOf("") }
        if (status.isNotEmpty()) shown = status

        LaunchedEffect(status) {
            if (status.isNotEmpty()) {
                delay(if (status.lines().size > 2) 6000L else 3500L)
                onStatusDismiss()
            }
        }

        AnimatedVisibility(
            visible = status.isNotEmpty(),
            modifier = Modifier
                .align(Alignment.TopCenter)
                .padding(top = 76.dp, start = 16.dp, end = 16.dp),
            enter = fadeIn() + slideInVertically { -it / 2 },
            exit = fadeOut() + slideOutVertically { -it / 2 }
        ) {
            val compact =
                shown.lines()
                    .map { it.trim() }
                    .filter { it.isNotEmpty() && !it.startsWith("Dir:") }
                    .joinToString("\n")
            // начала сообщений об ошибке берутся из файла языка (ключ common.error_prefixes, через «|»)
            val isError =
                compact.startsWith("Error") ||
                        tr("common.error_prefixes").split("|").any { it.isNotEmpty() && compact.startsWith(it) }

            Surface(
                shape = RoundedCornerShape(20.dp),
                color = cs.surfaceContainerHighest,
                tonalElevation = 6.dp,
                shadowElevation = 10.dp,
                modifier = Modifier
                    .widthIn(max = 520.dp)
                    .clickable { onStatusDismiss() }
            ) {
                Row(
                    Modifier.padding(horizontal = 16.dp, vertical = 12.dp),
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    Icon(
                        imageVector =
                            if (isError) Icons.Rounded.ErrorOutline else Icons.Rounded.CheckCircle,
                        contentDescription = null,
                        tint = if (isError) cs.error else cs.primary,
                        modifier = Modifier.size(20.dp)
                    )
                    Spacer(Modifier.width(12.dp))
                    Text(
                        compact,
                        style = MaterialTheme.typography.bodySmall,
                        color = cs.onSurface,
                        maxLines = 6,
                        overflow = TextOverflow.Ellipsis
                    )
                }
            }
        }

        // Затемнение + морфинг на время загрузки/переключения текстуры
        androidx.compose.animation.AnimatedVisibility(
            visible = loading,
            enter = androidx.compose.animation.fadeIn(),
            exit = androidx.compose.animation.fadeOut()
        ) {
            Box(
                Modifier
                    .fillMaxSize()
                    .background(androidx.compose.ui.graphics.Color.Black.copy(alpha = 0.55f))
                    .pointerInput(Unit) {},
                contentAlignment = Alignment.Center
            ) {
                MdLoadingIndicator(Modifier.size(72.dp), cs.primary)
            }
        }
    }
}

// ── Список спрайтов с автопрокруткой к выбранному ────────────────────────────

@Composable
private fun ModernSpriteList(
    sprites: List<ScSprite>,
    selected: ScSprite?,
    onClick: (ScSprite) -> Unit
) {
    val listState = rememberLazyListState()

    LaunchedEffect(selected?.id) {
        val id = selected?.id
        if (id != null) {
            val idx = sprites.indexOfFirst { it.id == id }
            if (idx >= 0) listState.animateScrollToItem(idx)
        }
    }

    LazyColumn(state = listState) {
        items(sprites, key = { it.id }) { sprite ->
            SpriteListItem(
                sprite = sprite,
                selected = selected?.id == sprite.id,
                onClick = { onClick(sprite) }
            )
        }
    }
}

// ── Миниатюра спрайта ────────────────────────────────────────────────────────

@Composable
private fun ModernSpriteThumb(atlas: ScAtlas, sprite: ScSprite, sizeDp: Int) {

    val bmp by produceState<ImageBitmap?>(null, sprite.id, atlas) {
        value = withContext(Dispatchers.Default) {
            try {
                cropSpriteBitmap(atlas, sprite)
            } catch (_: Throwable) {
                null
            }
        }
    }

    Box(
        Modifier
            .size(sizeDp.dp)
            .clip(RoundedCornerShape(16.dp))
            .background(MaterialTheme.colorScheme.surfaceContainerLowest)
            .border(
                1.dp,
                MaterialTheme.colorScheme.outlineVariant.copy(alpha = 0.4f),
                RoundedCornerShape(16.dp)
            ),
        contentAlignment = Alignment.Center
    ) {
        val b = bmp
        if (b != null) {
            Image(
                bitmap = b,
                contentDescription = null,
                modifier = Modifier.fillMaxSize().padding(3.dp),
                contentScale = ContentScale.Fit
            )
        }
    }
}

/*
 * AnimatedVisibility без неявного ресивера: внутри Row/Column компилятор
 * подставляет RowScope/ColumnScope-вариант и ругается. Обёртка на верхнем
 * уровне вызывает обычную версию.
 */
@Composable
private fun PopVisibility(
    visible: Boolean,
    modifier: Modifier,
    enter: EnterTransition,
    exit: ExitTransition,
    content: @Composable () -> Unit
) {
    AnimatedVisibility(
        visible = visible,
        modifier = modifier,
        enter = enter,
        exit = exit
    ) {
        content()
    }
}

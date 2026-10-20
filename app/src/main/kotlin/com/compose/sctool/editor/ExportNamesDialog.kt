package com.compose.sctool.editor

import com.compose.sctool.tr

import com.compose.sctool.MdLoadingIndicator

import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.rounded.Add
import androidx.compose.material.icons.rounded.Delete
import androidx.compose.material.icons.rounded.Edit
import androidx.compose.material.icons.rounded.Movie
import androidx.compose.material.icons.automirrored.rounded.Undo
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import com.compose.sctool.ScProcessor
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

/** Строка списка. origIndex = -1 для добавленных. */
class ExportRow(
    val origIndex: Int,
    val objectId: Int,
    val origName: String,
    name: String,
    deleted: Boolean = false
) {
    var name by mutableStateOf(name)
    var deleted by mutableStateOf(deleted)
}

private fun buildOps(rows: List<ExportRow>): String {
    val sb = StringBuilder()
    for (r in rows) {
        if (r.origIndex < 0) continue
        if (r.deleted) sb.append("D\t").append(r.origIndex).append('\n')
        else if (r.name != r.origName)
            sb.append("R\t").append(r.origIndex).append('\t').append(r.name).append('\n')
    }
    for (r in rows) {
        if (r.origIndex < 0 && !r.deleted)
            sb.append("A\t").append(r.objectId).append('\t').append(r.name).append('\n')
    }
    return sb.toString()
}

/**
 * onApply(opsText, done): done(null) — успех, done("текст") — ошибка.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun ExportNamesDialog(
    isRu: Boolean,
    scPath: String,
    onApply: (String, (String?) -> Unit) -> Unit,
    onDismiss: () -> Unit
) {
    val rows = remember(scPath) { mutableStateListOf<ExportRow>() }
    var loaded by remember(scPath) { mutableStateOf(false) }
    var loadError by remember(scPath) { mutableStateOf<String?>(null) }
    var query by remember { mutableStateOf("") }
    var error by remember { mutableStateOf<String?>(null) }
    var busy by remember { mutableStateOf(false) }
    var editRow by remember { mutableStateOf<ExportRow?>(null) }
    var showAdd by remember { mutableStateOf(false) }
    var showClips by remember { mutableStateOf(false) }

    LaunchedEffect(scPath) {
        val text = withContext(Dispatchers.IO) {
            try { ScProcessor.nativeGetExports(scPath) } catch (e: Throwable) { "Error: ${e.message}" }
        }
        if (text.startsWith("Error")) {
            loadError = text
        } else {
            var i = 0
            for (line in text.split('\n')) {
                val t = line.indexOf('\t')
                if (t <= 0) continue
                val id = line.substring(0, t).toIntOrNull() ?: continue
                val nm = line.substring(t + 1)
                rows.add(ExportRow(i, id, nm, nm))
                i++
            }
        }
        loaded = true
    }

    val q = query.trim().lowercase()
    val shown = rows.filter {
        q.isEmpty() || it.name.lowercase().contains(q) || it.objectId.toString() == q
    }
    val changes = buildOps(rows).isNotEmpty()

    Dialog(
        onDismissRequest = { if (!busy) onDismiss() },
        properties = DialogProperties(usePlatformDefaultWidth = false)
    ) {
        Surface(
            shape = MaterialTheme.shapes.extraLarge,
            color = MaterialTheme.colorScheme.surfaceContainerHigh,
            modifier = Modifier
                .fillMaxWidth(0.94f)
                .fillMaxHeight(0.86f)
        ) {
            Column(Modifier.padding(16.dp)) {

                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text(
                        tr("export.export_names"),
                        style = MaterialTheme.typography.titleLarge,
                        modifier = Modifier.weight(1f)
                    )
                    FilledTonalIconButton(
                        onClick = { showClips = true },
                        enabled = loaded && loadError == null && !busy
                    ) {
                        Icon(Icons.Rounded.Movie, tr("export.movieclips"))
                    }
                    Spacer(Modifier.width(6.dp))
                    FilledTonalIconButton(
                        onClick = { showAdd = true },
                        enabled = loaded && loadError == null && !busy
                    ) {
                        Icon(Icons.Rounded.Add, tr("export.add"))
                    }
                }

                Spacer(Modifier.height(8.dp))

                OutlinedTextField(
                    value = query,
                    onValueChange = { query = it },
                    singleLine = true,
                    label = { Text(tr("export.search_by_name_or_id")) },
                    modifier = Modifier.fillMaxWidth()
                )

                Spacer(Modifier.height(8.dp))

                Box(Modifier.weight(1f).fillMaxWidth()) {
                    if (!loaded) {
                        MdLoadingIndicator(Modifier.align(Alignment.Center).size(56.dp))
                    } else if (loadError != null) {
                        Text(
                            loadError ?: "",
                            color = MaterialTheme.colorScheme.error,
                            modifier = Modifier.align(Alignment.Center)
                        )
                    } else {
                        LazyColumn(Modifier.fillMaxSize()) {
                            items(shown) { r ->
                                ExportRowItem(
                                    isRu = isRu,
                                    row = r,
                                    enabled = !busy,
                                    onEdit = { editRow = r },
                                    onDelete = {
                                        if (r.origIndex < 0) rows.remove(r)
                                        else r.deleted = !r.deleted
                                    }
                                )
                            }
                        }
                    }
                }

                val err = error
                if (err != null) {
                    Spacer(Modifier.height(6.dp))
                    Text(
                        err,
                        color = MaterialTheme.colorScheme.error,
                        style = MaterialTheme.typography.bodySmall
                    )
                }

                Spacer(Modifier.height(10.dp))

                Row(
                    Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.End,
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    TextButton(onClick = onDismiss, enabled = !busy) {
                        Text(tr("export.cancel"))
                    }
                    Spacer(Modifier.width(8.dp))
                    Button(
                        enabled = changes && !busy,
                        onClick = {
                            busy = true
                            error = null
                            onApply(buildOps(rows)) { res ->
                                busy = false
                                if (res != null) error = res
                            }
                        }
                    ) {
                        Text(tr("export.apply"))
                    }
                }
            }
        }
    }

    if (showClips) {
        MovieClipsDialog(isRu = isRu, scPath = scPath, onDismiss = { showClips = false })
    }

    val er = editRow
    if (er != null) {
        var text by remember(er) { mutableStateOf(er.name) }
        AlertDialog(
            onDismissRequest = { editRow = null },
            title = { Text(tr("export.rename")) },
            text = {
                OutlinedTextField(
                    value = text,
                    onValueChange = { text = it },
                    singleLine = true,
                    supportingText = { Text("ID: ${er.objectId}") }
                )
            },
            confirmButton = {
                TextButton(
                    enabled = text.isNotBlank(),
                    onClick = { er.name = text.trim(); editRow = null }
                ) { Text("OK") }
            },
            dismissButton = {
                TextButton(onClick = { editRow = null }) {
                    Text(tr("export.cancel"))
                }
            }
        )
    }

    if (showAdd) {
        var idText by remember { mutableStateOf("") }
        var nameText by remember { mutableStateOf("") }
        AlertDialog(
            onDismissRequest = { showAdd = false },
            title = { Text(tr("export.new_export_name")) },
            text = {
                Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    OutlinedTextField(
                        value = idText,
                        onValueChange = { idText = it.filter { c -> c.isDigit() }.take(5) },
                        singleLine = true,
                        label = { Text(tr("export.movieclip_id")) },
                        keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number)
                    )
                    OutlinedTextField(
                        value = nameText,
                        onValueChange = { nameText = it },
                        singleLine = true,
                        label = { Text(tr("export.name")) }
                    )
                }
            },
            confirmButton = {
                TextButton(
                    enabled = idText.isNotEmpty() && nameText.isNotBlank(),
                    onClick = {
                        val id = idText.toIntOrNull()
                        if (id != null) rows.add(ExportRow(-1, id, "", nameText.trim()))
                        showAdd = false
                    }
                ) { Text(tr("export.add")) }
            },
            dismissButton = {
                TextButton(onClick = { showAdd = false }) {
                    Text(tr("export.cancel"))
                }
            }
        )
    }
}

@Composable
private fun ExportRowItem(
    isRu: Boolean,
    row: ExportRow,
    enabled: Boolean,
    onEdit: () -> Unit,
    onDelete: () -> Unit
) {
    val cs = MaterialTheme.colorScheme
    val isNew = row.origIndex < 0
    val edited = !isNew && !row.deleted && row.name != row.origName
    val nameColor = if (row.deleted) cs.onSurfaceVariant else cs.onSurface
    val tag = if (row.deleted) (tr("export.deleted"))
        else if (isNew) (tr("export.new"))
        else if (edited) (tr("export.edited"))
        else ""
    val sub = if (tag.isEmpty()) "ID ${row.objectId}" else "ID ${row.objectId} · $tag"

    Row(
        Modifier
            .fillMaxWidth()
            .padding(vertical = 4.dp),
        verticalAlignment = Alignment.CenterVertically
    ) {
        Column(Modifier.weight(1f)) {
            Text(
                row.name,
                color = nameColor,
                style = MaterialTheme.typography.bodyLarge,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis
            )
            Text(
                sub,
                color = if (tag.isEmpty()) cs.onSurfaceVariant else cs.primary,
                style = MaterialTheme.typography.bodySmall
            )
        }
        IconButton(onClick = onEdit, enabled = enabled && !row.deleted) {
            Icon(Icons.Rounded.Edit, tr("export.rename"))
        }
        IconButton(onClick = onDelete, enabled = enabled) {
            val ic = if (row.deleted) Icons.AutoMirrored.Rounded.Undo else Icons.Rounded.Delete
            Icon(ic, tr("export.delete"))
        }
    }
}


/** Строка списка MovieClip'ов (только чтение). */
private class ClipRow(val id: Int, val name: String, val frames: Int)

/** Список MovieClip'ов файла: ID, экспортнейм (если есть), число кадров. */
@Composable
fun MovieClipsDialog(isRu: Boolean, scPath: String, onDismiss: () -> Unit) {
    val rows = remember(scPath) { mutableStateListOf<ClipRow>() }
    var loaded by remember(scPath) { mutableStateOf(false) }
    var loadError by remember(scPath) { mutableStateOf<String?>(null) }
    var query by remember { mutableStateOf("") }
    var onlyNamed by remember { mutableStateOf(false) }

    LaunchedEffect(scPath) {
        val text = withContext(Dispatchers.IO) {
            try { ScProcessor.nativeGetMovieClips(scPath) } catch (e: Throwable) { "Error: ${e.message}" }
        }
        if (text.startsWith("Error")) {
            loadError = text
        } else {
            for (line in text.split('\n')) {
                val parts = line.split('\t')
                if (parts.size < 3) continue
                val id = parts[0].toIntOrNull() ?: continue
                rows.add(ClipRow(id, parts[1], parts[2].toIntOrNull() ?: 0))
            }
        }
        loaded = true
    }

    val q = query.trim().lowercase()
    val shown = rows.filter {
        (!onlyNamed || it.name.isNotEmpty()) &&
            (q.isEmpty() || it.name.lowercase().contains(q) || it.id.toString() == q)
    }

    Dialog(
        onDismissRequest = onDismiss,
        properties = DialogProperties(usePlatformDefaultWidth = false)
    ) {
        Surface(
            shape = MaterialTheme.shapes.extraLarge,
            color = MaterialTheme.colorScheme.surfaceContainerHigh,
            modifier = Modifier
                .fillMaxWidth(0.94f)
                .fillMaxHeight(0.86f)
        ) {
            Column(Modifier.padding(16.dp)) {
                Text(
                    tr("export.movieclips"),
                    style = MaterialTheme.typography.titleLarge
                )
                Text(
                    (tr("export.shown")) + shown.size + (tr("export.of")) + rows.size,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
                Spacer(Modifier.height(8.dp))
                OutlinedTextField(
                    value = query,
                    onValueChange = { query = it },
                    singleLine = true,
                    label = { Text(tr("export.search_by_name_or_id")) },
                    modifier = Modifier.fillMaxWidth()
                )
                Spacer(Modifier.height(4.dp))
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Checkbox(checked = onlyNamed, onCheckedChange = { onlyNamed = it })
                    Text(
                        tr("export.only_with_export_name"),
                        style = MaterialTheme.typography.bodyMedium
                    )
                }
                Box(Modifier.weight(1f).fillMaxWidth()) {
                    if (!loaded) {
                        MdLoadingIndicator(Modifier.align(Alignment.Center).size(56.dp))
                    } else if (loadError != null) {
                        Text(
                            loadError ?: "",
                            color = MaterialTheme.colorScheme.error,
                            modifier = Modifier.align(Alignment.Center)
                        )
                    } else {
                        LazyColumn(Modifier.fillMaxSize()) {
                            items(shown) { r ->
                                Column(Modifier.fillMaxWidth().padding(vertical = 6.dp)) {
                                    Text(
                                        if (r.name.isEmpty()) "—" else r.name,
                                        style = MaterialTheme.typography.bodyLarge,
                                        maxLines = 1,
                                        overflow = TextOverflow.Ellipsis
                                    )
                                    Text(
                                        "ID ${r.id} · " + r.frames + (tr("export.fr")),
                                        style = MaterialTheme.typography.bodySmall,
                                        color = MaterialTheme.colorScheme.onSurfaceVariant
                                    )
                                }
                                HorizontalDivider()
                            }
                        }
                    }
                }
                Spacer(Modifier.height(8.dp))
                Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.End) {
                    TextButton(onClick = onDismiss) { Text(tr("export.close")) }
                }
            }
        }
    }
}

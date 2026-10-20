package com.compose.sctool

import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.slideInHorizontally
import androidx.compose.animation.slideOutHorizontally
import androidx.compose.foundation.Image
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.clickable
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.rounded.InsertDriveFile
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.ui.platform.LocalContext
import androidx.core.content.FileProvider
import java.io.File
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.net.HttpURLConnection
import java.net.URL

// Картинка по ссылке: меняешь файл на Dropbox — меняется в приложении.
private const val AVATAR_URL =
    "https://www.dropbox.com/scl/fi/tkasy03ebjv4qpqhdx0ma/icon.jpg?rlkey=6mny92tvt0w1j5pjp3fcchaum&st=mesa6fwh&dl=1"

private fun loadAvatar(): ImageBitmap? {
    return try {
        val c = URL(AVATAR_URL).openConnection() as HttpURLConnection
        c.connectTimeout = 8000
        c.readTimeout = 8000
        c.instanceFollowRedirects = true
        val bmp = c.inputStream.use { android.graphics.BitmapFactory.decodeStream(it) }
        bmp?.asImageBitmap()
    } catch (e: Exception) {
        null
    }
}

@Composable
fun GreetingScreen(isRu: Boolean, saveDir: File? = null) {
    val ctx = LocalContext.current
    val recent = remember(saveDir) {
        try {
            saveDir?.listFiles()?.filter { it.isFile && !it.name.startsWith(".") }
                ?.sortedByDescending { it.lastModified() }?.take(4) ?: emptyList()
        } catch (e: Exception) { emptyList<File>() }
    }
    var avatar by remember { mutableStateOf<ImageBitmap?>(null) }
    LaunchedEffect(Unit) {
        avatar = withContext(Dispatchers.IO) { loadAvatar() }
    }

    Box(Modifier.fillMaxSize()) {
        Column(
            Modifier.align(Alignment.Center).padding(24.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.spacedBy(8.dp)
        ) {
            Text(
                tr("welcome.hi"),
                fontSize = 44.sp,
                fontWeight = FontWeight.Bold,
                color = MaterialTheme.colorScheme.onSurface
            )
            Text(
                tr("welcome.swipe_from_the_left_edge_to_open_t"),
                style = MaterialTheme.typography.bodyLarge,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                textAlign = TextAlign.Center
            )
        }

        if (recent.isNotEmpty()) {
            Column(
                Modifier.align(Alignment.BottomCenter).fillMaxWidth().padding(16.dp),
                verticalArrangement = Arrangement.spacedBy(8.dp)
            ) {
                Text(
                    tr("welcome.recently_saved"),
                    style = MaterialTheme.typography.labelLarge,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
                recent.forEach { file ->
                    Surface(
                        Modifier.fillMaxWidth().clip(RoundedCornerShape(20.dp)).clickable {
                            try {
                                val uri = FileProvider.getUriForFile(ctx, ctx.packageName + ".fileprovider", file)
                                val i = android.content.Intent(android.content.Intent.ACTION_VIEW)
                                i.setDataAndType(uri, ctx.contentResolver.getType(uri) ?: "*/*")
                                i.addFlags(android.content.Intent.FLAG_GRANT_READ_URI_PERMISSION)
                                ctx.startActivity(i)
                            } catch (e: Exception) {
                            }
                        },
                        color = MaterialTheme.colorScheme.surfaceContainerHigh
                    ) {
                        Row(Modifier.padding(horizontal = 16.dp, vertical = 12.dp),
                            verticalAlignment = Alignment.CenterVertically) {
                            Icon(Icons.Rounded.InsertDriveFile, null, Modifier.size(20.dp),
                                tint = MaterialTheme.colorScheme.primary)
                            Spacer(Modifier.width(12.dp))
                            Text(file.name, maxLines = 1,
                                overflow = androidx.compose.ui.text.style.TextOverflow.Ellipsis,
                                style = MaterialTheme.typography.bodyMedium)
                        }
                    }
                }
            }
        }

        Box(Modifier.align(Alignment.TopEnd).padding(16.dp)) {
            AnimatedVisibility(
                visible = avatar != null,
                enter = slideInHorizontally { it * 2 } + fadeIn(),
                exit = slideOutHorizontally { it * 2 } + fadeOut()
            ) {
                val img = avatar
                if (img != null) {
                    Image(
                        bitmap = img,
                        contentDescription = null,
                        contentScale = ContentScale.Crop,
                        modifier = Modifier.size(60.dp).clip(MdCookieShape(9, 0.07f))
                    )
                }
            }
        }
    }
}

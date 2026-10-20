package com.compose.sctool

import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.Build
import android.os.Environment
import androidx.core.content.FileProvider
import java.io.File

object FileHelper {

    private const val AUTHORITY = "com.compose.sctool.fileprovider"

    /** Папка для сохранения результатов: Download/SCTool/ */
    fun getOutDir(ctx: Context): File {
        val dir = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            // Android 10+ — используем Downloads через MediaStore или app-specific
            File(ctx.getExternalFilesDir(null), "SCTool")
        } else {
            File(Environment.getExternalStoragePublicDirectory(
                Environment.DIRECTORY_DOWNLOADS), "SCTool")
        }
        dir.mkdirs()
        return dir
    }

    /** URI для шаринга файла через FileProvider */
    fun shareFile(ctx: Context, file: File): Intent {
        val uri = FileProvider.getUriForFile(ctx, AUTHORITY, file)
        return Intent(Intent.ACTION_VIEW).apply {
            setDataAndType(uri, ctx.contentResolver.getType(uri) ?: "*/*")
            addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
        }
    }

    /** Скопировать Uri во временный файл в cache */
    fun copyToCache(ctx: Context, uri: Uri, name: String): File {
        val file = File(ctx.cacheDir, name)
        ctx.contentResolver.openInputStream(uri)?.use { it.copyTo(file.outputStream()) }
        return file
    }

    /** Получить имя файла из Uri */
    fun getName(ctx: Context, uri: Uri): String {
        ctx.contentResolver.query(uri, null, null, null, null)?.use { c ->
            val idx = c.getColumnIndex(android.provider.OpenableColumns.DISPLAY_NAME)
            if (c.moveToFirst() && idx >= 0) return c.getString(idx)
        }
        return uri.lastPathSegment ?: "file"
    }

    /** Найти выходной PNG/файл в папке по базовому имени */
    fun findOutput(outDir: File, baseName: String, ext: String): File? {
        return outDir.listFiles()?.firstOrNull {
            it.nameWithoutExtension == baseName && it.extension == ext
        }
    }
}

package com.compose.sctool

import android.database.Cursor
import android.database.MatrixCursor
import android.os.CancellationSignal
import android.os.ParcelFileDescriptor
import android.provider.DocumentsContract.Document
import android.provider.DocumentsContract.Root
import android.provider.DocumentsProvider
import android.webkit.MimeTypeMap
import java.io.File
import java.io.FileNotFoundException

class SCToolDocumentsProvider : DocumentsProvider() {

    companion object {
        const val AUTHORITY = "com.compose.sctool.documents"
        
        private val ROOT_PROJECTION = arrayOf(
            Root.COLUMN_ROOT_ID,
            Root.COLUMN_MIME_TYPES,
            Root.COLUMN_FLAGS,
            Root.COLUMN_ICON,
            Root.COLUMN_TITLE,
            Root.COLUMN_SUMMARY,
            Root.COLUMN_DOCUMENT_ID,
            Root.COLUMN_AVAILABLE_BYTES
        )
        
        private val DOCUMENT_PROJECTION = arrayOf(
            Document.COLUMN_DOCUMENT_ID,
            Document.COLUMN_MIME_TYPE,
            Document.COLUMN_DISPLAY_NAME,
            Document.COLUMN_LAST_MODIFIED,
            Document.COLUMN_FLAGS,
            Document.COLUMN_SIZE
        )
    }

    // Безопасное получение папки: вызов происходит строго во время работы методов, а не при инициализации класса
    private fun rootDir(): File {
        val ctx = context ?: throw IllegalStateException("Context not attached")
        val dir = FileHelper.getOutDir(ctx)
        if (!dir.exists()) {
            dir.mkdirs()
        }
        return dir
    }

    override fun onCreate(): Boolean = true

    override fun queryRoots(projection: Array<out String>?): Cursor {
        val result = MatrixCursor(projection ?: ROOT_PROJECTION)
        val dir = rootDir()

        result.newRow().apply {
            add(Root.COLUMN_ROOT_ID,      "root")
            add(Root.COLUMN_DOCUMENT_ID,  "root")
            add(Root.COLUMN_TITLE,        "QwScDroid")
            add(Root.COLUMN_SUMMARY,      dir.absolutePath)
            // Добавили FLAG_SUPPORTS_IS_CHILD для правильного отображения структуры
            add(Root.COLUMN_FLAGS,        Root.FLAG_SUPPORTS_CREATE or Root.FLAG_LOCAL_ONLY or Root.FLAG_SUPPORTS_IS_CHILD)
            add(Root.COLUMN_MIME_TYPES,   "*/*")
            add(Root.COLUMN_ICON,         R.mipmap.ic_launcher)
            add(Root.COLUMN_AVAILABLE_BYTES, dir.freeSpace)
        }
        return result
    }

    override fun queryDocument(documentId: String, projection: Array<out String>?): Cursor {
        val result = MatrixCursor(projection ?: DOCUMENT_PROJECTION)
        val file = if (documentId == "root") rootDir() else File(documentId)
        addFileRow(result, file, documentId)
        return result
    }

    override fun queryChildDocuments(
        parentDocumentId: String,
        projection: Array<out String>?,
        sortOrder: String?
    ): Cursor {
        val result = MatrixCursor(projection ?: DOCUMENT_PROJECTION)
        val parent = if (parentDocumentId == "root") rootDir() else File(parentDocumentId)
        
        parent.listFiles()?.forEach { file ->
            addFileRow(result, file, file.absolutePath)
        }
        return result
    }

    override fun openDocument(
        documentId: String,
        mode: String,
        signal: CancellationSignal?
    ): ParcelFileDescriptor {
        val file = File(documentId)
        val accessMode = ParcelFileDescriptor.parseMode(mode)
        return ParcelFileDescriptor.open(file, accessMode)
            ?: throw FileNotFoundException("Cannot open: $documentId")
    }

    private fun addFileRow(cursor: MatrixCursor, file: File, docId: String) {
        val mimeType = if (file.isDirectory) Document.MIME_TYPE_DIR
                       else getMimeType(file.name)
        
        var flags = 0
        if (file.isDirectory) flags = flags or Document.FLAG_DIR_SUPPORTS_CREATE
        if (file.canWrite()) flags = flags or Document.FLAG_SUPPORTS_DELETE or Document.FLAG_SUPPORTS_WRITE

        // Задаем красивое имя для корневой папки, чтобы не было пустых строк
        val displayName = if (docId == "root") "QwScDroid" else file.name

        cursor.newRow().apply {
            add(Document.COLUMN_DOCUMENT_ID, docId)
            add(Document.COLUMN_DISPLAY_NAME, displayName)
            add(Document.COLUMN_MIME_TYPE,    mimeType)
            add(Document.COLUMN_LAST_MODIFIED, file.lastModified())
            add(Document.COLUMN_FLAGS,         flags)
            add(Document.COLUMN_SIZE,          if (file.isFile) file.length() else null)
        }
    }

    private fun getMimeType(name: String): String {
        val ext = name.substringAfterLast('.', "")
        return MimeTypeMap.getSingleton().getMimeTypeFromExtension(ext) ?: "application/octet-stream"
    }
}


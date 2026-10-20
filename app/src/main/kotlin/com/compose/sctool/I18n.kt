package com.compose.sctool

import android.content.Context
import org.json.JSONObject
import java.io.File

/**
 * Тексты приложения лежат в файлах, а не в коде:
 *   assets/lang/en.json и assets/lang/ru.json   — встроенные языки
 *   <внутренняя папка приложения>/files/lang/ХХ.json — свои языки и правки, без пересборки
 *     (Android/data/<пакет>/files/lang/xx.json)
 *
 * Формат: {"_name": "Название языка", "ключ": "текст {0} {1}", ...}
 * Новый язык = скопировать en.json, переименовать в код языка (de.json), перевести значения.
 * Если ключа нет в выбранном языке — берётся английский, затем сам ключ.
 */
object I18n {
    private var base: Map<String, String> = emptyMap()
    private var cur: Map<String, String> = emptyMap()

    fun init(ctx: Context) {
        val code = try { LangHelper.getSaved(ctx) } catch (e: Exception) { "en" }
        base = load(ctx, "en")
        cur = if (code == "en") base else load(ctx, code)
    }

    fun t(key: String): String = cur[key] ?: base[key] ?: key

    fun t(key: String, vararg args: Any?): String {
        var s = t(key)
        for (i in args.indices) s = s.replace("{" + i + "}", args[i].toString())
        return s
    }

    /** Все доступные языки: код → название (из поля _name). */
    fun languages(ctx: Context): List<Pair<String, String>> {
        val codes = LinkedHashSet<String>()
        try { ctx.assets.list("lang")?.forEach { if (it.endsWith(".json")) codes.add(it.removeSuffix(".json")) } } catch (e: Exception) {}
        try { externalDir(ctx)?.listFiles()?.forEach { if (it.name.endsWith(".json")) codes.add(it.name.removeSuffix(".json")) } } catch (e: Exception) {}
        val res = ArrayList<Pair<String, String>>()
        for (c in codes) {
            val name = load(ctx, c)["_name"] ?: c
            res.add(Pair(c, name))
        }
        return res
    }

    private fun externalDir(ctx: Context): File? = ctx.getExternalFilesDir("lang")

    private fun load(ctx: Context, code: String): Map<String, String> {
        val map = HashMap<String, String>()
        try {
            ctx.assets.open("lang/$code.json").use { map.putAll(parse(it.bufferedReader().readText())) }
        } catch (e: Exception) {}
        try {
            val f = File(externalDir(ctx), "$code.json")
            if (f.exists()) map.putAll(parse(f.readText()))
        } catch (e: Exception) {}
        return map
    }

    private fun parse(text: String): Map<String, String> {
        val out = HashMap<String, String>()
        try {
            val o = JSONObject(text)
            val it = o.keys()
            while (it.hasNext()) {
                val k = it.next()
                out[k] = o.optString(k)
            }
        } catch (e: Exception) {}
        return out
    }
}

/** Короткий вызов: tr("ключ") или tr("ключ", аргумент0, аргумент1) для {0}, {1}. */
fun tr(key: String): String = I18n.t(key)
fun tr(key: String, vararg args: Any?): String = I18n.t(key, *args)

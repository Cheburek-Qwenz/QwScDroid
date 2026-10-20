package com.compose.sctool

import android.content.Context
import android.content.res.Configuration
import java.util.Locale

object LangHelper {
    private const val PREF = "settings"
    private const val KEY  = "language"

    fun getSaved(ctx: Context): String =
        ctx.getSharedPreferences(PREF, 0).getString(KEY, "") ?: ""

    fun save(ctx: Context, lang: String) =
        ctx.getSharedPreferences(PREF, 0).edit().putString(KEY, lang).apply()

    fun isFirst(ctx: Context) = getSaved(ctx).isEmpty()

    fun apply(ctx: Context): Context {
        val lang = getSaved(ctx)
        if (lang.isEmpty()) return ctx
        val locale = Locale(lang)
        Locale.setDefault(locale)
        val config = Configuration(ctx.resources.configuration)
        config.setLocale(locale)
        return ctx.createConfigurationContext(config)
    }
}

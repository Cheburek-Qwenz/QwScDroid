package com.compose.sctool

import android.content.Intent
import android.net.Uri
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.ClickableText
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.SpanStyle
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.buildAnnotatedString
import androidx.compose.ui.text.withStyle
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextDecoration
import androidx.compose.ui.unit.dp

/**
 * Лёгкий Markdown для патчноутов GitHub.
 * Поддержка: # ## ### заголовки, - * + списки (с вложенностью), 1. списки,
 * - [ ] / - [x] задачи, > цитаты, ``` блоки кода, ---, **жирный**, *курсив*,
 * ~~зачёркнутый~~, `код`, [текст](url) и голые http(s)-ссылки.
 */
private sealed class MdBlock {
    data class Heading(val level: Int, val text: String) : MdBlock()
    data class Para(val text: String) : MdBlock()
    data class Item(val marker: String, val text: String, val depth: Int) : MdBlock()
    data class Quote(val text: String) : MdBlock()
    data class Code(val text: String) : MdBlock()
    object Rule : MdBlock()
    object Gap : MdBlock()
}

private fun parseBlocks(src: String): List<MdBlock> {
    val out = ArrayList<MdBlock>()
    val lines = src.replace("\r\n", "\n").replace('\r', '\n').split('\n')
    var i = 0
    val para = StringBuilder()

    fun flushPara() {
        if (para.isNotEmpty()) {
            out.add(MdBlock.Para(para.toString().trim()))
            para.setLength(0)
        }
    }

    while (i < lines.size) {
        val raw = lines[i]
        val line = raw.trimEnd()
        val t = line.trimStart()

        if (t.startsWith("```")) {
            flushPara()
            val sb = StringBuilder()
            i++
            while (i < lines.size && !lines[i].trimStart().startsWith("```")) {
                if (sb.isNotEmpty()) sb.append('\n')
                sb.append(lines[i])
                i++
            }
            out.add(MdBlock.Code(sb.toString()))
            i++
            continue
        }

        if (t.isEmpty()) {
            flushPara()
            if (out.isNotEmpty() && out.last() !is MdBlock.Gap) out.add(MdBlock.Gap)
            i++
            continue
        }

        val h = Regex("^(#{1,6})\\s+(.*)$").find(t)
        if (h != null) {
            flushPara()
            out.add(MdBlock.Heading(h.groupValues[1].length, h.groupValues[2].trim().trimEnd('#').trim()))
            i++
            continue
        }

        if (Regex("^([-*_])(\\s*\\1){2,}$").matches(t)) {
            flushPara()
            out.add(MdBlock.Rule)
            i++
            continue
        }

        if (t.startsWith(">")) {
            flushPara()
            out.add(MdBlock.Quote(t.removePrefix(">").trim()))
            i++
            continue
        }

        val ul = Regex("^[-*+]\\s+(.*)$").find(t)
        val ol = Regex("^(\\d+)[.)]\\s+(.*)$").find(t)
        if (ul != null || ol != null) {
            flushPara()
            val indent = line.length - t.length
            val depth = (indent / 2).coerceIn(0, 3)
            if (ul != null) {
                var body = ul.groupValues[1]
                var marker = "•"
                if (body.startsWith("[ ] ")) { marker = "☐"; body = body.substring(4) }
                else if (body.startsWith("[x] ") || body.startsWith("[X] ")) { marker = "☑"; body = body.substring(4) }
                out.add(MdBlock.Item(marker, body, depth))
            } else if (ol != null) {
                out.add(MdBlock.Item(ol.groupValues[1] + ".", ol.groupValues[2], depth))
            }
            i++
            continue
        }

        if (para.isNotEmpty()) para.append(' ')
        para.append(t)
        i++
    }
    flushPara()
    while (out.isNotEmpty() && out.last() is MdBlock.Gap) out.removeAt(out.size - 1)
    return out
}

private const val URL_TAG = "URL"

private fun inline(
    src: String,
    codeBg: Color,
    linkColor: Color
): AnnotatedString = buildAnnotatedString {
    var i = 0
    val n = src.length

    fun closing(token: String, from: Int): Int = src.indexOf(token, from)

    while (i < n) {
        val c = src[i]

        if (c == '\\' && i + 1 < n && "\\`*_{}[]()#+-.!~>".indexOf(src[i + 1]) >= 0) {
            append(src[i + 1]); i += 2; continue
        }

        if (c == '`') {
            val e = closing("`", i + 1)
            if (e > i) {
                withStyle(SpanStyle(fontFamily = FontFamily.Monospace, background = codeBg)) {
                    append(src.substring(i + 1, e))
                }
                i = e + 1
                continue
            }
        }

        if (src.startsWith("**", i) || src.startsWith("__", i)) {
            val tok = src.substring(i, i + 2)
            val e = closing(tok, i + 2)
            if (e > i + 2) {
                withStyle(SpanStyle(fontWeight = FontWeight.Bold)) {
                    append(inline(src.substring(i + 2, e), codeBg, linkColor))
                }
                i = e + 2
                continue
            }
        }

        if (src.startsWith("~~", i)) {
            val e = closing("~~", i + 2)
            if (e > i + 2) {
                withStyle(SpanStyle(textDecoration = TextDecoration.LineThrough)) {
                    append(inline(src.substring(i + 2, e), codeBg, linkColor))
                }
                i = e + 2
                continue
            }
        }

        if ((c == '*' || c == '_') && i + 1 < n && src[i + 1] != ' ') {
            val prevOk = i == 0 || !src[i - 1].isLetterOrDigit()
            val e = closing(c.toString(), i + 1)
            if (prevOk && e > i + 1 && src[e - 1] != ' ') {
                withStyle(SpanStyle(fontStyle = FontStyle.Italic)) {
                    append(inline(src.substring(i + 1, e), codeBg, linkColor))
                }
                i = e + 1
                continue
            }
        }

        if (c == '[') {
            val m = Regex("^\\[([^\\]]+)\\]\\(([^)\\s]+)[^)]*\\)").find(src.substring(i))
            if (m != null) {
                val url = m.groupValues[2]
                pushStringAnnotation(URL_TAG, url)
                withStyle(SpanStyle(color = linkColor, textDecoration = TextDecoration.Underline)) {
                    append(m.groupValues[1])
                }
                pop()
                i += m.value.length
                continue
            }
        }

        if (c == 'h' && (src.startsWith("http://", i) || src.startsWith("https://", i))) {
            var e = i
            while (e < n && !src[e].isWhitespace() && src[e] != ')' && src[e] != '>') e++
            var url = src.substring(i, e)
            while (url.isNotEmpty() && ".,;:!?".indexOf(url.last()) >= 0) url = url.dropLast(1)
            if (url.length > 8) {
                pushStringAnnotation(URL_TAG, url)
                withStyle(SpanStyle(color = linkColor, textDecoration = TextDecoration.Underline)) {
                    append(url)
                }
                pop()
                i += url.length
                continue
            }
        }

        append(c)
        i++
    }
}

@Composable
private fun MdText(text: String, style: TextStyle, modifier: Modifier = Modifier) {
    val ctx = LocalContext.current
    val cs = MaterialTheme.colorScheme
    val annotated = inline(text, cs.surfaceContainerHighest, cs.primary)
    val styled = style.copy(color = cs.onSurface)
    ClickableText(
        text = annotated,
        style = styled,
        modifier = modifier,
        onClick = { off ->
            val a = annotated.getStringAnnotations(URL_TAG, off, off).firstOrNull()
            if (a != null) {
                try {
                    ctx.startActivity(
                        Intent(Intent.ACTION_VIEW, Uri.parse(a.item))
                            .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                    )
                } catch (e: Exception) {
                }
            }
        }
    )
}

@Composable
fun MarkdownText(markdown: String, modifier: Modifier = Modifier) {
    val cs = MaterialTheme.colorScheme
    val ty = MaterialTheme.typography
    val blocks = parseBlocks(markdown)

    Column(modifier) {
        for (b in blocks) {
            when (b) {
                is MdBlock.Heading -> {
                    val st =
                        if (b.level == 1) ty.titleLarge
                        else if (b.level == 2) ty.titleMedium
                        else ty.titleSmall
                    MdText(
                        b.text,
                        st.copy(fontWeight = FontWeight.Bold),
                        Modifier.padding(top = 8.dp, bottom = 2.dp)
                    )
                }
                is MdBlock.Para -> MdText(b.text, ty.bodyMedium, Modifier.padding(vertical = 2.dp))
                is MdBlock.Item -> {
                    Row(Modifier.padding(start = (b.depth * 16).dp, top = 1.dp, bottom = 1.dp)) {
                        Text(
                            b.marker,
                            style = ty.bodyMedium,
                            color = cs.primary,
                            modifier = Modifier.width(22.dp)
                        )
                        MdText(b.text, ty.bodyMedium, Modifier.weight(1f))
                    }
                }
                is MdBlock.Quote -> {
                    Row(Modifier.padding(vertical = 3.dp).height(IntrinsicSize.Min)) {
                        Box(
                            Modifier
                                .width(3.dp)
                                .fillMaxHeight()
                                .background(cs.outlineVariant, RoundedCornerShape(2.dp))
                        )
                        Spacer(Modifier.width(10.dp))
                        MdText(b.text, ty.bodyMedium.copy(fontStyle = FontStyle.Italic))
                    }
                }
                is MdBlock.Code -> {
                    Surface(
                        shape = RoundedCornerShape(12.dp),
                        color = cs.surfaceContainerHighest,
                        modifier = Modifier.fillMaxWidth().padding(vertical = 4.dp)
                    ) {
                        Text(
                            b.text,
                            modifier = Modifier.padding(10.dp),
                            style = ty.bodySmall.copy(fontFamily = FontFamily.Monospace),
                            color = cs.onSurface
                        )
                    }
                }
                is MdBlock.Rule -> HorizontalDivider(Modifier.padding(vertical = 6.dp))
                is MdBlock.Gap -> Spacer(Modifier.height(4.dp))
            }
        }
    }
}

package com.compose.sctool

import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.rounded.*

/**
 * Инструменты GIF <-> SC. Автор идеи и формата: @Ghoul1337i.
 * Подключаются в MainActivity вместе с ALL_SECTIONS, ToolDefs.kt менять не нужно.
 */
val GIF_SECTIONS: List<Section> get() = listOf(
    Section(tr("section.gif_sc"), listOf(
        ToolDef("gif2sc", tr("tool.gif2sc"), Icons.Rounded.Movie,
            listOf(
                ToolInput("file", tr("tool.gif2sc.file")),
                ToolInput("name", tr("tool.gif2sc.name"),
                    InputType.TEXT, "bgr_anim"),
                ToolInput("screen", tr("tool.gif2sc.screen"), InputType.SELECT, "1",
                    listOf(SelectOption("1", "2048×1152 (16:9)"), SelectOption("2", "1848×1112"))),
                ToolInput("tex", tr("tool.gif2sc.tex"), InputType.SELECT, "astc",
                    listOf(SelectOption("astc", tr("opt.gif2sc.tex.astc")),
                           SelectOption("rgba", tr("opt.gif2sc.tex.rgba")))),
                ToolInput("fps", tr("tool.gif2sc.fps"), InputType.NUMBER, "авто"),
                ToolInput("bg", tr("tool.gif2sc.bg"), InputType.TEXT, "000000")
            )),
        ToolDef("sc2gif", tr("tool.sc2gif"), Icons.Rounded.Movie,
            listOf(
                ToolInput("file", tr("tool.sc2gif.file")),
                ToolInput("name", tr("tool.sc2gif.name"), InputType.TEXT, ""),
                ToolInput("size", tr("tool.sc2gif.size"), InputType.NUMBER, "640"),
                ToolInput("fps", tr("tool.sc2gif.fps"), InputType.NUMBER, ""),
                ToolInput("masks", tr("tool.sc2gif.masks"), InputType.SELECT, "clip",
                    listOf(SelectOption("clip", tr("opt.sc2gif.masks.clip")),
                           SelectOption("ignore", tr("opt.sc2gif.masks.ignore"))))
            ))
    )),
    Section(tr("section.movieclips"), listOf(
        ToolDef("movieclips", tr("tool.movieclips"), Icons.Rounded.Movie,
            listOf(
                ToolInput("file", tr("tool.movieclips.file"))
            ))
    ))
)

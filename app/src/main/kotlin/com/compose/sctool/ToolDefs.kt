package com.compose.sctool

import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.rounded.*
import androidx.compose.ui.graphics.vector.ImageVector

enum class InputType { FILE, NUMBER, CHECKBOX, TEXT, SELECT, MULTI_FILE }

data class SelectOption(val value: String, val label: String)

data class ToolInput(
    val id:      String,
    val label:   String,
    val type:    InputType = InputType.FILE,
    val hint:    String = "",
    val options: List<SelectOption> = emptyList()  // для SELECT
)

data class ToolDef(
    val id:        String,
    val name:      String,
    val icon:      ImageVector,
    val inputs:    List<ToolInput>,
    val outputExt: String = ""
)

data class Section(
    val name:  String,
    val tools:  List<ToolDef>
)

private val ASTC_BLOCKS = listOf(
    SelectOption("4x4","4×4"), SelectOption("5x4","5×4"),
    SelectOption("5x5","5×5"), SelectOption("6x5","6×5"),
    SelectOption("6x6","6×6"), SelectOption("8x5","8×5"),
    SelectOption("8x6","8×6"), SelectOption("8x8","8×8"),
    SelectOption("10x5","10×5"), SelectOption("10x6","10×6"),
    SelectOption("10x8","10×8"), SelectOption("10x10","10×10"),
    SelectOption("12x10","12×10"), SelectOption("12x12","12×12"),
)

val ALL_SECTIONS: List<Section> get() = listOf(
    Section(tr("section.textures"), listOf(
        ToolDef("sctx2png", tr("tool.sctx2png"), Icons.Rounded.Image,
            listOf(ToolInput("file", tr("tool.sctx2png.file"))),
            outputExt = "png"),
        ToolDef("png2sctx", tr("tool.png2sctx"), Icons.Rounded.Upload,
            listOf(ToolInput("png", tr("tool.png2sctx.png")))),
        ToolDef("ktx2png", tr("tool.ktx2png"), Icons.Rounded.Image,
            listOf(ToolInput("file", tr("tool.ktx2png.file"))),
            outputExt = "png"),
        ToolDef("png2ktx", tr("tool.png2ktx"), Icons.Rounded.Upload,
            listOf(
                ToolInput("file", tr("tool.png2ktx.file")),
                ToolInput("block", tr("tool.png2ktx.block"),
                    InputType.SELECT, "8x8", ASTC_BLOCKS)
            )),
        ToolDef("png2sc", tr("tool.png2sc"), Icons.Rounded.SwapHoriz,
            listOf(
                ToolInput("png", tr("tool.png2sc.png")),
                ToolInput("orig", tr("tool.png2sc.orig")),
                ToolInput("idx", tr("tool.png2sc.idx"),
                    InputType.NUMBER, "0")
            )),
        ToolDef("atlas2sc", tr("tool.atlas2sc"), Icons.Rounded.Layers,
            listOf(
                ToolInput("png", tr("tool.atlas2sc.png")),
                ToolInput("orig", tr("tool.atlas2sc.orig"))
            )),
    )),
    Section(tr("section.sc_files"), listOf(
        ToolDef("info", tr("tool.info"), Icons.Rounded.Info,
            listOf(ToolInput("file", tr("tool.info.file")))),
        ToolDef("split", tr("tool.split"), Icons.Rounded.CallSplit,
            listOf(ToolInput("file", tr("tool.split.file")))),
        ToolDef("join", tr("tool.join"), Icons.Rounded.Add,
            listOf(
                ToolInput("logic", tr("tool.join.logic")),
                ToolInput("tex", tr("tool.join.tex"))
            )),
        ToolDef("extract", tr("tool.extract"), Icons.Rounded.Download,
            listOf(
                ToolInput("file", tr("tool.extract.file")),
                ToolInput("idx", tr("tool.extract.idx"), InputType.NUMBER, "0")
            )),
        ToolDef("sc2png", tr("tool.sc2png"), Icons.Rounded.Image,
            listOf(
                ToolInput("file", tr("tool.sc2png.file")),
                ToolInput("idx", tr("tool.sc2png.idx"),
                    InputType.NUMBER, "")
            ), outputExt = "png"),
        ToolDef("exports", tr("tool.exports"), Icons.Rounded.List,
            listOf(ToolInput("file", tr("tool.exports.file")))),
        ToolDef("downgrade", tr("tool.downgrade"), Icons.Rounded.ArrowDownward,
            listOf(ToolInput("file", tr("tool.downgrade.file")))),
    )),
    Section(tr("section.build"), listOf(
        ToolDef("build_sc", tr("tool.build_sc"), Icons.Rounded.Build,
            listOf(
                ToolInput("pngs", tr("tool.build_sc.pngs"),
                    InputType.MULTI_FILE),
                ToolInput("names", tr("tool.build_sc.names"), InputType.TEXT,
                    "hero_icon_shelly,hero_icon_colt"),
                ToolInput("block", tr("tool.build_sc.block"),
                    InputType.SELECT, "8x8", ASTC_BLOCKS)
            )),
    )),
)

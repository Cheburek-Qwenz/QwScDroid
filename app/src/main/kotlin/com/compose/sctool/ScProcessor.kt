package com.compose.sctool

object ScProcessor {
    init { System.loadLibrary("sctool") }

    // SC Files
    external fun nativeScInfo(sc: String): String
    external fun nativeSplitSc(sc: String, out: String): String
    external fun nativeJoinSc(logic: String, tex: String, out: String): String
    external fun nativeExtractKtx(sc: String, idx: Int, out: String): String
    external fun nativeDowngrade(sc: String, version: String, out: String): String

    // Textures
    external fun nativeKtx2Png(ktx: String, out: String): String
    external fun nativePng2Ktx(png: String, block: String, out: String): String
    external fun nativePng2Sc(png: String, orig: String, idx: Int, rotate: String, out: String): String
    external fun nativeSctx2Png(sctx: String, out: String): String
    external fun nativePng2ScBatch(list: String, orig: String, out: String): String
    external fun nativeGif2Sc(gif: String, name: String, fps: Int, screen: Int, bg: String, texFormat: Int, out: String): String
    external fun nativeSc2Gif(sc: String, name: String, maxSide: Int, fps: Int, out: String): String
    external fun nativeGetExports(path: String): String
    external fun nativeGetMovieClips(path: String): String
    external fun nativeEditExports(ops: String, orig: String, out: String): String
    external fun nativePng2Sctx(png: String, out: String): String

    // Sprites + SC
    external fun nativeCutSprites(logic: String, tex: String, out: String): String
    external fun nativeSc2Png(sc: String, idx: Int, out: String): String
    external fun nativeExports(sc: String, out: String): String

    // Inject
    external fun nativeInjectPreview(sc: String, out: String): String
    external fun nativeInjectPut(sc: String, sprite: String, name: String, out: String): String
    external fun nativeInjectList(sc: String): String

    // Build
    external fun nativeAnim2Static(sc: String, out: String): String
    external fun nativeBuildSc(pngPaths: String, names: String, block: String, out: String): String
    external fun nativeMcCopy(src: String, dst: String, names: String, out: String): String

    // Editor
    external fun nativeGetScInfo(scPath: String, outDir: String): String
    external fun nativeGetScTexture(scPath: String, outDir: String, textureIndex: Int): String
}

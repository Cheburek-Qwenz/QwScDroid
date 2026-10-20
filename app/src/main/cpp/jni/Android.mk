LOCAL_PATH := $(call my-dir)

# ── ZSTD ──────────────────────────────────────────────────────────
include $(CLEAR_VARS)
LOCAL_MODULE    := zstd
LOCAL_SRC_FILES := \
    zstd/lib/common/debug.c \
    zstd/lib/common/entropy_common.c \
    zstd/lib/common/error_private.c \
    zstd/lib/common/fse_decompress.c \
    zstd/lib/common/pool.c \
    zstd/lib/common/threading.c \
    zstd/lib/common/xxhash.c \
    zstd/lib/common/zstd_common.c \
    zstd/lib/compress/fse_compress.c \
    zstd/lib/compress/hist.c \
    zstd/lib/compress/huf_compress.c \
    zstd/lib/compress/zstd_compress.c \
    zstd/lib/compress/zstd_compress_literals.c \
    zstd/lib/compress/zstd_compress_sequences.c \
    zstd/lib/compress/zstd_compress_superblock.c \
    zstd/lib/compress/zstd_double_fast.c \
    zstd/lib/compress/zstd_fast.c \
    zstd/lib/compress/zstd_lazy.c \
    zstd/lib/compress/zstd_ldm.c \
    zstd/lib/compress/zstd_opt.c \
    zstd/lib/compress/zstdmt_compress.c \
    zstd/lib/decompress/huf_decompress.c \
    zstd/lib/decompress/zstd_ddict.c \
    zstd/lib/decompress/zstd_decompress.c \
    zstd/lib/decompress/zstd_decompress_block.c
LOCAL_C_INCLUDES := $(LOCAL_PATH)/zstd/lib
LOCAL_CFLAGS     := -O2 -DZSTD_MULTITHREAD=0
include $(BUILD_STATIC_LIBRARY)

# ── STB image ─────────────────────────────────────────────────────
include $(CLEAR_VARS)
LOCAL_MODULE    := stb
LOCAL_SRC_FILES := stb/stb_impl.c
LOCAL_C_INCLUDES := $(LOCAL_PATH)/stb
include $(BUILD_STATIC_LIBRARY)

# ── astcenc ───────────────────────────────────────────────────────
include $(CLEAR_VARS)
LOCAL_MODULE    := astcenc
LOCAL_SRC_FILES := \
    astcenc/Source/astcenc_averages_and_directions.cpp \
    astcenc/Source/astcenc_block_sizes.cpp \
    astcenc/Source/astcenc_color_quantize.cpp \
    astcenc/Source/astcenc_color_unquantize.cpp \
    astcenc/Source/astcenc_compress_symbolic.cpp \
    astcenc/Source/astcenc_compute_variance.cpp \
    astcenc/Source/astcenc_decompress_symbolic.cpp \
    astcenc/Source/astcenc_diagnostic_trace.cpp \
    astcenc/Source/astcenc_entry.cpp \
    astcenc/Source/astcenc_find_best_partitioning.cpp \
    astcenc/Source/astcenc_ideal_endpoints_and_weights.cpp \
    astcenc/Source/astcenc_image.cpp \
    astcenc/Source/astcenc_integer_sequence.cpp \
    astcenc/Source/astcenc_mathlib.cpp \
    astcenc/Source/astcenc_mathlib_softfloat.cpp \
    astcenc/Source/astcenc_partition_tables.cpp \
    astcenc/Source/astcenc_percentile_tables.cpp \
    astcenc/Source/astcenc_pick_best_endpoint_format.cpp \
    astcenc/Source/astcenc_quantization.cpp \
    astcenc/Source/astcenc_symbolic_physical.cpp \
    astcenc/Source/astcenc_weight_align.cpp \
    astcenc/Source/astcenc_weight_quant_xfer_tables.cpp
LOCAL_C_INCLUDES := $(LOCAL_PATH)/astcenc/Source
LOCAL_CPPFLAGS   := -std=c++14 -O2 -fno-exceptions -fno-rtti \
                    -DASTCENC_VECALIGN=16 -DASTCENC_ISA_NONE=1
include $(BUILD_STATIC_LIBRARY)

# ── QwSCTX JNI ────────────────────────────────────────────────────
include $(CLEAR_VARS)
LOCAL_MODULE    := qwsctx
LOCAL_SRC_FILES := \
    qwsctx_jni.cpp \
    sctx_parser.cpp \
    astc_decoder.cpp
LOCAL_C_INCLUDES := \
    $(LOCAL_PATH)/zstd/lib \
    $(LOCAL_PATH)/stb \
    $(LOCAL_PATH)/astcenc/Source
LOCAL_CPPFLAGS   := -std=c++17 -O2 -frtti -fexceptions -pthread
LOCAL_STATIC_LIBRARIES := zstd stb astcenc
LOCAL_LDLIBS     := -llog -landroid -pthread
include $(BUILD_SHARED_LIBRARY)

# ── SC Tool JNI ───────────────────────────────────────────────────
include $(CLEAR_VARS)
LOCAL_MODULE    := sctool
LOCAL_SRC_FILES := \
    sctool_jni.cpp          \
    sc_core.cpp             \
    sc_ops.cpp              \
    sc_ops_patch.cpp        \
    tex_ops.cpp             \
    png2sc_op.cpp           \
    sc2png_exports.cpp      \
    inject_impl.cpp         \
    mc_copy_impl.cpp        \
    sc_sprites_stub.cpp     \
    downgrade_jni.cpp       \
    sc_editor_jni.cpp      \
    sc2_reader.cpp          \
    sc2_replace.cpp         \
    gif_sc.cpp              \
    build_sc_impl.cpp      \
    sctx_parser.cpp         \
    astc_decoder.cpp
LOCAL_C_INCLUDES := \
    $(LOCAL_PATH)/zstd/lib \
    $(LOCAL_PATH)/stb \
    $(LOCAL_PATH)/astcenc/Source
LOCAL_CPPFLAGS   := -std=c++17 -O2 -frtti -fexceptions -pthread \
                    -DSCTOOL_JNI_CLASS=Java_com_compose_sctool_ScProcessor
LOCAL_STATIC_LIBRARIES := zstd stb astcenc
LOCAL_LDLIBS     := -llog -landroid -lz -ldl -pthread
include $(BUILD_SHARED_LIBRARY)

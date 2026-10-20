#pragma once
// SC2 (SC v5 / v6) reader.
//
// Container layout (confirmed against real sprays_68.sc byte-for-byte):
//   "SC" (2) + version:u32 LE (4)              -> pos=6
//   [v6 only: +2 bytes unknown, skip]           -> pos=8
//   descriptor_size:u32                         (Header FlatBuffers table size)
//   descriptor_size bytes: Header FlatBuffers table (NOT compressed)
//   zstd-compressed body, length = Header.compressed_size
//     (or "rest of file minus external_matrix_bank_size" if compressed_size==0)
//   [external_matrix_bank_size bytes, uncompressed, if nonzero]  -- not parsed here
//
// Decompressed body layout:
//   u32 data_storage_size + DataStorage FlatBuffers table (that many bytes)
//   ... then at ABSOLUTE offset Header.resources_offset (within body):
//   [u32 size + FlatBuffers table] x 6, in fixed order:
//     ExportNames, TextFields, Shapes, MovieClips, MovieClipModifiers, Textures
//
// Field indices below are 0-based FlatBuffers vtable slot indices, i.e. the
// declaration order of each table in the .fbs schema (sc2_schemas/*.fbs from
// sc-workshop/SupercellFlash). Only the tables/fields needed for viewing
// shapes/exports and for texture Replace are implemented; MovieClips/
// MovieClipModifiers/TextFields/MatrixBank content is skipped (but chunk
// sizes are still consumed so offsets stay correct).

#include <cstdint>
#include <string>
#include <vector>
#include "sc_core.h"

struct Sc2Vertex {
    float x = 0, y = 0;   // raw twips (float), same convention as SC1
    float u = 0, v = 0;   // normalized 0..1 (stored as u16/65535 in file)
};

struct Sc2Command {
    uint32_t texture_index = 0;
    std::vector<Sc2Vertex> vertices;   // fan, like SC1 TAG_SHAPE_DRAW_BITMAP_COMMAND
};

struct Sc2Shape {
    uint16_t id = 0;
    std::vector<Sc2Command> commands;
    std::string export_name;   // resolved via MovieClip export -> children_ids chain; may be empty
};

struct Sc2Export {
    uint16_t object_id = 0;
    std::string name;
};

struct Sc2MovieClip {
    uint16_t id = 0;
    bool has_export_name_ref = false;
    uint32_t export_name_ref_id = 0;      // index into Sc2File::strings, if has_export_name_ref
    std::vector<uint16_t> children_ids;   // ids of child DisplayObjects (Shape or nested MovieClip)
    // Анимация (используется sc2gif):
    uint8_t  framerate = 0;
    uint16_t frame_count = 0;
    std::vector<uint8_t> children_blending;     // режим наложения/маски для каждого ребёнка
    std::vector<uint32_t> frame_elem_counts;   // число элементов в каждом кадре
};

struct Sc2TextureData {
    bool     present = false;
    uint8_t  format = 0;       // bit_flags: 8 = KhronosTexture (KTX blob); sniff magic to be sure
    uint8_t  pixel_type = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    std::string external_path;  // set if texture is external (rare for single-file .sc)
    // data offset/len are relative to Sc2File::textures_chunk (owned storage)
    uint32_t data_off = 0;
    uint32_t data_len = 0;
};

struct Sc2TextureSet {
    Sc2TextureData highres;
    Sc2TextureData lowres;  // present=false if none
};

struct Sc2File {
    bool     valid = false;
    uint8_t  version = 0;                 // 5 or 6
    uint32_t translation_precision = 0;    // Header.fbs Precision enum
    uint32_t scale_precision = 0;

    uint32_t shape_count = 0, movieclip_count = 0, texture_count = 0, textfield_count = 0;

    std::vector<std::string>    strings;        // DataStorage.strings, index 0 = ""
    std::vector<uint8_t>        data_storage;    // owns the DataStorage flatbuffer (vertex buffer lives here)
    uint32_t vertex_buf_off = 0, vertex_buf_len = 0; // shapes_bitmap_poins location within data_storage

    // Из DataStorage (нужны для анимации): тройки u16 {child_index, matrix_index, color_index}
    // и банк матриц (по 6 float на матрицу, как лежат в файле).
    std::vector<uint16_t>       frame_elements;
    std::vector<float>          bank_matrices;
    std::vector<uint8_t>        bank_colors;      // по 7 байт на цвет-трансформ (как в файле)

    std::vector<Sc2Export>      exports;
    std::vector<Sc2Shape>       shapes;
    std::vector<uint16_t>       modifier_ids;     // MovieClipModifiers: id объекта
    std::vector<uint8_t>        modifier_types;   // 38 = маска, 39 = «в маске» (начало), 40 = конец маски
    std::vector<Sc2MovieClip>   movieclips;   // children_ids only; frames/matrices not parsed
    std::vector<uint8_t>        textures_chunk;  // owns the Textures flatbuffer (texture pixel blobs live here)
    std::vector<Sc2TextureSet>  textures;
};

// Returns true and fills `out` on success. On failure, *err (if non-null) gets a message.
bool sc2_parse(const std::vector<uint8_t>& file_data, Sc2File& out, std::string* err = nullptr);

// Convenience: normalized [minU,minV,maxU,maxV] bounding box across all commands of a shape.
// Returns false if the shape has no vertices.
bool sc2_shape_uv_bbox(const Sc2Shape& shape, float& min_u, float& min_v, float& max_u, float& max_v);

#include "applet_mitm_gbuf.hpp"
#include "applet_mitm_log.hpp"
#include <cstring>

namespace ams::mitm::applet {

    namespace {

        const char *ColorFormatName(u32 fmt) {
            /* NvColorFormat is a 64-bit-ish enum; the low word is what matters here.
             * Common surface formats seen on the Switch: */
            switch (fmt) {
                case 0x1: return "A8";
                case 0x2: return "L8";
                case 0xD5: return "A8B8G8R8";
                case 0xD6: return "A8R8G8B8";
                case 0xD7: return "X8B8G8R8";
                case 0xE5: return "R5G6B5";
                default:   return "?";
            }
        }

        const char *KindName(u32 kind) {
            switch (kind) {
                case 0x00: return "Pitch";
                case 0xFE: return "Generic_16BX2";
                case 0xDB: return "Z16";
                default:   return "?";
            }
        }

    }

    const NvGraphicBufferRaw *FindGraphicBuffer(const void *parcel, size_t size) {
        if (parcel == nullptr || size < sizeof(NvGraphicBufferRaw)) {
            return nullptr;
        }
        const u8 *p = static_cast<const u8 *>(parcel);
        const size_t magic_off = __builtin_offsetof(NvGraphicBufferRaw, magic);

        /* The flattened object is 4-byte aligned within the parcel. */
        for (size_t i = magic_off; i + (sizeof(NvGraphicBufferRaw) - magic_off) <= size; i += 4) {
            u32 magic;
            std::memcpy(std::addressof(magic), p + i, sizeof(magic));
            if (magic == NvGraphicBufferMagic) {
                return reinterpret_cast<const NvGraphicBufferRaw *>(p + i - magic_off);
            }
        }
        return nullptr;
    }

    void LogGraphicBuffer(const char *tag, const NvGraphicBufferRaw *gb) {
        LogLine("=== %s NvGraphicBuffer ===", tag);
        LogLine("    nvmap_id=%d  stride=%u px  total_size=%u B  num_planes=%u",
                gb->nvmap_id, gb->stride, gb->total_size, gb->num_planes);
        LogLine("    format=0x%x ext=0x%x usage=0x%x type=%u pid=%u",
                gb->format, gb->ext_format, gb->usage, gb->type, gb->pid);

        const u32 n = (gb->num_planes <= 3) ? gb->num_planes : 3;
        for (u32 i = 0; i < n; i++) {
            const NvSurfaceRaw *s = std::addressof(gb->planes[i]);
            LogLine("    plane[%u] %ux%u pitch=%u off=0x%x size=%llu",
                    i, s->width, s->height, s->pitch, s->offset,
                    static_cast<unsigned long long>(s->size));
            LogLine("             color_fmt=0x%x(%s) layout=%u kind=0x%x(%s) block_h_log2=%u scan=%u",
                    s->color_format, ColorFormatName(s->color_format),
                    s->layout, s->kind, KindName(s->kind),
                    s->block_height_log2, s->scan);
        }
    }

}

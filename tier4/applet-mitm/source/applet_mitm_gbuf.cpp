#include "applet_mitm_gbuf.hpp"
#include "applet_mitm_log.hpp"
#include <cstring>

namespace ams::mitm::applet {

    namespace {

        const char *ColorFormatName(u64 fmt) {
            switch (fmt) {
                case 0x0100532120ULL: return "A8B8G8R8";
                case 0x0100D12120ULL: return "A8R8G8B8";
                case 0x0100532020ULL: return "A2B10G10R10";
                case 0x0100531410ULL: return "A1B5G5R5";
                case 0x0000531110ULL: return "B5G6R5";
                case 0x0101240408ULL: return "A8";
                default:              return "?";
            }
        }

        const char *LayoutName(u32 l) {
            switch (l) {
                case 1:  return "Pitch";
                case 2:  return "Tiled";
                case 3:  return "BlockLinear";
                default: return "?";
            }
        }

        const char *KindName(u32 kind) {
            switch (kind) {
                case 0x00: return "Pitch";
                case 0xFE: return "Generic_16BX2";
                default:   return "?";
            }
        }

        const char *ScanName(u32 s) {
            switch (s) {
                case 0:  return "Progressive";
                case 1:  return "Interlaced";
                default: return "?";
            }
        }

    }

    const NvGraphicBufferRaw *FindGraphicBuffer(const void *parcel, size_t size) {
        if (parcel == nullptr || size < sizeof(NvGraphicBufferRaw)) {
            return nullptr;
        }
        const u8 *p = static_cast<const u8 *>(parcel);
        const size_t magic_off = __builtin_offsetof(NvGraphicBufferRaw, magic);

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
        LogLine("    nvmap_id=%d  stride=%u px  total_size=%u B  num_planes=%u  usage=0x%x  format=0x%x",
                gb->nvmap_id, gb->stride, gb->total_size, gb->num_planes, gb->usage, gb->format);

        const u32 n = (gb->num_planes <= 3) ? gb->num_planes : 3;
        for (u32 i = 0; i < n; i++) {
            const NvSurfaceRaw *s = std::addressof(gb->planes[i]);
            LogLine("    plane[%u] %ux%u  pitch=%u B  offset=0x%x  size=%llu B",
                    i, s->width, s->height, s->pitch, s->offset,
                    static_cast<unsigned long long>(s->size));
            LogLine("             fmt=0x%llx(%s)  layout=%u(%s)  kind=0x%x(%s)  block_h_log2=%u  scan=%u(%s)",
                    static_cast<unsigned long long>(s->color_format), ColorFormatName(s->color_format),
                    s->layout, LayoutName(s->layout),
                    s->kind, KindName(s->kind),
                    s->block_height_log2,
                    s->scan, ScanName(s->scan));
        }
    }

}

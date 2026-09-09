/*
 * T210 VIC 4.0 (class NVB0B6, host1x class 0x5D) configuration structs.
 *
 * Transcribed verbatim from libdrm tests/tegra/vic40.h
 * (Copyright (c) 2016-2018 NVIDIA Corporation, MIT). GCC aarch64 packs u64
 * bitfields LSB-first, matching the per-field bit comments in that header.
 *
 * sizeof(VicConfigStruct) must be 1552 (0x610); SET_CONTROL_PARAMS wants
 * (1552 / 16) << 16 = 97 << 16.
 */
#pragma once
#include <stratosphere.hpp>

namespace ams::mitm::applet::vic {

    /* method register byte-offsets (value pushed as method >> 2) */
    constexpr u32 SET_APPLICATION_ID              = 0x00000200;
    constexpr u32 EXECUTE                         = 0x00000300;
    constexpr u32 SET_SURFACE0_SLOT0_LUMA_OFFSET  = 0x00000400;
    constexpr u32 SET_CONTROL_PARAMS              = 0x00000704;
    constexpr u32 SET_CONFIG_STRUCT_OFFSET        = 0x00000708;
    constexpr u32 SET_OUTPUT_SURFACE_LUMA_OFFSET  = 0x00000720;

    constexpr u32 HOST1X_CLASS_VIC = 0x5D;
    /* nvhost "uclass" method regs for a VIC channel */
    constexpr u32 UCLASS_INCR_SYNCPT   = 0x00;
    constexpr u32 UCLASS_METHOD_OFFSET = 0x10;
    constexpr u32 UCLASS_METHOD_DATA   = 0x11;

    /* Declaring an increment in the submit's syncpt_incrs only tells nvhost to
     * raise the syncpoint's *max*. The increment itself must be programmed into
     * the command stream, or the syncpoint never reaches the fence - our wait
     * times out and every other client of that syncpoint (nvnflinger composites
     * on the VIC!) stalls forever. libdrm does this via
     * drm_tegra_pushbuf_sync_cond: NONINCR(0x0,1) then cond<<shift | syncpt_id.
     * VIC 4.0 reports version 0x21 -> cond_shift 8. */
    /* SETCL selects which engine's register space the following opcodes address.
     * libdrm omits it because the DRM kernel driver emits it from job->class;
     * nvservices' CHANNEL_SUBMIT may not, so we emit it ourselves. Harmless if
     * the class was already VIC. mask=0 means "set class, write no registers". */
    constexpr u32 Host1xOpcodeSetClass(u32 offset, u32 class_id, u32 mask) {
        return (UINT32_C(0) << 28) | ((offset & 0xFFF) << 16) | ((class_id & 0x3FF) << 6) | (mask & 0x3F);
    }

    constexpr u32 Host1xOpcodeNonIncr(u32 offset, u32 count) {
        return (UINT32_C(2) << 28) | ((offset & 0xFFF) << 16) | (count & 0xFFFF);
    }
    constexpr u32 INCR_SYNCPT_COND_IMMEDIATE = 0;
    constexpr u32 INCR_SYNCPT_COND_OP_DONE   = 1;
    constexpr u32 INCR_SYNCPT_COND_SHIFT     = 8;

    /* pixel formats (libdrm vic.h + VIC NVB0B6 table).
     * 32/33/34 are consecutive: A8R8G8B8, A8B8G8R8, R8G8B8A8. The game's
     * swapchain buffers are A8B8G8R8 (NvColorFormat 0x0100532120). */
    constexpr u32 PIXFMT_A8R8G8B8 = 32;
    constexpr u32 PIXFMT_A8B8G8R8 = 33;
    constexpr u32 PIXFMT_R8G8B8A8 = 34;
    /* block kinds */
    constexpr u32 BLK_KIND_PITCH        = 0;
    constexpr u32 BLK_KIND_GENERIC_16Bx2 = 1;
    /* cache widths */
    constexpr u32 CACHE_WIDTH_64Bx4 = 2;

    #pragma pack(push, 1)

    typedef struct {
        u64 SlotEnable : 1;
        u64 DeNoise : 1;
        u64 AdvancedDenoise : 1;
        u64 CadenceDetect : 1;
        u64 MotionMap : 1;
        u64 MMapCombine : 1;
        u64 IsEven : 1;
        u64 ChromaEven : 1;
        u64 CurrentFieldEnable : 1;
        u64 PrevFieldEnable : 1;
        u64 NextFieldEnable : 1;
        u64 NextNrFieldEnable : 1;
        u64 CurMotionFieldEnable : 1;
        u64 PrevMotionFieldEnable : 1;
        u64 PpMotionFieldEnable : 1;
        u64 CombMotionFieldEnable : 1;
        u64 FrameFormat : 4;
        u64 FilterLengthY : 2;
        u64 FilterLengthX : 2;
        u64 Panoramic : 12;
        u64 reserved1 : 22;
        u64 DetailFltClamp : 6;
        u64 FilterNoise : 10;
        u64 FilterDetail : 10;
        u64 ChromaNoise : 10;
        u64 ChromaDetail : 10;
        u64 DeinterlaceMode : 4;
        u64 MotionAccumWeight : 3;
        u64 NoiseIir : 11;
        u64 LightLevel : 4;
        u64 reserved4 : 2;
        u64 SoftClampLow : 10;
        u64 SoftClampHigh : 10;
        u64 reserved5 : 3;
        u64 reserved6 : 9;
        u64 PlanarAlpha : 10;
        u64 ConstantAlpha : 1;
        u64 StereoInterleave : 3;
        u64 ClipEnabled : 1;
        u64 ClearRectMask : 8;
        u64 DegammaMode : 2;
        u64 reserved7 : 1;
        u64 DecompressEnable : 1;
        u64 reserved9 : 5;
        u64 DecompressCtbCount : 8;
        u64 DecompressZbcColor : 32;
        u64 reserved12 : 24;
        u64 SourceRectLeft : 30;
        u64 reserved14 : 2;
        u64 SourceRectRight : 30;
        u64 reserved15 : 2;
        u64 SourceRectTop : 30;
        u64 reserved16 : 2;
        u64 SourceRectBottom : 30;
        u64 reserved17 : 2;
        u64 DestRectLeft : 14;
        u64 reserved18 : 2;
        u64 DestRectRight : 14;
        u64 reserved19 : 2;
        u64 DestRectTop : 14;
        u64 reserved20 : 2;
        u64 DestRectBottom : 14;
        u64 reserved21 : 2;
        u64 reserved22 : 32;
        u64 reserved23 : 32;
    } SlotConfig;

    typedef struct {
        u64 SlotPixelFormat : 7;
        u64 SlotChromaLocHoriz : 2;
        u64 SlotChromaLocVert : 2;
        u64 SlotBlkKind : 4;
        u64 SlotBlkHeight : 4;
        u64 SlotCacheWidth : 3;
        u64 reserved0 : 10;
        u64 SlotSurfaceWidth : 14;
        u64 SlotSurfaceHeight : 14;
        u64 reserved1 : 4;
        u64 SlotLumaWidth : 14;
        u64 SlotLumaHeight : 14;
        u64 reserved2 : 4;
        u64 SlotChromaWidth : 14;
        u64 SlotChromaHeight : 14;
        u64 reserved3 : 4;
    } SlotSurfaceConfig;

    typedef struct {
        u64 luma_coeff0 : 20; u64 luma_coeff1 : 20; u64 luma_coeff2 : 20; u64 luma_r_shift : 4;
        u64 luma_coeff3 : 20; u64 LumaKeyLower : 10; u64 LumaKeyUpper : 10; u64 LumaKeyEnabled : 1;
        u64 reserved0 : 2; u64 reserved1 : 21;
    } LumaKeyStruct;

    typedef struct {
        u64 matrix_coeff00 : 20; u64 matrix_coeff10 : 20; u64 matrix_coeff20 : 20; u64 matrix_r_shift : 4;
        u64 matrix_coeff01 : 20; u64 matrix_coeff11 : 20; u64 matrix_coeff21 : 20; u64 reserved0 : 3; u64 matrix_enable : 1;
        u64 matrix_coeff02 : 20; u64 matrix_coeff12 : 20; u64 matrix_coeff22 : 20; u64 reserved1 : 4;
        u64 matrix_coeff03 : 20; u64 matrix_coeff13 : 20; u64 matrix_coeff23 : 20; u64 reserved2 : 4;
    } MatrixStruct;

    typedef struct {
        u64 ClearRect0Left : 14; u64 reserved0 : 2; u64 ClearRect0Right : 14; u64 reserved1 : 2;
        u64 ClearRect0Top : 14; u64 reserved2 : 2; u64 ClearRect0Bottom : 14; u64 reserved3 : 2;
        u64 ClearRect1Left : 14; u64 reserved4 : 2; u64 ClearRect1Right : 14; u64 reserved5 : 2;
        u64 ClearRect1Top : 14; u64 reserved6 : 2; u64 ClearRect1Bottom : 14; u64 reserved7 : 2;
    } ClearRectStruct;

    typedef struct {
        u64 AlphaK1 : 10; u64 reserved0 : 6; u64 AlphaK2 : 10; u64 reserved1 : 6;
        u64 SrcFactCMatchSelect : 3; u64 reserved2 : 1; u64 DstFactCMatchSelect : 3; u64 reserved3 : 1;
        u64 SrcFactAMatchSelect : 3; u64 reserved4 : 1; u64 DstFactAMatchSelect : 3; u64 reserved5 : 1;
        u64 reserved6 : 4; u64 reserved7 : 4; u64 reserved8 : 4; u64 reserved9 : 4;
        u64 reserved10 : 2; u64 OverrideR : 10; u64 OverrideG : 10; u64 OverrideB : 10; u64 OverrideA : 10;
        u64 reserved11 : 2; u64 UseOverrideR : 1; u64 UseOverrideG : 1; u64 UseOverrideB : 1; u64 UseOverrideA : 1;
        u64 MaskR : 1; u64 MaskG : 1; u64 MaskB : 1; u64 MaskA : 1; u64 reserved12 : 12;
    } BlendingSlotStruct;

    typedef struct {
        u64 AlphaFillMode : 3; u64 AlphaFillSlot : 3; u64 BackgroundAlpha : 10;
        u64 BackgroundR : 10; u64 BackgroundG : 10; u64 BackgroundB : 10;
        u64 RegammaMode : 2; u64 OutputFlipX : 1; u64 OutputFlipY : 1; u64 OutputTranspose : 1;
        u64 reserved1 : 1; u64 reserved2 : 12;
        u64 TargetRectLeft : 14; u64 reserved3 : 2; u64 TargetRectRight : 14; u64 reserved4 : 2;
        u64 TargetRectTop : 14; u64 reserved5 : 2; u64 TargetRectBottom : 14; u64 reserved6 : 2;
    } OutputConfig;

    typedef struct {
        u64 OutPixelFormat : 7; u64 OutChromaLocHoriz : 2; u64 OutChromaLocVert : 2;
        u64 OutBlkKind : 4; u64 OutBlkHeight : 4; u64 reserved0 : 3; u64 reserved1 : 10;
        u64 OutSurfaceWidth : 14; u64 OutSurfaceHeight : 14; u64 reserved2 : 4;
        u64 OutLumaWidth : 14; u64 OutLumaHeight : 14; u64 reserved3 : 4;
        u64 OutChromaWidth : 14; u64 OutChromaHeight : 14; u64 reserved4 : 4;
    } OutputSurfaceConfig;

    typedef struct {
        u64 DownsampleHoriz : 11; u64 reserved0 : 5; u64 DownsampleVert : 11; u64 reserved1 : 5;
        u64 reserved2 : 32; u64 reserved3 : 32; u64 reserved4 : 32;
    } PipeConfig;

    typedef struct {
        SlotConfig slotConfig;
        SlotSurfaceConfig slotSurfaceConfig;
        LumaKeyStruct lumaKeyStruct;
        MatrixStruct colorMatrixStruct;
        MatrixStruct gamutMatrixStruct;
        BlendingSlotStruct blendingSlotStruct;
    } SlotStruct;

    typedef struct {
        PipeConfig pipeConfig;
        OutputConfig outputConfig;
        OutputSurfaceConfig outputSurfaceConfig;
        MatrixStruct outColorMatrixStruct;
        ClearRectStruct clearRectStruct[4];
        SlotStruct slotStruct[8];
    } VicConfigStruct;

    #pragma pack(pop)

    static_assert(sizeof(SlotConfig) == 64);
    static_assert(sizeof(SlotStruct) == 176);
    static_assert(sizeof(VicConfigStruct) == 1552);

}

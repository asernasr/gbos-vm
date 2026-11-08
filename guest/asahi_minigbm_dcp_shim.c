/*
 * Apple Silicon DCP (Display Co-Processor) + Asahi AGX minigbm Backend & Alignment Shim
 *
 * Loaded as the first DT_NEEDED library (libasahi_dcp_bm.so) of libminigbm_gralloc.so.
 *
 * Solves three bare-metal Apple Silicon (t8103 / M1) display & GPU buffer constraints:
 *  1. Registers the "apple" KMS DRM driver (/dev/dri/card0, apple-dcp) in minigbm's
 *     drv_create() backend table while skipping the render-only "asahi" node
 *     (/dev/dri/renderD128) during dumb-buffer allocator discovery so minigbm allocates
 *     scanout-capable DMA-BUF GEM objects on /dev/dri/card0 that Mesa's kmsro/asahi_dri
 *     and Honeykrisp (libvulkan_asahi.so) import via PRIME FD.
 *  2. Restricts BO_USE_SCANOUT format combinations to Apple DCP hardware plane formats
 *     (XR24, AR24, XB24, AB24, XR30, AR30, XB30, AB30) with explicit DRM_FORMAT_MOD_LINEAR,
 *     preventing minigbm from selecting RGB565 (RG16) or 24-bit BGR888 (BG24) for scanout
 *     (which cause drmModeAddFB2WithModifiers in drm_hwcomposer to fail or drop frames).
 *  3. Enforces 128-byte (AIL_CACHELINE) pitch/stride alignment on all linear planes and
 *     16,384-byte (16 KB apple-dart IOMMU page) total buffer size alignment (+ 128-byte
 *     AGX tile prefetch guard) in both drv_bo_from_format_and_padding() and
 *     drv_dumb_bo_create_ex(), preventing DCP IOMMU faults and AGX linear stride mismatches.
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <link.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <android/log.h>

#define LOG_TAG "AsahiMinigbmDCP"
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define ALOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

/* Apple AGX cacheline (128B) & Apple DART / M1 page size (16 KiB) */
#define ASAHI_AIL_CACHELINE 128U
#define ASAHI_PAGE_SIZE     16384UL
#define ALIGN_UP(v, a)      (((v) + ((a) - 1)) & ~((typeof(v))(a) - 1))

/* FourCC helper */
#define FOURCC(a, b, c, d) \
    ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

#define DRM_FORMAT_R8               FOURCC('R', '8', ' ', ' ')
#define DRM_FORMAT_R16              FOURCC('R', '1', '6', ' ')
#define DRM_FORMAT_RG88             FOURCC('R', 'G', '8', '8')
#define DRM_FORMAT_RGB565           FOURCC('R', 'G', '1', '6')
#define DRM_FORMAT_BGR888           FOURCC('B', 'G', '2', '4')
#define DRM_FORMAT_ARGB8888         FOURCC('A', 'R', '2', '4')
#define DRM_FORMAT_XRGB8888         FOURCC('X', 'R', '2', '4')
#define DRM_FORMAT_ABGR8888         FOURCC('A', 'B', '2', '4')
#define DRM_FORMAT_XBGR8888         FOURCC('X', 'B', '2', '4')
#define DRM_FORMAT_ARGB2101010      FOURCC('A', 'R', '3', '0')
#define DRM_FORMAT_XRGB2101010      FOURCC('X', 'R', '3', '0')
#define DRM_FORMAT_ABGR2101010      FOURCC('A', 'B', '3', '0')
#define DRM_FORMAT_XBGR2101010      FOURCC('X', 'B', '3', '0')
#define DRM_FORMAT_ABGR16161616F    FOURCC('A', 'B', '4', 'H')
#define DRM_FORMAT_NV12             FOURCC('N', 'V', '1', '2')
#define DRM_FORMAT_NV21             FOURCC('N', 'V', '2', '1')
#define DRM_FORMAT_YVU420           FOURCC('Y', 'V', '1', '2')
#define DRM_FORMAT_YVU420_ANDROID   FOURCC('9', '9', '9', '7')
#define DRM_FORMAT_FLEX_IMPL        FOURCC('9', '9', '9', '8')
#define DRM_FORMAT_FLEX_YCbCr_420   FOURCC('9', '9', '9', '9')
#define DRM_FORMAT_P010             FOURCC('P', '0', '1', '0')

#define DRM_FORMAT_MOD_LINEAR       0ULL
#define DRM_FORMAT_MOD_INVALID      ((1ULL << 56) - 1)

/* minigbm BO_USE_* flags matching libminigbm_gralloc.so */
#define BO_USE_SCANOUT              (1ULL << 0)
#define BO_USE_CURSOR               (1ULL << 1)
#define BO_USE_CURSOR_64X64         BO_USE_CURSOR
#define BO_USE_RENDERING            (1ULL << 2)
#define BO_USE_LINEAR               (1ULL << 4)
#define BO_USE_SW_READ_NEVER        (1ULL << 5)
#define BO_USE_SW_READ_RARELY       (1ULL << 6)
#define BO_USE_SW_READ_OFTEN        (1ULL << 7)
#define BO_USE_SW_WRITE_NEVER       (1ULL << 8)
#define BO_USE_SW_WRITE_RARELY      (1ULL << 9)
#define BO_USE_SW_WRITE_OFTEN       (1ULL << 10)
#define BO_USE_EXTERNAL_DISP        (1ULL << 11)
#define BO_USE_PROTECTED            (1ULL << 12)
#define BO_USE_HW_VIDEO_ENCODER     (1ULL << 13)
#define BO_USE_CAMERA_WRITE         (1ULL << 14)
#define BO_USE_CAMERA_READ          (1ULL << 15)
#define BO_USE_TEXTURING            (1ULL << 16)
#define BO_USE_HW_VIDEO_DECODER     (1ULL << 17)
#define BO_USE_RENDERSCRIPT         (1ULL << 18)
#define BO_USE_GPU_DATA_BUFFER      (1ULL << 19)
#define BO_USE_SENSOR_DIRECT_DATA   (1ULL << 20)
#define BO_USE_FRONT_RENDERING      (1ULL << 21)

/* Standard masks used in libminigbm_gralloc.so (0xf530) */
#define ASAHI_RENDER_MASK           0x00031e30ULL
#define ASAHI_SCANOUT_RENDER_MASK   (ASAHI_RENDER_MASK | BO_USE_SCANOUT | BO_USE_CURSOR)
#define ASAHI_LINEAR_CAMERA_MASK    0x000c60c0ULL

/* DRM_IOCTL_MODE_CREATE_DUMB (0xc02064b2) */
struct drm_mode_create_dumb {
    uint32_t height;
    uint32_t width;
    uint32_t bpp;
    uint32_t flags;
    uint32_t handle;
    uint32_t pitch;
    uint64_t size;
};
#define DRM_IOCTL_MODE_CREATE_DUMB_NR 0xc02064b2UL

typedef struct _drmVersion {
    int version_major;
    int version_minor;
    int version_patchlevel;
    int name_len;
    char *name;
    int date_len;
    char *date;
    int desc_len;
    char *desc;
} drmVersion, *drmVersionPtr;

/* Exact struct bo layout verified from libminigbm_gralloc.so (0xd990..0xda60) */
struct driver;
union bo_handle {
    void *ptr;
    int32_t s32;
    uint32_t u32;
    int64_t s64;
    uint64_t u64;
};

struct bo {
    struct driver *drv;             /* +0x00 */
    struct {
        uint32_t width;             /* +0x08 */
        uint32_t height;            /* +0x0c */
        uint32_t format;            /* +0x10 */
        uint32_t tiling;            /* +0x14 */
        size_t num_planes;          /* +0x18 */
        uint32_t offsets[4];        /* +0x20 */
        uint32_t sizes[4];          /* +0x30 */
        uint32_t strides[4];        /* +0x40 */
        uint64_t format_modifier;   /* +0x50 */
        uint64_t use_flags;         /* +0x58 */
        size_t total_size;          /* +0x60 */
        uint8_t _pad[24];           /* +0x68..0x7f */
    } meta;
    uint8_t is_test_buffer;         /* +0x80 */
    uint8_t _pad2[7];               /* +0x81..0x87 */
    union bo_handle handles[4];     /* +0x88 */
};

struct format_metadata {
    uint32_t priority;
    uint32_t tiling;
    uint64_t modifier;
};

/* Exact struct backend layout (0xa0 = 160 bytes) verified from libminigbm_gralloc.so */
struct backend {
    char *name;                                                                 /* +0x00 */
    uint32_t vendor_id;                                                         /* +0x08 */
    uint32_t _pad0;                                                             /* +0x0c */
    int (*init)(struct driver *drv);                                            /* +0x10 */
    void (*close)(struct driver *drv);                                          /* +0x18 */
    int (*bo_create)(struct bo *bo, uint32_t width, uint32_t height,
                     uint32_t format, uint64_t use_flags);                      /* +0x20 */
    int (*bo_create_with_modifiers)(struct bo *bo, uint32_t width,
                                    uint32_t height, uint32_t format,
                                    const uint64_t *modifiers, uint32_t count); /* +0x28 */
    void *_reserved0[3];                                                        /* +0x30..0x40 */
    int (*bo_destroy)(struct bo *bo);                                           /* +0x48 */
    int (*bo_import)(struct bo *bo, void *data);                                /* +0x50 */
    int (*bo_export)(struct bo *bo, size_t plane);                              /* +0x58 */
    void *(*bo_map)(struct bo *bo, void *vma, uint32_t map_flags);              /* +0x60 */
    int (*bo_unmap)(struct bo *bo, void *vma);                                  /* +0x68 */
    int (*bo_invalidate)(struct bo *bo, void *mapping);                         /* +0x70 */
    int (*bo_flush)(struct bo *bo, void *mapping);                              /* +0x78 */
    void (*resolve_format_and_use_flags)(struct driver *drv, uint32_t format,
                                         uint64_t use_flags,
                                         uint32_t *out_format,
                                         uint64_t *out_use_flags);              /* +0x80 */
    size_t (*num_planes_from_modifier)(struct driver *drv, uint32_t format,
                                       uint64_t modifier);                      /* +0x88 */
    int (*resource_info)(struct bo *bo, uint32_t strides[4],
                         uint32_t offsets[4], uint64_t *format_modifier);       /* +0x90 */
    uint32_t (*get_max_texture_2d_size)(struct driver *drv);                    /* +0x98 */
};

/* Pointers to original libminigbm_gralloc.so and libdrm.so functions */
static struct driver *(*real_drv_create)(int fd, const struct backend *override_backend) = NULL;
static int (*real_drv_bo_from_format_and_padding)(struct bo *bo, uint32_t stride,
                                                  uint32_t vertical_subsampling,
                                                  uint32_t aligned_height,
                                                  uint32_t format,
                                                  uint32_t padding[4]) = NULL;
static int (*real_drv_dumb_bo_create_ex)(struct bo *bo, uint32_t width, uint32_t height,
                                         uint32_t format, uint64_t use_flags,
                                         uint64_t quirks) = NULL;
static int (*real_drv_dumb_bo_destroy)(struct bo *bo) = NULL;
static int (*real_drv_prime_bo_import)(struct bo *bo, void *data) = NULL;
static int (*real_drv_prime_bo_export)(struct bo *bo, size_t plane) = NULL;
static void *(*real_drv_dumb_bo_map)(struct bo *bo, void *vma, uint32_t map_flags) = NULL;
static int (*real_drv_bo_munmap)(struct bo *bo, void *vma) = NULL;
static void (*real_drv_resolve_format_and_use_flags_helper)(struct driver *drv,
                                                            uint32_t format,
                                                            uint64_t use_flags,
                                                            uint32_t *out_format,
                                                            uint64_t *out_use_flags) = NULL;
static void (*real_drv_add_combinations)(struct driver *drv, const uint32_t *formats,
                                         uint32_t num_formats,
                                         struct format_metadata *metadata,
                                         uint64_t use_flags) = NULL;
static void (*real_drv_modify_combination)(struct driver *drv, uint32_t format,
                                           struct format_metadata *metadata,
                                           uint64_t use_flags) = NULL;
static int (*real_drv_modify_linear_combinations)(struct driver *drv) = NULL;
static drmVersionPtr (*real_drmGetVersion)(int fd) = NULL;
static void (*real_drmFreeVersion)(drmVersionPtr v) = NULL;
static int (*real_drmIoctl)(int fd, unsigned long request, void *arg) = NULL;

static void resolve_symbols(void);

static inline uint32_t format_bpp(uint32_t format) {
    switch (format) {
        case DRM_FORMAT_ABGR16161616F:
            return 64;
        case DRM_FORMAT_ARGB8888:
        case DRM_FORMAT_XRGB8888:
        case DRM_FORMAT_ABGR8888:
        case DRM_FORMAT_XBGR8888:
        case DRM_FORMAT_ARGB2101010:
        case DRM_FORMAT_XRGB2101010:
        case DRM_FORMAT_ABGR2101010:
        case DRM_FORMAT_XBGR2101010:
            return 32;
        case DRM_FORMAT_BGR888:
            return 24;
        case DRM_FORMAT_RGB565:
        case DRM_FORMAT_R16:
        case DRM_FORMAT_RG88:
        case DRM_FORMAT_P010:
            return 16;
        default:
            return 8;
    }
}

static inline bool is_apple_dcp_scanout_format(uint32_t format) {
    switch (format) {
        case DRM_FORMAT_ARGB8888:
        case DRM_FORMAT_XRGB8888:
        case DRM_FORMAT_ABGR8888:
        case DRM_FORMAT_XBGR8888:
        case DRM_FORMAT_ARGB2101010:
        case DRM_FORMAT_XRGB2101010:
        case DRM_FORMAT_ABGR2101010:
        case DRM_FORMAT_XBGR2101010:
            return true;
        default:
            return false;
    }
}

/*
 * Enforce 128-byte (AIL_CACHELINE) pitch alignment on all planes and
 * 16 KiB (ASAHI_PAGE_SIZE) total buffer size alignment on struct bo.
 */
static void enforce_asahi_dcp_bo_layout(struct bo *bo, uint32_t aligned_height) {
    if (!bo || bo->meta.num_planes == 0) {
        return;
    }
    uint32_t h = aligned_height ? aligned_height : bo->meta.height;
    h = ALIGN_UP(h, 2U);

    size_t running_offset = 0;
    for (size_t p = 0; p < bo->meta.num_planes && p < 4; p++) {
        uint32_t stride = bo->meta.strides[p];
        uint32_t min_align = (p == 0) ? ASAHI_AIL_CACHELINE : 64U;
        /* For YUV 4:2:0 subplanes, aligning plane 0 to 128B guarantees UV is 64B-aligned */
        stride = ALIGN_UP(stride, min_align);
        bo->meta.strides[p] = stride;

        uint32_t plane_h = (p == 0) ? h : ((h + 1U) / 2U);
        if (bo->meta.format == DRM_FORMAT_R8 && bo->meta.height == 1) {
            plane_h = 1;
        }
        uint32_t plane_size = ALIGN_UP(stride * plane_h, ASAHI_AIL_CACHELINE);
        if (bo->meta.sizes[p] > plane_size) {
            plane_size = ALIGN_UP(bo->meta.sizes[p], ASAHI_AIL_CACHELINE);
        }
        running_offset = ALIGN_UP(running_offset, (size_t)ASAHI_AIL_CACHELINE);
        bo->meta.offsets[p] = (uint32_t)running_offset;
        bo->meta.sizes[p] = plane_size;
        running_offset += plane_size;
    }

    /* Add 128B AGX tile-prefetch safety margin and align to 16 KiB DART IOMMU page */
    size_t total = ALIGN_UP(running_offset + ASAHI_AIL_CACHELINE, ASAHI_PAGE_SIZE);
    if (bo->meta.total_size < total) {
        bo->meta.total_size = total;
    } else {
        bo->meta.total_size = ALIGN_UP(bo->meta.total_size, ASAHI_PAGE_SIZE);
    }
    /* Explicit linear modifier for Apple DCP / drm_hwcomposer scanout */
    if (bo->meta.format_modifier == DRM_FORMAT_MOD_INVALID) {
        bo->meta.format_modifier = DRM_FORMAT_MOD_LINEAR;
    }
    bo->meta.tiling = 0;
}

int drv_bo_from_format_and_padding(struct bo *bo, uint32_t stride,
                                   uint32_t vertical_subsampling,
                                   uint32_t aligned_height,
                                   uint32_t format,
                                   uint32_t padding[4]) {
    resolve_symbols();
    /* Align base stride to 128 bytes (AIL_CACHELINE) before plane calculation */
    uint32_t aligned_stride = ALIGN_UP(stride, ASAHI_AIL_CACHELINE);
    int ret = real_drv_bo_from_format_and_padding(
        bo, aligned_stride, vertical_subsampling, aligned_height, format, padding);
    if (ret == 0 && bo) {
        enforce_asahi_dcp_bo_layout(bo, aligned_height);
    }
    return ret;
}

int drv_dumb_bo_create_ex(struct bo *bo, uint32_t width, uint32_t height,
                          uint32_t format, uint64_t use_flags, uint64_t quirks) {
    resolve_symbols();
    uint32_t bpp = format_bpp(format);
    /*
     * Ensure width * bpp is a multiple of 128 bytes (AIL_CACHELINE):
     *  - 32 bpp -> align width to 32 pixels (128 bytes)
     *  - 16 bpp -> align width to 64 pixels (128 bytes)
     *  -  8 bpp -> align width to 128 pixels (128 bytes, so UV subplanes are 64B-aligned)
     */
    uint32_t pixel_align = 64U;
    if (bpp == 32) {
        pixel_align = 32U;
    } else if (bpp == 16) {
        pixel_align = 64U;
    } else if (bpp == 8) {
        pixel_align = 128U;
    }
    uint32_t aligned_w = (height == 1 && format == DRM_FORMAT_R8)
                             ? ALIGN_UP(width, ASAHI_AIL_CACHELINE)
                             : ALIGN_UP(width, pixel_align);
    uint32_t aligned_h = (height == 1 && format == DRM_FORMAT_R8)
                             ? 1U
                             : ALIGN_UP(height, 16U);

    int ret = real_drv_dumb_bo_create_ex(bo, aligned_w, aligned_h, format, use_flags, quirks);
    if (ret == 0 && bo) {
        enforce_asahi_dcp_bo_layout(bo, aligned_h);
    }
    return ret;
}

static int apple_dcp_bo_create(struct bo *bo, uint32_t width, uint32_t height,
                               uint32_t format, uint64_t use_flags) {
    return drv_dumb_bo_create_ex(bo, width, height, format, use_flags, 0);
}

static int apple_dcp_bo_create_with_modifiers(struct bo *bo, uint32_t width,
                                              uint32_t height, uint32_t format,
                                              const uint64_t *modifiers,
                                              uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        if (modifiers[i] == DRM_FORMAT_MOD_LINEAR ||
            modifiers[i] == DRM_FORMAT_MOD_INVALID) {
            int ret = drv_dumb_bo_create_ex(bo, width, height, format, BO_USE_SCANOUT, 0);
            if (ret == 0 && bo) {
                bo->meta.format_modifier = DRM_FORMAT_MOD_LINEAR;
            }
            return ret;
        }
    }
    return -EINVAL;
}

static void apple_dcp_resolve_format_and_use_flags(struct driver *drv,
                                                   uint32_t format,
                                                   uint64_t use_flags,
                                                   uint32_t *out_format,
                                                   uint64_t *out_use_flags) {
    resolve_symbols();
    /*
     * Map flexible and implementation-defined Android formats cleanly for
     * Apple DCP scanout vs camera/video pipelines.
     */
    if (format == DRM_FORMAT_FLEX_IMPL) {
        if (use_flags & (BO_USE_CAMERA_WRITE | BO_USE_CAMERA_READ |
                         BO_USE_HW_VIDEO_ENCODER | BO_USE_HW_VIDEO_DECODER)) {
            *out_format = DRM_FORMAT_NV12;
            *out_use_flags = (use_flags & ~BO_USE_SCANOUT) | BO_USE_LINEAR;
            return;
        }
        /* Default scanout/GPU implementation-defined format on Apple DCP */
        *out_format = DRM_FORMAT_ABGR8888;
        *out_use_flags = use_flags;
        return;
    }
    if (format == DRM_FORMAT_FLEX_YCbCr_420) {
        *out_format = DRM_FORMAT_NV12;
        *out_use_flags = (use_flags & ~BO_USE_SCANOUT) | BO_USE_LINEAR;
        return;
    }
    /*
     * Apple DCP primary scanout plane does not accept RGB565 or 24-bit BGR888.
     * Strip BO_USE_SCANOUT on those formats so SurfaceFlinger/drm_hwcomposer
     * composites them into an ABGR8888/XRGB8888 scanout target instead of
     * failing drmModeAddFB2WithModifiers.
     */
    if ((use_flags & BO_USE_SCANOUT) && !is_apple_dcp_scanout_format(format)) {
        use_flags &= ~BO_USE_SCANOUT;
    }
    real_drv_resolve_format_and_use_flags_helper(drv, format, use_flags,
                                                 out_format, out_use_flags);
    if ((*out_use_flags & BO_USE_SCANOUT) && !is_apple_dcp_scanout_format(*out_format)) {
        *out_use_flags &= ~BO_USE_SCANOUT;
    }
}

static int apple_dcp_driver_init(struct driver *drv) {
    resolve_symbols();
    struct format_metadata linear_meta = {
        .priority = 1,
        .tiling = 0,
        .modifier = DRM_FORMAT_MOD_LINEAR,
    };

    /* 1. Formats supported directly by Apple DCP scanout planes + Asahi AGX */
    static const uint32_t dcp_scanout_formats[] = {
        DRM_FORMAT_ARGB8888,
        DRM_FORMAT_XRGB8888,
        DRM_FORMAT_ABGR8888,
        DRM_FORMAT_XBGR8888,
        DRM_FORMAT_ARGB2101010,
        DRM_FORMAT_XRGB2101010,
        DRM_FORMAT_ABGR2101010,
        DRM_FORMAT_XBGR2101010,
    };
    real_drv_add_combinations(drv, dcp_scanout_formats,
                              sizeof(dcp_scanout_formats) / sizeof(dcp_scanout_formats[0]),
                              &linear_meta, ASAHI_SCANOUT_RENDER_MASK);

    /* 2. Non-scanout render/texture/video formats (composited by GPU/HWC) */
    static const uint32_t non_scanout_formats[] = {
        DRM_FORMAT_RGB565,
        DRM_FORMAT_BGR888,
        DRM_FORMAT_ABGR16161616F,
        DRM_FORMAT_R8,
        DRM_FORMAT_R16,
        DRM_FORMAT_RG88,
        DRM_FORMAT_NV12,
        DRM_FORMAT_NV21,
        DRM_FORMAT_YVU420,
        DRM_FORMAT_YVU420_ANDROID,
        DRM_FORMAT_P010,
    };
    real_drv_add_combinations(drv, non_scanout_formats,
                              sizeof(non_scanout_formats) / sizeof(non_scanout_formats[0]),
                              &linear_meta, ASAHI_RENDER_MASK);

    real_drv_modify_combination(drv, DRM_FORMAT_R8, &linear_meta,
                                ASAHI_LINEAR_CAMERA_MASK | BO_USE_GPU_DATA_BUFFER |
                                BO_USE_SENSOR_DIRECT_DATA);
    real_drv_modify_combination(drv, DRM_FORMAT_NV12, &linear_meta,
                                ASAHI_LINEAR_CAMERA_MASK | BO_USE_HW_VIDEO_DECODER |
                                BO_USE_HW_VIDEO_ENCODER);
    real_drv_modify_combination(drv, DRM_FORMAT_NV21, &linear_meta,
                                BO_USE_CAMERA_READ | BO_USE_CAMERA_WRITE);

    int ret = real_drv_modify_linear_combinations(drv);
    ALOGI("Initialized Apple DCP + Asahi AGX minigbm backend (128B pitch, 16KiB DART page)");
    return ret;
}

static struct backend backend_apple_dcp = {
    .name = "apple",
    .vendor_id = 0,
    .init = apple_dcp_driver_init,
    .close = NULL,
    .bo_create = apple_dcp_bo_create,
    .bo_create_with_modifiers = apple_dcp_bo_create_with_modifiers,
    .bo_destroy = NULL, /* filled in resolve_symbols() */
    .bo_import = NULL,
    .bo_export = NULL,
    .bo_map = NULL,
    .bo_unmap = NULL,
    .bo_invalidate = NULL,
    .bo_flush = NULL,
    .resolve_format_and_use_flags = apple_dcp_resolve_format_and_use_flags,
};

/*
 * Intercept drmIoctl to guarantee DRM_IOCTL_MODE_CREATE_DUMB allocations on
 * Apple DCP (/dev/dri/card0) always have 128-byte pitch alignment and 16 KiB
 * DART IOMMU page-aligned backing size.
 */
int drmIoctl(int fd, unsigned long request, void *arg) {
    resolve_symbols();
    if (request == DRM_IOCTL_MODE_CREATE_DUMB_NR && arg != NULL) {
        struct drm_mode_create_dumb *cd = (struct drm_mode_create_dumb *)arg;
        uint32_t bpp = cd->bpp ? cd->bpp : 32U;
        uint32_t bytes_per_pixel = (bpp + 7U) / 8U;
        uint32_t min_pitch = ALIGN_UP(cd->width * bytes_per_pixel, ASAHI_AIL_CACHELINE);
        if (bytes_per_pixel > 0) {
            cd->width = (min_pitch + bytes_per_pixel - 1U) / bytes_per_pixel;
        }
        uint64_t min_size = ALIGN_UP((uint64_t)min_pitch * (uint64_t)cd->height + ASAHI_AIL_CACHELINE,
                                     ASAHI_PAGE_SIZE);
        uint32_t needed_h = (uint32_t)((min_size + min_pitch - 1ULL) / min_pitch);
        if (cd->height < needed_h) {
            cd->height = needed_h;
        }
        int ret = real_drmIoctl(fd, request, arg);
        if (ret == 0) {
            cd->pitch = ALIGN_UP(cd->pitch, ASAHI_AIL_CACHELINE);
            cd->size = ALIGN_UP(cd->size, ASAHI_PAGE_SIZE);
        }
        return ret;
    }
    return real_drmIoctl(fd, request, arg);
}

struct driver *drv_create(int fd, const struct backend *override_backend) {
    resolve_symbols();
    if (!override_backend && real_drmGetVersion) {
        drmVersionPtr ver = real_drmGetVersion(fd);
        if (ver && ver->name) {
            if (strcmp(ver->name, "apple") == 0) {
                ALOGI("drv_create(fd=%d): binding Apple DCP KMS driver to backend_apple_dcp", fd);
                real_drmFreeVersion(ver);
                return real_drv_create(fd, &backend_apple_dcp);
            }
            if (strcmp(ver->name, "asahi") == 0) {
                /*
                 * /dev/dri/renderD128 ("asahi") is a render-only node without KMS dumb
                 * buffer ioctls. Check if /dev/dri/card0 ("apple") exists; if so, let
                 * cros_gralloc_driver fall through its renderD loop to /dev/dri/card0.
                 */
                if (access("/dev/dri/card0", R_OK | W_OK) == 0) {
                    ALOGI("drv_create(fd=%d): deferring render-only 'asahi' node to 'apple' card0", fd);
                    real_drmFreeVersion(ver);
                    return NULL;
                }
                ALOGW("drv_create(fd=%d): card0 unavailable; binding 'asahi' to backend_apple_dcp", fd);
                real_drmFreeVersion(ver);
                return real_drv_create(fd, &backend_apple_dcp);
            }
            real_drmFreeVersion(ver);
        } else if (ver) {
            real_drmFreeVersion(ver);
        }
    }
    return real_drv_create(fd, override_backend);
}

static int find_minigbm_phdr_cb(struct dl_phdr_info *info, size_t size, void *data) {
    (void)size;
    if (!info->dlpi_name) {
        return 0;
    }
    if (strstr(info->dlpi_name, "libminigbm_gralloc.so") != NULL) {
        *(uintptr_t *)data = (uintptr_t)info->dlpi_addr;
        return 1;
    }
    return 0;
}

static void resolve_symbols(void) {
    static pthread_mutex_t init_lock = PTHREAD_MUTEX_INITIALIZER;
    static volatile int initialized = 0;
    if (__atomic_load_n(&initialized, __ATOMIC_ACQUIRE)) {
        return;
    }
    pthread_mutex_lock(&init_lock);
    if (initialized) {
        pthread_mutex_unlock(&init_lock);
        return;
    }

    void *drm_handle = dlopen("libdrm.so", RTLD_NOW | RTLD_NOLOAD);
    if (!drm_handle) {
        drm_handle = dlopen("libdrm.so", RTLD_NOW);
    }
    if (drm_handle) {
        real_drmGetVersion = dlsym(drm_handle, "drmGetVersion");
        real_drmFreeVersion = dlsym(drm_handle, "drmFreeVersion");
        real_drmIoctl = dlsym(drm_handle, "drmIoctl");
    }

    uintptr_t base = 0;
    dl_iterate_phdr(find_minigbm_phdr_cb, &base);
    if (base != 0) {
        /* Exact verified offsets in libminigbm_gralloc.so */
        real_drv_create = (void *)(base + 0xc510UL);
        real_drv_bo_from_format_and_padding = (void *)(base + 0xe6a0UL);
        real_drv_dumb_bo_create_ex = (void *)(base + 0xe820UL);
        real_drv_dumb_bo_destroy = (void *)(base + 0xea30UL);
        real_drv_prime_bo_import = (void *)(base + 0xec40UL);
        real_drv_prime_bo_export = (void *)(base + 0xee50UL);
        real_drv_dumb_bo_map = (void *)(base + 0xef20UL);
        real_drv_bo_munmap = (void *)(base + 0xf040UL);
        real_drv_resolve_format_and_use_flags_helper = (void *)(base + 0xf370UL);
        real_drv_add_combinations = (void *)(base + 0xf0c0UL);
        real_drv_modify_combination = (void *)(base + 0xf170UL);
        real_drv_modify_linear_combinations = (void *)(base + 0xf1f0UL);

        backend_apple_dcp.bo_destroy = real_drv_dumb_bo_destroy;
        backend_apple_dcp.bo_import = real_drv_prime_bo_import;
        backend_apple_dcp.bo_export = real_drv_prime_bo_export;
        backend_apple_dcp.bo_map = real_drv_dumb_bo_map;
        backend_apple_dcp.bo_unmap = real_drv_bo_munmap;

        /* Patch libminigbm_gralloc.so GOT.PLT slots so internal calls are also intercepted */
        long page_sz = sysconf(_SC_PAGESIZE);
        if (page_sz <= 0) {
            page_sz = 16384;
        }
        uintptr_t got_page = (base + 0x1ce00UL) & ~((uintptr_t)page_sz - 1UL);
        if (mprotect((void *)got_page, (size_t)page_sz * 2, PROT_READ | PROT_WRITE) == 0) {
            *(void **)(base + 0x1ce58UL) = (void *)&drv_create;
            *(void **)(base + 0x1cfd0UL) = (void *)&drv_bo_from_format_and_padding;
            *(void **)(base + 0x1cfd8UL) = (void *)&drv_dumb_bo_create_ex;
            *(void **)(base + 0x1d028UL) = (void *)&drmIoctl;
        }
    }

    __atomic_store_n(&initialized, 1, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&init_lock);
}

__attribute__((constructor))
static void asahi_minigbm_dcp_ctor(void) {
    resolve_symbols();
}

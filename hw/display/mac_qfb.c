/*
 * "Qemu FrameBuffer" for Motorola 680x0 Macintosh.
 *                 Copyright (c) 2022 Solra Bizna
 *
 * large parts from QEMU Motorola 680x0 Macintosh Video Card Emulation.
 *                 Copyright (c) 2012-2018 Laurent Vivier
 *
 * some parts from QEMU G364 framebuffer Emulator.
 *                 Copyright (c) 2007-2011 Herve Poussineau
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

#include "qemu/datadir.h" /* for qemu_find_file */
#include "qemu/osdep.h"
#include "qemu/units.h"
#include "ui/console.h"
#include "ui/pixel_ops.h"
#include "hw/loader.h" /* for get_image_size */
#include "hw/nubus/nubus.h"
#include "hw/display/mac_qfb.h"
#include "qapi/error.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"

#define VIDEO_BASE          0x0
#define QFB_BASE            0x00C00000

#define QFB_VRAM_SIZE       (12 * MiB) /* enough for 2048x1536 at 32-bit */

#define QFB_VERSION         0x0 /* reads 'qfb0', writing this resets the QFB */
#define QFB_MODE_WIDTH      0x4 /* width in pixels */
#define QFB_MODE_HEIGHT     0x8 /* height in pixels */
#define QFB_MODE_DEPTH      0xC /* depth (one of 1, 2, 4, 8, 16, 24) */
#define QFB_MODE_PAGE       0x10 /* which "page" of VRAM is live */
#define QFB_MODE_STRIDE     0x14 /* not writable */
#define QFB_MODE_NUM_PAGES  0x18 /* not writable */
#define QFB_PAL_INDEX       0x1C /* index into palette */
#define QFB_PAL_COLOR       0x20 /* xxRRGGBB palette entry */
#define QFB_LUT_INDEX       0x24 /* index into gamma LUTs */
#define QFB_LUT_COLOR       0x28 /* xxRRGGBB gamma LUT entry */
#define QFB_IRQ_MASK        0x2C /* irq mask */
#define QFB_IRQ             0x30 /* read irq status, write irq ack */
#define QFB_CUSTOM_WIDTH    0x34 /* r/o: user-specified width, if any */
#define QFB_CUSTOM_HEIGHT   0x38 /* r/o: user-specified height, if any */
#define QFB_CUSTOM_DEPTH    0x3C /* user-specified depth, write = stderr */

#define QFB_IRQ_VBL         0x1

#define QFB_MAX_WIDTH       2048 /* QuickDraw gets very cranky any higher */
#define QFB_MAX_HEIGHT      1536

/* Vertical Blank period (60.00Hz) */
#define QFB_IRQ_VBL_PERIOD_NS 16666667

typedef void qfb_draw_line_func(QfbState *s, uint8_t *d, uint32_t addr,
                                int width);

static inline uint8_t qfb_read_byte(QfbState *s, uint32_t addr)
{
    return s->vram[addr % QFB_VRAM_SIZE];
}

/* 1-bit color */
static void qfb_draw_line1(QfbState *s, uint8_t *d, uint32_t addr,
                           int width)
{
    uint8_t r, g, b;
    int x;

    for (x = 0; x < width; x++) {
        int bit = x & 7;
        int idx = (qfb_read_byte(s, addr) >> (7 - bit)) & 1;
        r = s->palette_red[idx];
        g = s->palette_green[idx];
        b = s->palette_blue[idx];
        addr += (bit == 7);
        r = s->gamma_red[r];
        g = s->gamma_green[g];
        b = s->gamma_blue[b];

        *(uint32_t *)d = rgb_to_pixel32(r, g, b);
        d += 4;
    }
}

/* 2-bit color */
static void qfb_draw_line2(QfbState *s, uint8_t *d, uint32_t addr,
                           int width)
{
    uint8_t r, g, b;
    int x;

    for (x = 0; x < width; x++) {
        int bit = (x & 3);
        int idx = (qfb_read_byte(s, addr) >> ((3 - bit) << 1)) & 3;
        r = s->palette_red[idx];
        g = s->palette_green[idx];
        b = s->palette_blue[idx];
        addr += (bit == 3);
        r = s->gamma_red[r];
        g = s->gamma_green[g];
        b = s->gamma_blue[b];

        *(uint32_t *)d = rgb_to_pixel32(r, g, b);
        d += 4;
    }
}

/* 4-bit color */
static void qfb_draw_line4(QfbState *s, uint8_t *d, uint32_t addr,
                           int width)
{
    uint8_t r, g, b;
    int x;

    for (x = 0; x < width; x++) {
        int bit = x & 1;
        int idx = (qfb_read_byte(s, addr) >> ((1 - bit) << 2)) & 15;
        r = s->palette_red[idx];
        g = s->palette_green[idx];
        b = s->palette_blue[idx];
        addr += (bit == 1);
        r = s->gamma_red[r];
        g = s->gamma_green[g];
        b = s->gamma_blue[b];

        *(uint32_t *)d = rgb_to_pixel32(r, g, b);
        d += 4;
    }
}

/* 8-bit color */
static void qfb_draw_line8(QfbState *s, uint8_t *d, uint32_t addr,
                           int width)
{
    uint8_t r, g, b;
    int x;

    for (x = 0; x < width; x++) {
        int idx = qfb_read_byte(s, addr);
        r = s->palette_red[idx];
        g = s->palette_green[idx];
        b = s->palette_blue[idx];
        addr++;
        r = s->gamma_red[r];
        g = s->gamma_green[g];
        b = s->gamma_blue[b];

        *(uint32_t *)d = rgb_to_pixel32(r, g, b);
        d += 4;
    }
}

/* 16-bit color */
static void qfb_draw_line16(QfbState *s, uint8_t *d, uint32_t addr,
                            int width)
{
    uint8_t r, g, b;
    int x;

    for (x = 0; x < width; x++) {
        uint16_t pixel;
        pixel = (qfb_read_byte(s, addr) << 8) | qfb_read_byte(s, addr + 1);
        r = ((pixel >> 10) & 0x1f) << 3;
        r = r | (r >> 5);
        g = ((pixel >> 5) & 0x1f) << 3;
        g = g | (g >> 5);
        b = (pixel & 0x1f) << 3;
        b = b | (b >> 5);
        addr += 2;
        r = s->gamma_red[r];
        g = s->gamma_green[g];
        b = s->gamma_blue[b];

        *(uint32_t *)d = rgb_to_pixel32(r, g, b);
        d += 4;
    }
}

/* 24-bit color */
static void qfb_draw_line24(QfbState *s, uint8_t *d, uint32_t addr,
                            int width)
{
    uint8_t r, g, b;
    int x;

    for (x = 0; x < width; x++) {
        r = qfb_read_byte(s, addr + 1);
        g = qfb_read_byte(s, addr + 2);
        b = qfb_read_byte(s, addr + 3);
        addr += 4;
        r = s->gamma_red[r];
        g = s->gamma_green[g];
        b = s->gamma_blue[b];

        *(uint32_t *)d = rgb_to_pixel32(r, g, b);
        d += 4;
    }
}


enum {
    QFB_DRAW_LINE1,
    QFB_DRAW_LINE2,
    QFB_DRAW_LINE4,
    QFB_DRAW_LINE8,
    QFB_DRAW_LINE16,
    QFB_DRAW_LINE24,
    QFB_DRAW_LINE_NB,
};

static qfb_draw_line_func * const qfb_draw_line_table[QFB_DRAW_LINE_NB] = {
    qfb_draw_line1,
    qfb_draw_line2,
    qfb_draw_line4,
    qfb_draw_line8,
    qfb_draw_line16,
    qfb_draw_line24,
};

static int qfb_check_dirty(QfbState *s, DirtyBitmapSnapshot *snap,
                           ram_addr_t addr, int len)
{
    return memory_region_snapshot_get_dirty(&s->mem_vram, snap, addr, len);
}

static void qfb_draw_graphic(QfbState *s)
{
    DisplaySurface *surface = qemu_console_surface(s->con);
    DirtyBitmapSnapshot *snap = NULL;
    ram_addr_t page;
    uint32_t v = 0;
    int y, ymin;
    int qfb_stride = s->stride;
    qfb_draw_line_func *qfb_draw_line;

    switch (s->depth) {
    case 1:
        v = QFB_DRAW_LINE1;
        break;
    case 2:
        v = QFB_DRAW_LINE2;
        break;
    case 4:
        v = QFB_DRAW_LINE4;
        break;
    case 8:
        v = QFB_DRAW_LINE8;
        break;
    case 16:
        v = QFB_DRAW_LINE16;
        break;
    case 24:
        v = QFB_DRAW_LINE24;
        break;
    }

    qfb_draw_line = qfb_draw_line_table[v];
    assert(qfb_draw_line != NULL);

    snap = memory_region_snapshot_and_clear_dirty(&s->mem_vram, 0x0,
                                             memory_region_size(&s->mem_vram),
                                             DIRTY_MEMORY_VGA);

    ymin = -1;
    page = s->regs[QFB_MODE_PAGE >> 2] * s->stride * s->height;
    for (y = 0; y < s->height; y++, page += qfb_stride) {
        if (qfb_check_dirty(s, snap, page, qfb_stride)) {
            uint8_t *data_display;

            data_display = surface_data(surface) + y * surface_stride(surface);
            qfb_draw_line(s, data_display, page, s->width);

            if (ymin < 0) {
                ymin = y;
            }
        } else {
            if (ymin >= 0) {
                dpy_gfx_update(s->con, 0, ymin, s->width, y - ymin);
                ymin = -1;
            }
        }
    }

    if (ymin >= 0) {
        dpy_gfx_update(s->con, 0, ymin, s->width, y - ymin);
    }

    g_free(snap);
}

static void qfb_invalidate_display(void *opaque)
{
    QfbState *s = opaque;

    memory_region_set_dirty(&s->mem_vram, 0, QFB_VRAM_SIZE);
}

static uint32_t qfb_calculate_stride(uint32_t width, uint32_t depth) {
    /* start at 64 so that a two-register MOVEM will always be a safe way to
       blit, even at 1-bpp */
    uint32_t pot;
    for (pot = 64; pot < width; pot = pot << 1) {}

    switch (depth) {
    default:
    case 1:
        return pot / 8;
    case 2:
        return pot / 4;
    case 4:
        return pot / 2;
    case 8:
        return pot;
    case 16:
        return pot * 2;
    case 24:
    case 32:
        return pot * 4;
    }
}

static void qfb_update_mode(QfbState *s)
{
    uint32_t page, num_pages;
    s->regs[QFB_MODE_WIDTH >> 2] = s->width;
    s->regs[QFB_MODE_HEIGHT >> 2] = s->height;
    s->regs[QFB_MODE_STRIDE >> 2] = s->stride
        = qfb_calculate_stride(s->width, s->depth);
    s->regs[QFB_MODE_DEPTH >> 2] = s->depth;
    num_pages = QFB_VRAM_SIZE / (s->height * s->stride);
    if (num_pages > 32000) num_pages = 32000;
    s->regs[QFB_MODE_NUM_PAGES >> 2] = num_pages;
    page = s->regs[QFB_MODE_PAGE >> 2];
    if (page >= num_pages)
        s->regs[QFB_MODE_PAGE >> 2] = page = 0;
    qfb_invalidate_display(s);
}

static void qfb_update_display(void *opaque)
{
    QfbState *s = opaque;
    DisplaySurface *surface = qemu_console_surface(s->con);

    qemu_flush_coalesced_mmio_buffer();

    if (s->width == 0 || s->height == 0) {
        return;
    }

    if (s->width != surface_width(surface) ||
        s->height != surface_height(surface)) {
        qemu_console_resize(s->con, s->width, s->height);
        qfb_invalidate_display(s);
    }

    qfb_draw_graphic(s);
}

static void qfb_update_irq(QfbState *s)
{
    uint32_t irq_state = s->regs[QFB_IRQ >> 2] &
                         s->regs[QFB_IRQ_MASK >> 2];

    if (irq_state) {
        qemu_irq_raise(s->irq);
    } else {
        qemu_irq_lower(s->irq);
    }
}

static int64_t qfb_next_vbl(void)
{
    return (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + QFB_IRQ_VBL_PERIOD_NS) /
            QFB_IRQ_VBL_PERIOD_NS * QFB_IRQ_VBL_PERIOD_NS;
}

static void qfb_vbl_timer(void *opaque)
{
    QfbState *s = opaque;
    int64_t next_vbl;

    s->regs[QFB_IRQ >> 2] |= QFB_IRQ_VBL;
    qfb_update_irq(s);

    /* 60 Hz irq */
    next_vbl = qfb_next_vbl();
    timer_mod(s->vbl_timer, next_vbl);
}

static void qfb_reset(QfbState *s)
{
    int i;

    s->palette_current = 0;
    s->gamma_current = 0;
    for (i = 0; i < 256; i++) {
        s->palette_red[i] = 255 - i;
        s->palette_green[i] = 255 - i;
        s->palette_blue[i] = 255 - i;
        s->gamma_red[i] = i;
        s->gamma_green[i] = i;
        s->gamma_blue[i] = i;
    }
    memset(s->vram, 0, QFB_VRAM_SIZE);
    s->width = s->regs[QFB_CUSTOM_WIDTH >> 2];
    s->height = s->regs[QFB_CUSTOM_HEIGHT >> 2];
    s->depth = 1;
    s->regs[QFB_MODE_PAGE >> 2] = 0;
    qfb_update_mode(s);
}

static uint64_t qfb_ctrl_read(void *opaque,
                              hwaddr addr,
                              unsigned int size)
{
    QfbState *s = opaque;
    uint64_t val = 0;

    switch (addr) {
    case QFB_VERSION:
        val = 0x71666230; /* 'qfb0' */
        break;
    case QFB_PAL_COLOR:
        val = val | ((uint32_t)s->palette_red[s->palette_current] << 16);
        val = val | ((uint32_t)s->palette_green[s->palette_current] << 8);
        val = val | ((uint32_t)s->palette_blue[s->palette_current]);
        break;
    case QFB_LUT_COLOR:
        val = val | ((uint32_t)s->gamma_red[s->gamma_current] << 16);
        val = val | ((uint32_t)s->gamma_green[s->gamma_current] << 8);
        val = val | ((uint32_t)s->gamma_blue[s->gamma_current]);
        break;
    default:
        if (addr < QFB_CTRL_TOPADDR) {
            val = s->regs[addr >> 2];
        }
        else {
            val = 0xFFFFFFFF;
        }
        break;
    }

    return val;
}

static void qfb_ctrl_write(void *opaque,
                           hwaddr addr,
                           uint64_t val,
                           unsigned int size)
{
    QfbState *s = opaque;
    int64_t next_vbl;

    if (addr >= QFB_CTRL_TOPADDR) return;

    switch (addr) {
    case QFB_VERSION:
        qfb_reset(s);
        break;
    case QFB_MODE_WIDTH:
        s->width = (val > QFB_MAX_WIDTH ? QFB_MAX_WIDTH : val);
        qfb_update_mode(s);
        break;
    case QFB_MODE_HEIGHT:
        s->height = (val > QFB_MAX_HEIGHT ? QFB_MAX_HEIGHT : val);
        qfb_update_mode(s);
        break;
    case QFB_MODE_DEPTH:
        switch (val) {
        case 1: case 2: case 4: case 8: case 16: case 24:
            s->depth = val;
            break;
        default:
            s->depth = 1;
            break;
        }
        qfb_update_mode(s);
        break;
    case QFB_MODE_PAGE:
        s->regs[addr >> 2] = val;
        qfb_update_mode(s);
        break;
    case QFB_MODE_STRIDE: case QFB_MODE_NUM_PAGES:
        /* reject write */
        break;
    case QFB_PAL_INDEX:
        s->palette_current = val % 256;
        s->regs[addr >> 2] = s->palette_current;
        break;
    case QFB_PAL_COLOR:
        s->palette_red[s->palette_current] = (val >> 16) & 255;
        s->palette_green[s->palette_current] = (val >> 8) & 255;
        s->palette_blue[s->palette_current] = val & 255;
        qfb_invalidate_display(s);
        break;
    case QFB_LUT_INDEX:
        s->gamma_current = val % 256;
        s->regs[addr >> 2] = s->gamma_current;
        break;
    case QFB_LUT_COLOR:
        s->gamma_red[s->gamma_current] = (val >> 16) & 255;
        s->gamma_green[s->gamma_current] = (val >> 8) & 255;
        s->gamma_blue[s->gamma_current] = val & 255;
        qfb_invalidate_display(s);
        break;
    case QFB_IRQ_MASK:
        s->regs[addr >> 2] = val & QFB_IRQ_VBL;
        if (val & QFB_IRQ_VBL) {
            next_vbl = qfb_next_vbl();
            timer_mod(s->vbl_timer, next_vbl);
        } else {
            timer_del(s->vbl_timer);
        }
        break;
    case QFB_IRQ:
        s->regs[addr >> 2] = s->regs[addr >> 2] & ~val;
        qfb_update_irq(s);
        break;
    case QFB_CUSTOM_DEPTH:
        /* Support debug output. */
        if (val <= 255)
            fputc(val, stderr);
        break;
    default:
        /* ignore all other writes */
        break;
    }
}

static const MemoryRegionOps qfb_ctrl_ops = {
    .read = qfb_ctrl_read,
    .write = qfb_ctrl_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static int qfb_post_load(void *opaque, int version_id)
{
    qfb_update_mode(opaque);
    return 0;
}

static const VMStateDescription vmstate_qfb = {
    .name = "mac_qfb",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = qfb_post_load,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32(palette_current, QfbState),
        VMSTATE_UINT8_ARRAY(palette_red, QfbState, 256),
        VMSTATE_UINT8_ARRAY(palette_green, QfbState, 256),
        VMSTATE_UINT8_ARRAY(palette_blue, QfbState, 256),
        VMSTATE_UINT32(gamma_current, QfbState),
        VMSTATE_UINT8_ARRAY(gamma_red, QfbState, 256),
        VMSTATE_UINT8_ARRAY(gamma_green, QfbState, 256),
        VMSTATE_UINT8_ARRAY(gamma_blue, QfbState, 256),
        VMSTATE_UINT32(width, QfbState),
        VMSTATE_UINT32(height, QfbState),
        VMSTATE_UINT32(stride, QfbState),
        VMSTATE_UINT8(depth, QfbState),
        VMSTATE_UINT32_ARRAY(regs, QfbState, QFB_NUM_REGS),
        VMSTATE_TIMER_PTR(vbl_timer, QfbState),
        VMSTATE_END_OF_LIST()
    }
};

static const GraphicHwOps qfb_ops = {
    .invalidate = qfb_invalidate_display,
    .gfx_update = qfb_update_display,
};

static uint32_t qfb_calc_nubus_checksum(uint8_t* p, Int128 size)
{
    uint32_t ret = 0;
    while (size > 0) {
        ret = rol32(ret, 1) + *p++;
        --size;
    }
    return ret;
}

static inline uint32_t qfb_ptr_read_u32(uint8_t *p)
{
#if !HOST_BIG_ENDIAN
    return bswap32(*(uint32_t*)p);
#else
    return *(uint32_t*)p;
#endif
}

static inline uint16_t qfb_ptr_read_u16(uint8_t *p)
{
#if !HOST_BIG_ENDIAN
    return bswap16(*(uint16_t*)p);
#else
    return *(uint16_t*)p;
#endif
}

static inline void qfb_ptr_write_u32(uint8_t *p, uint32_t val)
{
#if !HOST_BIG_ENDIAN
    val = bswap32(val);
#endif
    *(uint32_t*)p = val;
}

static inline void qfb_ptr_write_u16(uint8_t *p, uint16_t val)
{
#if !HOST_BIG_ENDIAN
    val = bswap16(val);
#endif
    *(uint16_t*)p = val;
}

static void qfb_patch_vpblock(QfbState *ms, uint8_t *ptr, uint32_t offset, uint32_t size, uint32_t* out_depth)
{
    uint32_t depth;

    if (offset % 2 != 0 || offset + 46 > size)
    {
        fprintf(stderr, "warning: Skipped patching an element in the mac_qfb.rom because it had an invalid offset!\n");
        return;
    }
    if (qfb_ptr_read_u32(ptr + offset) != 46)
    {
        fprintf(stderr, "warning: Skipped patching an element in the mac_qfb.rom (0x%X) because it did not appear to be a VPBlock!\n", offset);
        return;
    }

    depth = qfb_ptr_read_u16(ptr + offset + 36);
    qfb_ptr_write_u16(ptr + offset + 8, qfb_calculate_stride(ms->width, depth));
    qfb_ptr_write_u16(ptr + offset + 14, ms->height);
    qfb_ptr_write_u16(ptr + offset + 16, ms->width);
    *out_depth = depth;
}

static void qfb_patch_sresource(QfbState *ms, uint8_t *ptr, uint32_t offset, uint32_t size)
{
    uint32_t depth;

    if (offset % 2 != 0 || offset + 8 > size)
    {
        fprintf(stderr, "warning: Skipped patching an element in the mac_qfb.rom because it had an invalid offset!\n");
        return;
    }
    if (ptr[offset] != 1 || ptr[offset+4] != 3)
    {
        fprintf(stderr, "warning: Skipped patching an element in the mac_qfb.rom (0x%X) because it did not appear to be a video parameter sResource!\n", offset);
        return;
    }

    qfb_patch_vpblock(ms, ptr, (offset + qfb_ptr_read_u32(ptr + offset)) & 0xFFFFFF, size, &depth);

    if(depth != 0)
        qfb_ptr_write_u32(ptr + offset + 4, ((QFB_VRAM_SIZE / ms->height / qfb_calculate_stride(ms->width, depth)) & 0xFFFFFF) | 0x03000000);
}

/* QuickDraw-compatible video cards must specify every resolution supported by
   the card in their NuBus declaration ROM. While individual "sResources" can
   be enabled, disabled, or deleted on the fly by drivers, there is no
   supported way to add new sResources or to modify the actual data stored
   inside them at runtime. Therefore, in order to support an arbitrary
   user-specified resolution in our device, we must set aside an sResource to
   be patched, by QEMU, with our chosen resolution.

   (note: none of this applies on later versions of the System Software, such
   as 8.1, which support the new "native driver" interface, and query the
   driver for all this information. It DOES apply to A/UX though, and that's
   what matters to me.) */
static void qfb_try_patch_decl_rom(MemoryRegion *rom, QfbState *ms)
{
    Int128 size;
    uint8_t *ptr;
    uint32_t checksum, offset, record_addr;

    ptr = memory_region_get_ram_ptr(rom);
    size = rom->size;
    if (size < 32)
        return; /* Much too small to be a useful declaration ROM. */
    else if (size % 4 != 0)
        return; /* things won't be aligned, back out! */

    /* Check that the ROM is a valid, Apple-format NuBus declaration ROM that
       uses all four byte lanes */
    if (qfb_ptr_read_u32(ptr + size - 6) != 0x5A932BC7 || ptr[size-8] != 1
        || ptr[size-7] != 1 || ptr[size-2] != 0 || ptr[size-1] != 0x0F)
    {
        fprintf(stderr, "warning: mac_qfb.rom has an invalid format header block\n");
        fprintf(stderr, "qfb_ptr_read_u32(ptr + size - 6) != 0x5A932BC7 = %i\n", (int)(qfb_ptr_read_u32(ptr + size - 6) != 0x5A932BC7));
        fprintf(stderr, "qfb_ptr_read_u32(ptr + size - 6) = 0x%08X\n", qfb_ptr_read_u32(ptr + size - 6));
        fprintf(stderr, "ptr[size-8] != 1 = %i\n", (int)(ptr[size-8] != 1));
        fprintf(stderr, "ptr[size-7] != 1 = %i\n", (int)(ptr[size-7] != 1));
        fprintf(stderr, "ptr[size-2] != 0 = %i\n", (int)(ptr[size-2] != 0));
        fprintf(stderr, "ptr[size-1] != 0x0F = %i\n", (int)(ptr[size-1] != 0x0F));
        fprintf(stderr, "%p\n", ptr);
        FILE* f = popen("hexdump -C", "w");
        fwrite(ptr, size, 1, f);
        fclose(f);
        return;
    }

    /* Check that the size in the format header matches the size of the actual
       image. */
    if (size != qfb_ptr_read_u32(ptr + size - 16))
    {
        fprintf(stderr, "warning: mac_qfb.rom has incorrect size in header\n");
        fprintf(stderr, "  (expected %u, got %u)\n", (unsigned)size, (unsigned)qfb_ptr_read_u32(ptr + size - 16));
        return;
    }

    /* Check that the ROM checksum is valid. */
    checksum = qfb_ptr_read_u32(ptr + size - 12);
    qfb_ptr_write_u32(ptr + size - 12, 0);
    if (checksum != qfb_calc_nubus_checksum(ptr, size))
    {
        fprintf(stderr, "warning: mac_qfb.rom has incorrect checksum in header\n");
        fprintf(stderr, "  (should be %08X, got %08X)\n", (unsigned)qfb_calc_nubus_checksum(ptr, size), (unsigned)checksum);
        /* but we may still attempt to patch it, and we'll even fix up the
           checksum when we're done so that Slot Manager will have a crack at
           your ROM! */
    }

    if (!memcmp(ptr + size - 28, "PatchMe!", 8))
    {
        offset = size - 32;
        while(offset > 0) {
            record_addr = qfb_ptr_read_u32(ptr + offset);
            if(record_addr == 0) break;
            qfb_patch_sresource(ms, ptr, record_addr, size);
            /* (safe to downcast size here because we've already checked it
               against the 32-bit size stored in the format header block) */
            offset -= 4;
        }
    }

    checksum = qfb_calc_nubus_checksum(ptr, size);
    qfb_ptr_write_u32(ptr + size - 12, checksum);
}

static bool qfb_common_realize(DeviceState *dev, QfbState *s, Error **errp)
{
    NubusDevice *nd = NUBUS_DEVICE(dev);
    DisplaySurface *surface;
    char *path;
    int64_t size;
    int fd;
    uint8_t *ptr;

    if (s->width < 32) s->width = 640;
    else if (s->width > QFB_MAX_WIDTH) s->width = QFB_MAX_WIDTH;
    if (s->height < 32) s->height = 480;
    else if (s->height > QFB_MAX_HEIGHT) s->height = QFB_MAX_HEIGHT;
    s->regs[QFB_CUSTOM_WIDTH >> 2] = s->width;
    s->regs[QFB_CUSTOM_HEIGHT >> 2] = s->height;
    switch(s->depth) {
    case 1: case 2: case 4: case 8: case 16: case 24:
        break;
    case 32:
        s->depth = 24;
        break;
    default:
        s->depth = 8;
        break;
    }
    s->regs[QFB_CUSTOM_DEPTH >> 2] = s->depth;

    s->con = graphic_console_init(dev, 0, &qfb_ops, s);
    surface = qemu_console_surface(s->con);

    if (surface_bits_per_pixel(surface) != 32) {
        error_setg(errp, "unknown host depth %d",
                   surface_bits_per_pixel(surface));
        return false;
    }

    /* Install the declaration ROM. (We can't use the support in nubus-device
       for this because we need to patch the ROM before the system boots, and
       QEMU's architecture makes that rather hard.) */
    path = qemu_find_file(QEMU_FILE_TYPE_BIOS, "mac_qfb.rom");
    if (path == NULL) {
        /* If we couldn't find the ROM in any of our paths, try the working
           directory. */
        path = g_strdup("mac_qfb.rom");
    }
    size = get_image_size(path);
    if (size < 0) {
        error_setg(errp, "failed to find romfile \"mac_qfb.rom\"\n");
    }
    else if (size == 0) {
        error_setg(errp, "\"mac_qfb.rom\" is empty\n");
    }
    else if (size > NUBUS_DECL_ROM_MAX_SIZE) {
        error_setg(errp, "\"mac_qfb.rom\" is too large\n");
    }
    else {
        fd = open(path, O_RDONLY | O_BINARY);
        if (fd < 0) {
            fprintf(stderr, "Couldn't open \"%s\": %s\n",
                    path, strerror(errno));
        }
        else {
            memory_region_init_rom(&nd->decl_rom, OBJECT(dev), "qfb-rom",
                                   size, &error_abort);
            ptr = memory_region_get_ram_ptr(&nd->decl_rom);
            if (read(fd, ptr, size) != size) {
                fprintf(stderr, "Couldn't read \"%s\": %s\n",
                        path, strerror(errno));
            }
            else {
                memory_region_add_subregion(&nd->slot_mem,
                                            NUBUS_SLOT_SIZE - size,
                                            &nd->decl_rom);
                qfb_try_patch_decl_rom(&nd->decl_rom, s);
            }
            close(fd);
        }
    }
    g_free(path);

    memory_region_init_io(&s->mem_ctrl, OBJECT(dev), &qfb_ctrl_ops, s,
                          "qfb-ctrl", QFB_CTRL_TOPADDR);

    memory_region_init_ram(&s->mem_vram, OBJECT(dev), "qfb-vram",
                           QFB_VRAM_SIZE, &error_abort);
    memory_region_set_log(&s->mem_vram, true, DIRTY_MEMORY_VGA);
    s->vram = memory_region_get_ram_ptr(&s->mem_vram);

    s->vbl_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, qfb_vbl_timer, s);
    qfb_update_mode(s);
    return true;
}

static void qfb_nubus_set_irq(void *opaque, int n, int level)
{
    QfbNubusState *s = NUBUS_QFB(opaque);
    NubusDevice *nd = NUBUS_DEVICE(s);

    nubus_set_irq(nd, level);
}

static void qfb_nubus_realize(DeviceState *dev, Error **errp)
{
    NubusDevice *nd = NUBUS_DEVICE(dev);
    QfbNubusState *s = NUBUS_QFB(dev);
    QfbNubusDeviceClass *ndc = NUBUS_QFB_GET_CLASS(dev);
    QfbState *ms = &s->qfb;

    ndc->parent_realize(dev, errp);
    if (*errp) {
        return;
    }

    if (!qfb_common_realize(dev, ms, errp)) {
        return;
    }

    memory_region_add_subregion(&nd->slot_mem, QFB_BASE, &ms->mem_ctrl);
    memory_region_add_subregion(&nd->slot_mem, VIDEO_BASE, &ms->mem_vram);

    ms->irq = qemu_allocate_irq(qfb_nubus_set_irq, s, 0);
}

static void qfb_nubus_unrealize(DeviceState *dev)
{
    QfbNubusState *s = NUBUS_QFB(dev);
    QfbNubusDeviceClass *ndc = NUBUS_QFB_GET_CLASS(dev);
    QfbState *ms = &s->qfb;

    ndc->parent_unrealize(dev);

    qemu_free_irq(ms->irq);
}

static void qfb_nubus_reset(DeviceState *d)
{
    QfbNubusState *s = NUBUS_QFB(d);
    QfbState *ms = &s->qfb;
    qfb_reset(ms);
}

static Property qfb_nubus_properties[] = {
    DEFINE_PROP_UINT32("width", QfbNubusState, qfb.width, 640),
    DEFINE_PROP_UINT32("height", QfbNubusState, qfb.height, 480),
    DEFINE_PROP_UINT8("depth", QfbNubusState, qfb.depth, 8),
    DEFINE_PROP_END_OF_LIST(),
};

static const VMStateDescription vmstate_qfb_nubus = {
    .name = "qfb-nubus",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_STRUCT(qfb, QfbNubusState, 1, vmstate_qfb, QfbState),
        VMSTATE_END_OF_LIST()
    }
};

static void qfb_nubus_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    QfbNubusDeviceClass *ndc = NUBUS_QFB_CLASS(klass);

    device_class_set_parent_realize(dc, qfb_nubus_realize,
                                    &ndc->parent_realize);
    device_class_set_parent_unrealize(dc, qfb_nubus_unrealize,
                                      &ndc->parent_unrealize);
    dc->desc = "Nubus \"Qemu FrameBuffer\" for Macintosh";
    dc->reset = qfb_nubus_reset;
    dc->vmsd = &vmstate_qfb_nubus;
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
    device_class_set_props(dc, qfb_nubus_properties);
}

static const TypeInfo qfb_nubus_info = {
    .name          = TYPE_NUBUS_QFB,
    .parent        = TYPE_NUBUS_DEVICE,
    .instance_size = sizeof(QfbNubusState),
    .class_init    = qfb_nubus_class_init,
    .class_size    = sizeof(QfbNubusDeviceClass),
};

static void qfb_register_types(void)
{
    type_register_static(&qfb_nubus_info);
}

type_init(qfb_register_types)

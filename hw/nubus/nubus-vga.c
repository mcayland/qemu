/*
 * QEMU Macintosh Nubus VGA card
 *
 * Copyright (c) 2024 Mark Cave-Ayland <mark.cave-ayland@ilande.co.uk>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * Implementation of VGA card for Nubus. The registers match the layout of
 * the QEMU PCI VGA card:
 *
 *   0x000: EDID blob
 *
 *   0x400-0x41f: VGA ioport registers (as located at 0x3c0 for ISA)
 *   0x500-0x515: Bochs VBE extension ioport
 */


#include "qemu/osdep.h"
#include "hw/nubus/nubus-vga.h"
#include "hw/display/vga_int.h"


static uint64_t nubus_vga_ioport_read(void *ptr, hwaddr addr,
                                      unsigned size)
{
    VGACommonState *s = ptr;
    uint64_t ret = 0;

    switch (size) {
    case 1:
        ret = vga_ioport_read(s, addr + 0x3c0);
        break;
    case 2:
        ret  = vga_ioport_read(s, addr + 0x3c0);
        ret |= vga_ioport_read(s, addr + 0x3c1) << 8;
        break;
    }
    return ret;
}

static void nubus_vga_ioport_write(void *ptr, hwaddr addr,
                                   uint64_t val, unsigned size)
{
    VGACommonState *s = ptr;

    switch (size) {
    case 1:
        vga_ioport_write(s, addr + 0x3c0, val);
        break;
    case 2:
        /*
         * Update bytes in little endian order.  Allows to update
         * indexed registers with a single word write because the
         * index byte is updated first.
         */
        vga_ioport_write(s, addr + 0x3c0, val & 0xff);
        vga_ioport_write(s, addr + 0x3c1, (val >> 8) & 0xff);
        break;
    }
}

static const MemoryRegionOps nubus_vga_ioport_ops = {
    .read = nubus_vga_ioport_read,
    .write = nubus_vga_ioport_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 2,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static uint64_t nubus_vga_bochs_read(void *ptr, hwaddr addr,
                                     unsigned size)
{
    VGACommonState *s = ptr;
    int index = addr >> 1;

    vbe_ioport_write_index(s, 0, index);
    return vbe_ioport_read_data(s, 0);
}

static void nubus_vga_bochs_write(void *ptr, hwaddr addr,
                                  uint64_t val, unsigned size)
{
    VGACommonState *s = ptr;
    int index = addr >> 1;

    vbe_ioport_write_index(s, 0, index);
    vbe_ioport_write_data(s, 0, val);
}

static const MemoryRegionOps nubus_vga_bochs_ops = {
    .read = nubus_vga_bochs_read,
    .write = nubus_vga_bochs_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 2,
    .impl.max_access_size = 2,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void nubus_vga_realize(DeviceState *dev, Error **errp)
{
    NubusVGADeviceClass *nvdc = NUBUS_VGA_DEVICE_GET_CLASS(dev);
    NubusVGADevice *s = NUBUS_VGA_DEVICE(dev);
    NubusDevice *nd = NUBUS_DEVICE(dev);

    nvdc->parent_realize(dev, errp);
    if (*errp) {
        return;
    }

    s->vga.vram_size_mb = NUBUS_SUPER_SLOT_SIZE / MiB;
    s->vga.bank_offset = 0;
    if (!vga_common_init(&s->vga, OBJECT(dev), errp)) {
        return;
    }

    /* VGA I/O registers */
    memory_region_init_io(&s->vga_ioport_mem, OBJECT(dev),
                          &nubus_vga_ioport_ops, s, "vga-ioport",
                          PCI_VGA_IOPORT_SIZE);
    memory_region_add_subregion(&nd->slot_mem, PCI_VGA_IOPORT_OFFSET,
                                &s->vga_ioport_mem);

    /* Bochs VBE registers */
    memory_region_init_io(&s->bochs_vbe_mem, OBJECT(dev), &nubus_vga_bochs_ops,
                          s, "vbe-ioport", PCI_VGA_BOCHS_SIZE);
    memory_region_add_subregion(&nd->slot_mem, PCI_VGA_BOCHS_OFFSET,
                                &s->bochs_vbe_mem);

    /* EDID */
    qemu_edid_generate(s->edid, sizeof(s->edid), &s->edid_info);
    qemu_edid_region_io(&s->edid_mem, OBJECT(dev), s->edid, sizeof(s->edid));
    memory_region_add_subregion(&nd->slot_mem, 0, &s->edid_mem);

    /* VRAM */
    s->vga.con = graphic_console_init(dev, 0, s->vga.hw_ops, &s->vga);
    memory_region_add_subregion(&nd->super_slot_mem, 0x0,
                                &s->vga.vram);
}

static void nubus_vga_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    NubusVGADeviceClass *nvdc = NUBUS_VGA_DEVICE_CLASS(oc);

    device_class_set_parent_realize(dc, nubus_vga_realize,
                                    &nvdc->parent_realize);
}

static const TypeInfo nubus_vga_types[] = {
    {
        .name = TYPE_NUBUS_VGA_DEVICE,
        .parent = TYPE_NUBUS_DEVICE,
        .instance_size = sizeof(NubusVGADevice),
        .class_init = nubus_vga_class_init,
        .class_size = sizeof(NubusVGADeviceClass),
    },
};

DEFINE_TYPES(nubus_vga_types)

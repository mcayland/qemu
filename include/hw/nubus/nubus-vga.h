/*
 * QEMU Macintosh Nubus VGA card
 *
 * Copyright (c) 2024 Mark Cave-Ayland <mark.cave-ayland@ilande.co.uk>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_NUBUS_VGA_H
#define HW_NUBUS_VGA_H

#include "hw/nubus/nubus.h"
#include "hw/display/vga_int.h"
#include "hw/display/edid.h"
#include "qom/object.h"


#define TYPE_NUBUS_VGA_DEVICE "nubus-vga"
OBJECT_DECLARE_TYPE(NubusVGADevice, NubusVGADeviceClass,
                    NUBUS_VGA_DEVICE)

struct NubusVGADeviceClass {
    DeviceClass parent_class;

    DeviceRealize parent_realize;
};

struct NubusVGADevice {
    NubusDevice parent_obj;

    VGACommonState vga;
    MemoryRegion vga_ioport_mem;
    MemoryRegion bochs_vbe_mem;
    MemoryRegion edid_mem;
    qemu_edid_info edid_info;
    uint8_t edid[384];
};

#endif

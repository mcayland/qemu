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

#ifndef MAC_QFB_H
#define MAC_QFB_H

#include "exec/memory.h"
#include "hw/irq.h"
#include "ui/console.h"
#include "qemu/timer.h"
#include "qom/object.h"

#define QFB_CTRL_TOPADDR  0x40
#define QFB_NUM_REGS      (QFB_CTRL_TOPADDR / sizeof(uint32_t))

typedef struct QfbState {
    MemoryRegion mem_vram;
    MemoryRegion mem_ctrl;
    QemuConsole *con;

    uint8_t *vram;
    uint32_t palette_current;
    uint8_t palette_red[256];
    uint8_t palette_green[256];
    uint8_t palette_blue[256];
    uint32_t gamma_current;
    uint8_t gamma_red[256];
    uint8_t gamma_green[256];
    uint8_t gamma_blue[256];
    uint32_t width, height; /* in pixels */
    uint32_t stride; /* in bytes */
    uint8_t depth;

    uint32_t regs[QFB_NUM_REGS];

    QEMUTimer *vbl_timer;
    qemu_irq irq;
} QfbState;

#define TYPE_NUBUS_QFB "nubus-qfb"
OBJECT_DECLARE_TYPE(QfbNubusState, QfbNubusDeviceClass, NUBUS_QFB)

struct QfbNubusDeviceClass {
    DeviceClass parent_class;

    DeviceRealize parent_realize;
    DeviceUnrealize parent_unrealize;
};


struct QfbNubusState {
    NubusDevice busdev;

    QfbState qfb;
};

#endif

/*
 * QEMU model of the G233 SPI Controller
 *
 * Copyright (c) 2025 Shengjie Lin 2874146120@qq.com
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_G233_SPI_H
#define HW_G233_SPI_H

#include "qemu/fifo8.h"
#include "hw/sysbus.h"

#define TYPE_G233_SPI "g233-spi"
#define G233_SPI(obj) OBJECT_CHECK(G233SPIState, (obj), TYPE_G233_SPI)

typedef struct G233SPIState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    SSIBus *ssi;
    qemu_irq irq;
    qemu_irq *cs_lines;

    uint32_t cr1;
    uint32_t cr2;
    uint32_t sr;
    uint32_t dr_tx;
    uint32_t dr_rx;
    uint32_t csctrl;
    uint32_t prev_csctrl;

    bool rx_fifo_has_data;
    int interrupt_count;
} G233SPIState;

#endif /* HW_G233_SPI_H */

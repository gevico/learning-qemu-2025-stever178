/*
 * QEMU model of the G233 SPI Controller
 *
 * Copyright (c) 2025 Learning QEMU 2025
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "hw/ssi/ssi.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/ssi/g233_spi.h"

/* Register offsets */
#define SPI_CR1     0x00
#define SPI_CR2     0x04
#define SPI_SR      0x08
#define SPI_DR      0x0C
#define SPI_CSCTRL  0x10

/* CR1 bits */
#define SPI_CR1_SPE     (1 << 6)   /* SPI Enable */
#define SPI_CR1_MSTR    (1 << 2)   /* Master mode */

/* CR2 bits */
#define SPI_CR2_RXNEIE  (1 << 6)   /* RX buffer not empty interrupt enable */
#define SPI_CR2_ERRIE   (1 << 5)   /* Error interrupt enable */
#define SPI_CR2_TXEIE   (1 << 7)   /* TX buffer empty interrupt enable */

/* SR bits */
#define SPI_SR_TXE      (1 << 1)   /* Transmit buffer empty */
#define SPI_SR_RXNE     (1 << 0)   /* Receive buffer not empty */
#define SPI_SR_UDR      (1 << 2)   /* Underrun flag */
#define SPI_SR_OVR      (1 << 3)   /* Overrun flag */
#define SPI_SR_BSY      (1 << 7)   /* Busy flag */

static void g233_spi_update_irq(G233SPIState *s)
{
    bool irq_state = false;

    /* RXNE interrupt */
    if ((s->cr2 & SPI_CR2_RXNEIE) && (s->sr & SPI_SR_RXNE)) {
        irq_state = true;
    }

    /* TXE interrupt */
    if ((s->cr2 & SPI_CR2_TXEIE) && (s->sr & SPI_SR_TXE)) {
        irq_state = true;
    }

    /* Error interrupt */
    if ((s->cr2 & SPI_CR2_ERRIE) && (s->sr & (SPI_SR_UDR | SPI_SR_OVR))) {
        irq_state = true;
    }

    if (irq_state) {
        s->interrupt_count++;
    }

    qemu_set_irq(s->irq, irq_state);
}

static uint64_t g233_spi_read(void *opaque, hwaddr addr, unsigned size)
{
    G233SPIState *s = G233_SPI(opaque);
    uint32_t ret = 0;

    switch (addr) {
    case SPI_CR1:
        ret = s->cr1;
        break;
    case SPI_CR2:
        ret = s->cr2;
        break;
    case SPI_SR:
        ret = s->sr;
        break;
    case SPI_DR:
        ret = s->dr_rx;
        /* Clear RXNE after reading */
        s->sr &= ~SPI_SR_RXNE;
        s->rx_fifo_has_data = false;
        g233_spi_update_irq(s);
        break;
    case SPI_CSCTRL:
        ret = s->csctrl;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "g233_spi_read: Bad offset 0x%lx\n", addr);
        break;
    }

    return ret;
}

static void g233_spi_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    G233SPIState *s = G233_SPI(opaque);

    switch (addr) {
    case SPI_CR1:
        s->cr1 = value;
        break;
    case SPI_CR2:
        s->cr2 = value;
        g233_spi_update_irq(s);
        break;
    case SPI_SR:
        /* Clear error flags */
        s->sr &= ~(value & (SPI_SR_UDR | SPI_SR_OVR));
        g233_spi_update_irq(s);
        break;
    case SPI_DR:
        // qemu_log_mask(LOG_TRACE, 
        //     "SPI: Write DR=0x%02X, CR1=0x%02X, CSCTRL=0x%02X\n",
        //     (uint8_t)value, s->cr1, s->csctrl);
        if (s->cr1 & SPI_CR1_SPE) {
            /* Check for overrun BEFORE starting transfer - if RX has unread data */
            bool overrun = (s->rx_fifo_has_data && (s->sr & SPI_SR_RXNE));

            s->dr_tx = value & 0xFF;
            s->sr &= ~SPI_SR_TXE;  /* Clear TXE */
            s->sr |= SPI_SR_BSY;   /* Set busy */

            /* Transfer byte through SSI bus */
            uint32_t rx = ssi_transfer(s->ssi, s->dr_tx);

            // qemu_log_mask(LOG_TRACE,
            //               "SPI: Transfer TX=0x%02X -> RX=0x%02X\n",
            //               s->dr_tx, rx & 0xFF);
            
            /* Now handle overrun AFTER transfer */
            if (overrun) {
                s->sr |= SPI_SR_OVR;  /* Set overrun flag */
            }

            s->dr_rx = rx & 0xFF;
            s->sr |= SPI_SR_TXE | SPI_SR_RXNE;  /* Set TXE and RXNE */
            s->sr &= ~SPI_SR_BSY;                /* Clear busy */
            s->rx_fifo_has_data = true;

            g233_spi_update_irq(s);
        }
        break;
    case SPI_CSCTRL:
        // qemu_log_mask(LOG_TRACE, 
        //     "SPI: Write CSCTRL=0x%02X (prev=0x%02X)\n",
        //     (uint32_t)value, s->csctrl);
        s->prev_csctrl = s->csctrl;
        s->csctrl = value;

        /* Control CS lines based on enable and active bits */
        /* CS0: bits 0 (enable) and 4 (active) */
        if (s->cs_lines && s->cs_lines[0]) {
            bool cs0_active = (value & 0x11) == 0x11;  /* Both EN and ACT set */
            qemu_set_irq(s->cs_lines[0], !cs0_active); /* CS is active low */
        }

        /* CS1: bits 1 (enable) and 5 (active) */
        if (s->cs_lines && s->cs_lines[1]) {
            bool cs1_active = (value & 0x22) == 0x22;  /* Both EN and ACT set */
            qemu_set_irq(s->cs_lines[1], !cs1_active); /* CS is active low */
        }
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "g233_spi_write: Bad offset 0x%lx\n", addr);
        break;
    }
}

static const MemoryRegionOps g233_spi_ops = {
    .read = g233_spi_read,
    .write = g233_spi_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void g233_spi_reset(Object *obj, ResetType type)
{
    G233SPIState *s = G233_SPI(obj);

    s->cr1 = 0;
    s->cr2 = 0;
    s->sr = 0x00000002;  /* 根据规格书：只有TXE位为1 */
    s->dr_tx = 0;
    s->dr_rx = 0;
    s->csctrl = 0;
    s->prev_csctrl = 0;
    s->rx_fifo_has_data = false;
    s->interrupt_count = 0;

    /* 取消断言所有CS线 */
    if (s->cs_lines) {
        for (int i = 0; i < 4; i++) {
            if (s->cs_lines[i]) {
                qemu_set_irq(s->cs_lines[i], 1);  /* 无效(高电平) */
            }
        }
    }
}

static void g233_spi_realize(DeviceState *dev, Error **errp)
{
    G233SPIState *s = G233_SPI(dev);

    s->ssi = ssi_create_bus(dev, "ssi");
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);

    /* Create CS lines for up to 4 devices */
    s->cs_lines = g_new0(qemu_irq, 4);
    qdev_init_gpio_out_named(dev, s->cs_lines, SSI_GPIO_CS, 4);

    memory_region_init_io(&s->iomem, OBJECT(s), &g233_spi_ops, s,
                          TYPE_G233_SPI, 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);
}

static const VMStateDescription vmstate_g233_spi = {
    .name = TYPE_G233_SPI,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(cr1, G233SPIState),
        VMSTATE_UINT32(cr2, G233SPIState),
        VMSTATE_UINT32(sr, G233SPIState),
        VMSTATE_UINT32(dr_tx, G233SPIState),
        VMSTATE_UINT32(dr_rx, G233SPIState),
        VMSTATE_UINT32(csctrl, G233SPIState),
        VMSTATE_END_OF_LIST()
    }
};

static void g233_spi_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->realize = g233_spi_realize;
    rc->phases.hold = g233_spi_reset;
    dc->vmsd = &vmstate_g233_spi;
}

static const TypeInfo g233_spi_info = {
    .name          = TYPE_G233_SPI,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(G233SPIState),
    .class_init    = g233_spi_class_init,
};

static void g233_spi_register_types(void)
{
    type_register_static(&g233_spi_info);
}

type_init(g233_spi_register_types)

/*
 * G233 Flash Device (W25X16/W25X32)
 * Based on m25p80.c but simplified for G233
 *
 * Copyright (c) 2025 Learning QEMU 2025
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/ssi/ssi.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"
#include "qemu/bitops.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qapi/error.h"

#define TYPE_G233_FLASH "g233-flash"
OBJECT_DECLARE_SIMPLE_TYPE(G233FlashState, G233_FLASH)

/* Flash commands */
#define CMD_JEDEC_ID    0x9F
#define CMD_READ        0x03
#define CMD_WREN        0x06
#define CMD_WRDI        0x04
#define CMD_RDSR        0x05
#define CMD_WRSR        0x01
#define CMD_PP          0x02  /* Page Program */
#define CMD_SE          0x20  /* Sector Erase */

/* Status register bits */
#define SR_WIP          0x01  /* Write In Progress */

typedef enum {
    STATE_IDLE,
    STATE_READING_CMD,
    STATE_READING_ID,
    STATE_READING_DATA,
    STATE_READING_SR,
    STATE_WRITING_SR,
    STATE_WRITING_DATA,
    STATE_ERASING,
} FlashState;

struct G233FlashState {
    SSIPeripheral parent_obj;

    BlockBackend *blk;
    uint8_t *storage;
    uint32_t size;
    uint8_t jedec_id[3];

    FlashState state;
    uint8_t cmd;
    uint32_t addr;
    int addr_bytes;
    int data_pos;

    uint8_t status_reg;
    bool write_enable;
    uint8_t page_buf[256];
    int page_pos;
};

static void g233_flash_reset(Object *obj, ResetType type)
{
    G233FlashState *s = G233_FLASH(obj);

    s->state = STATE_IDLE;
    s->cmd = 0;
    s->addr = 0;
    s->addr_bytes = 0;
    s->data_pos = 0;
    s->status_reg = 0;
    s->write_enable = false;
    s->page_pos = 0;
}

static uint32_t g233_flash_transfer(SSIPeripheral *ss, uint32_t tx)
{
    G233FlashState *s = G233_FLASH(ss);
    uint32_t rx = 0;


    switch (s->state) {
    case STATE_IDLE:
        s->cmd = tx;
        switch (tx) {
        case CMD_JEDEC_ID:
            s->state = STATE_READING_ID;
            s->data_pos = 0;
            break;
        case CMD_RDSR:
            s->state = STATE_READING_SR;
            break;
        case CMD_WREN:
            s->write_enable = true;
            break;
        case CMD_WRDI:
            s->write_enable = false;
            break;
        case CMD_READ:
            s->state = STATE_READING_CMD;
            s->addr = 0;
            s->addr_bytes = 0;
            break;
        case CMD_PP:  /* Page Program */
            if (s->write_enable) {
                s->state = STATE_READING_CMD;
                s->addr = 0;
                s->addr_bytes = 0;
                s->page_pos = 0;
            }
            break;
        case CMD_SE:  /* Sector Erase */
            if (s->write_enable) {
                s->state = STATE_READING_CMD;
                s->addr = 0;
                s->addr_bytes = 0;
            }
            break;
        }
        break;

    case STATE_READING_ID:
        if (s->data_pos < 3) {
            rx = s->jedec_id[s->data_pos++];
        } else {
            rx = 0xFF;
        }
        break;

    case STATE_READING_SR:
        rx = s->status_reg;
        s->state = STATE_IDLE;
        break;

    case STATE_READING_CMD:
        /* Collect 3-byte address */
        s->addr = (s->addr << 8) | tx;
        s->addr_bytes++;
        if (s->addr_bytes >= 3) {
            if (s->cmd == CMD_READ) {
                s->state = STATE_READING_DATA;
                s->data_pos = 0;
            } else if (s->cmd == CMD_PP) {
                s->state = STATE_WRITING_DATA;
            } else if (s->cmd == CMD_SE) {
                /* Erase 4KB sector */
                uint32_t sector_addr = s->addr & ~0xFFF;
                if (sector_addr < s->size) {
                    memset(s->storage + sector_addr, 0xFF, 4096);
                }
                s->write_enable = false;
                s->state = STATE_IDLE;
            }
        }
        break;

    case STATE_READING_DATA:
        if (s->addr + s->data_pos < s->size) {
            rx = s->storage[s->addr + s->data_pos];
        } else {
            rx = 0xFF;
        }
        s->data_pos++;
        break;

    case STATE_WRITING_DATA:
        if (s->page_pos < 256) {
            s->page_buf[s->page_pos] = tx;
            s->page_pos++;
        }
        break;

    default:
        s->state = STATE_IDLE;
        break;
    }

    return rx & 0xFF;
}

static void g233_flash_cs(void *opaque, int n, int level)
{
    fprintf(stderr, "!!!FLASH_CS_CALLED!!! n=%d, level=%d\n", n, level);
    G233FlashState *s = G233_FLASH(opaque);

    fprintf(stderr, "FLASH_CS: level=%d, state=%d, page_pos=%d\n", level, s->state, s->page_pos);

    if (level) {
        /* CS deasserted - complete operation */
        if (s->state == STATE_WRITING_DATA && s->page_pos > 0) {
            /* Write page buffer to flash - use the exact address from command */
            if (s->addr + s->page_pos <= s->size) {
                fprintf(stderr, "FLASH: Writing %d bytes to address 0x%x\n", s->page_pos, s->addr);
                fprintf(stderr, "FLASH: First 10 bytes: ");
                for (int i = 0; i < 10 && i < s->page_pos; i++) {
                    fprintf(stderr, "%02x ", s->page_buf[i]);
                }
                fprintf(stderr, "\n");
                memcpy(s->storage + s->addr, s->page_buf, s->page_pos);
                fprintf(stderr, "FLASH: Write complete, storage[0]=0x%02x\n", s->storage[s->addr]);
            }
            s->write_enable = false;
        }
        s->state = STATE_IDLE;
        s->data_pos = 0;
        s->page_pos = 0;
        s->addr = 0;
        s->addr_bytes = 0;
    }
}

static const VMStateDescription vmstate_g233_flash = {
    .name = TYPE_G233_FLASH,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(status_reg, G233FlashState),
        VMSTATE_BOOL(write_enable, G233FlashState),
        VMSTATE_END_OF_LIST()
    }
};

static void g233_flash_realize(SSIPeripheral *ss, Error **errp)
{
    G233FlashState *s = G233_FLASH(ss);
    DeviceState *dev = DEVICE(ss);

    /* Default to W25X16 (2MB) */
    if (!s->size) {
        s->size = 2 * 1024 * 1024;
    }

    /* Set JEDEC ID based on size */
    s->jedec_id[0] = 0xEF;  /* Winbond */
    s->jedec_id[1] = 0x30;  /* Memory type */
    if (s->size == 2 * 1024 * 1024) {
        s->jedec_id[2] = 0x15;  /* W25X16 */
    } else if (s->size == 4 * 1024 * 1024) {
        s->jedec_id[2] = 0x16;  /* W25X32 */
    }

    s->storage = g_malloc0(s->size);

    /* Initialize with 0xFF (erased state) */
    memset(s->storage, 0xFF, s->size);

    qdev_init_gpio_in_named(dev, g233_flash_cs, SSI_GPIO_CS, 1);
}

static void g233_flash_finalize(Object *obj)
{
    G233FlashState *s = G233_FLASH(obj);
    g_free(s->storage);
}

static const Property g233_flash_properties[] = {
    DEFINE_PROP_UINT32("size", G233FlashState, size, 0),
};

static int g233_flash_set_cs(SSIPeripheral *ss, bool select)
{
    G233FlashState *s = G233_FLASH(ss);

    if (!select) {
        /* CS deasserted (going high) - complete operation */
        if (s->state == STATE_WRITING_DATA && s->page_pos > 0) {
            /* Write page buffer to flash */
            if (s->addr + s->page_pos <= s->size) {
                memcpy(s->storage + s->addr, s->page_buf, s->page_pos);
            }
            s->write_enable = false;
        }
        s->state = STATE_IDLE;
        s->data_pos = 0;
        s->page_pos = 0;
        s->addr = 0;
        s->addr_bytes = 0;
    }

    return 0;
}

static void g233_flash_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    SSIPeripheralClass *k = SSI_PERIPHERAL_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    k->realize = g233_flash_realize;
    k->transfer = g233_flash_transfer;
    k->set_cs = g233_flash_set_cs;
    k->cs_polarity = SSI_CS_LOW;
    rc->phases.hold = g233_flash_reset;
    dc->vmsd = &vmstate_g233_flash;
    device_class_set_props(dc, g233_flash_properties);
}

static const TypeInfo g233_flash_info = {
    .name          = TYPE_G233_FLASH,
    .parent        = TYPE_SSI_PERIPHERAL,
    .instance_size = sizeof(G233FlashState),
    .instance_finalize = g233_flash_finalize,
    .class_init    = g233_flash_class_init,
};

static void g233_flash_register_types(void)
{
    type_register_static(&g233_flash_info);
}

type_init(g233_flash_register_types)

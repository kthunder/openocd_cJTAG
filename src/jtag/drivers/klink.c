// SPDX-License-Identifier: GPL-2.0-or-later

/***************************************************************************
 *   Copyright (C) 2024              xx                                    *
 *   xxxxx@xxx.xx                                                          *
 ***************************************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#if IS_CYGWIN == 1
#include "windows.h"
#undef LOG_ERROR
#endif

/* project specific includes */
#include <jtag/adapter.h>
#include <jtag/interface.h>
#include <jtag/commands.h>
#include <helper/time_support.h>
#include "libusb_helper.h"

/* system includes */
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/time.h>
#include <time.h>
#include <stdio.h>
#include <stdint.h>

extern int klink_usb_open(uint16_t vid, uint16_t pid);
extern int klink_read_message(char *buffer, size_t length);
extern int klink_send_message(uint8_t opcode, uint16_t bits, uint8_t *ucData);

typedef struct
{
    uint8_t head;
    uint8_t len;
    uint8_t padding1;
    uint8_t padding2;
    uint8_t opcode;
    uint8_t opcode_ex;
    uint16_t bits;
    uint8_t data[];
} message;

static int klink_init(void)
{
    // log_set_level(LOG_WARN);
    // log_debug("klink_init");
#ifdef _WIN32
    const char *port = NULL;
#else
    const char *port = "/dev/ttyACM0";
#endif

    int ret = klink_usb_open(0x1D50, 0x60AC);
    LOG_DEBUG("klink_usb_open ret:%d", ret);
    klink_send_message(0xFF, 0, NULL);
    return ERROR_OK;
}

COMMAND_HANDLER(klink_handle_hello_command)
{
    printf("%s", __func__);
    return ERROR_OK;
}

static const struct command_registration klink_subcommand_handlers[] = {
    {
        .name = "hello",
        .handler = &klink_handle_hello_command,
        .mode = COMMAND_ANY,
        .help = "USB VID and PID of the adapter",
        .usage = "vid pid",
    },
    COMMAND_REGISTRATION_DONE};

static const struct command_registration klink_command_handlers[] = {
    {
        .name = "klink",
        .mode = COMMAND_ANY,
        .help = "perform klink management",
        .chain = klink_subcommand_handlers,
        .usage = "",
    },
    COMMAND_REGISTRATION_DONE};

/* Set new end state */
static void klink_end_state(enum tap_state state)
{
    if (tap_is_state_stable(state))
        tap_set_end_state(state);
    else
    {
        LOG_ERROR("BUG: %i is not a valid end state", state);
        exit(-1);
    }
}

/* Move to the end state by queuing a sequence to clock into TMS */
static void klink_state_move(void)
{
    uint8_t tms_scan = tap_get_tms_path(tap_get_state(), tap_get_end_state());
    uint8_t tms_scan_bits = tap_get_tms_path_len(tap_get_state(), tap_get_end_state());

    LOG_DEBUG_IO("state move from %s to %s: %d clocks, %02X on tms",
                 tap_state_name(tap_get_state()), tap_state_name(tap_get_end_state()),
                 tms_scan_bits, tms_scan);
    klink_send_message(1, tms_scan_bits, &tms_scan);

    tap_set_state(tap_get_end_state());
}

static void klink_execute_scan(struct jtag_command *cmd)
{
    uint8_t buffer[0x1000] = {0};
    LOG_DEBUG_IO("%s type:%d", cmd->cmd.scan->ir_scan ? "IRSCAN" : "DRSCAN",
                 jtag_scan_type(cmd->cmd.scan));

    /* Make sure there are no trailing fields with num_bits == 0, or the logic below will fail. */
    while (cmd->cmd.scan->num_fields > 0 && cmd->cmd.scan->fields[cmd->cmd.scan->num_fields - 1].num_bits == 0)
    {
        cmd->cmd.scan->num_fields--;
        LOG_DEBUG("discarding trailing empty field");
    }

    if (!cmd->cmd.scan->num_fields)
    {
        LOG_DEBUG("empty scan, doing nothing");
        return;
    }

    if (cmd->cmd.scan->ir_scan)
    {
        if (tap_get_state() != TAP_IRSHIFT)
        {
            klink_end_state(TAP_IRSHIFT);
            klink_state_move();
        }
    }
    else
    {
        if (tap_get_state() != TAP_DRSHIFT)
        {
            klink_end_state(TAP_DRSHIFT);
            klink_state_move();
        }
    }

    klink_end_state(cmd->cmd.scan->end_state);

    struct scan_field *field = cmd->cmd.scan->fields;
    unsigned int scan_size = 0;

    for (unsigned int i = 0; i < cmd->cmd.scan->num_fields; i++, field++)
    {
        scan_size += field->num_bits;
        LOG_DEBUG_IO("%s%s field %u/%u %u bits",
                     field->in_value ? "in" : "",
                     field->out_value ? "out" : "",
                     i,
                     cmd->cmd.scan->num_fields,
                     field->num_bits);
        if (i == cmd->cmd.scan->num_fields - 1 && tap_get_state() != tap_get_end_state())
        {
            LOG_DEBUG_IO("Last field SHIFT");
            klink_send_message(0, field->num_bits, field->out_value);
        }
        else
        {
            LOG_DEBUG_IO("field SHIFT");
            klink_send_message(0, field->num_bits, field->out_value);
        }
        klink_read_message(field->in_value != NULL ? (char *)field->in_value : (char *)buffer, DIV_ROUND_UP(field->num_bits, 8));
        // if (field->out_value)
        //     log_hex("SCAN out:", (uint8_t *)field->out_value, bytes - 8);
        // if (field->in_value)
        //     log_hex("SCAN in :", field->in_value, bytes - 8);
    }

    uint8_t tms_scan_bits = 1;
    uint8_t tms_scan = 0;
    klink_send_message(1, tms_scan_bits, &tms_scan);
    tap_set_state(cmd->cmd.scan->ir_scan ? TAP_IRPAUSE : TAP_DRPAUSE);
    LOG_DEBUG_IO("to state %s", tap_state_name(tap_get_state()));

    if (tap_get_state() != tap_get_end_state())
    {
        klink_end_state(tap_get_end_state());
        klink_state_move();
    }

    LOG_DEBUG_IO("%s scan, %i bits, end in %s",
                 (cmd->cmd.scan->ir_scan) ? "IR" : "DR", scan_size,
                 tap_state_name(tap_get_end_state()));
}

static void klink_execute_pathmove(struct jtag_command *cmd)
{
    uint8_t ucTMS[0x10] = {0};
    LOG_DEBUG_IO("pathmove: %i states, end in %i",
                 cmd->cmd.pathmove->num_states,
                 cmd->cmd.pathmove->path[cmd->cmd.pathmove->num_states - 1]);

    for (int k = 0; k < cmd->cmd.pathmove->num_states; k++)
    {
        // 计算当前位所在的字节和位索引
        uint32_t i = k >> 3;  // 等价于 k / 8
        uint32_t j = k & 0x7; // 等价于 k % 8
        if (cmd->cmd.pathmove->path[k] == tap_state_transition(tap_get_state(), false))
        {
            uint8_t mask = 1u << j;
            ucTMS[i] &= ~mask;
        }
        else if (cmd->cmd.pathmove->path[k] == tap_state_transition(tap_get_state(), true))
        {
            uint8_t mask = 1u << j;
            ucTMS[i] |= mask;
        }
        else
        {
            LOG_ERROR("BUG: %s -> %s isn't a valid TAP transition.",
                      tap_state_name(tap_get_state()), tap_state_name(cmd->cmd.pathmove->path[k]));
            exit(-1);
        }

        tap_set_state(cmd->cmd.pathmove->path[k]);
    }
    klink_send_message(1, cmd->cmd.pathmove->num_states, ucTMS);
    klink_end_state(tap_get_state());
}

/* TODO: Is there need to call cmsis_dap_flush() for the JTAG_PATHMOVE,
 * JTAG_RUNTEST, JTAG_STABLECLOCKS? */
static void klink_execute_command(struct jtag_command *cmd)
{
    switch (cmd->type)
    {
    case JTAG_SCAN:
        LOG_DEBUG("-->JTAG_SCAN");
        klink_execute_scan(cmd);
        break;
    case JTAG_TLR_RESET:
        // 持续num_cycles个周期的JTAG_TLR_RESET状态，最后切换到end_state
        LOG_DEBUG("-->JTAG_TLR_RESET");
        uint8_t ucFF[0x10] = {0};
        memset(ucFF, 0xFF, 0x10);
        klink_send_message(1, cmd->cmd.runtest->num_cycles + 5, ucFF);
        tap_set_state(TAP_RESET);
        break;
    case JTAG_RUNTEST:
        // 持续num_cycles个周期的RUNTEST状态，最后切换到end_state
        LOG_DEBUG("-->JTAG_RUNTEST");
        uint8_t uc00[0x10] = {0};
        klink_end_state(cmd->cmd.runtest->end_state);
        // 保存当前的tap状态
        enum tap_state saved_end_state = tap_get_end_state();
        // 移动到IDLE
        if (tap_get_state() != TAP_IDLE)
        {
            klink_end_state(TAP_IDLE);
            klink_state_move();
        }
        // 执行IDLE周期
        klink_send_message(1, cmd->cmd.runtest->num_cycles, uc00);
        // 恢复tap状态
        klink_end_state(saved_end_state);
        if (tap_get_state() != tap_get_end_state())
            klink_state_move();
        break;
    case JTAG_RESET:
        // 复位Tap状态函数
        // 连续6个以上的TMS高可将状态机置为Test-Logic Reset状态
        LOG_DEBUG("-->JTAG_RESET");
        break;
    case JTAG_PATHMOVE:
        // 状态机迁移
        LOG_DEBUG("-->JTAG_PATHMOVE");
        klink_execute_pathmove(cmd);
        break;
    case JTAG_SLEEP:
        LOG_DEBUG("-->JTAG_SLEEP");
        jtag_sleep(cmd->cmd.sleep->us);
        break;
    case JTAG_STABLECLOCKS:
        // 输出稳定空周期
        LOG_DEBUG("-->JTAG_STABLECLOCKS");
        uint8_t ucbuff[0x10] = {0};
        if (tap_get_state() == TAP_RESET)
            memset(ucbuff, 0xFF, 0x10);
        klink_send_message(1, cmd->cmd.stableclocks->num_cycles, ucbuff);
        break;
    case JTAG_TMS:
        LOG_DEBUG("-->JTAG_TMS");
        klink_send_message(1, cmd->cmd.tms->num_bits, cmd->cmd.tms->bits);
        break;
    default:
        LOG_ERROR("BUG: unknown JTAG command type 0x%X encountered", cmd->type);
        exit(-1);
    }
}

static int klink_execute_queue(struct jtag_command *cmd_queue)
{
    struct jtag_command *cmd = cmd_queue;

    while (cmd)
    {
        klink_execute_command(cmd);
        cmd = cmd->next;
    }

    return ERROR_OK;
}

static struct jtag_interface klink_interface = {
    .supported = DEBUG_CAP_TMS_SEQ,
    .execute_queue = klink_execute_queue,
};

struct adapter_driver klink_adapter_driver = {
    .name = "klink",
    .transport_ids = TRANSPORT_JTAG,
    .commands = klink_command_handlers,

    .init = klink_init,

    .jtag_ops = &klink_interface,
};
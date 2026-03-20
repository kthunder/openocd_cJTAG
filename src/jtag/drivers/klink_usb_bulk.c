// SPDX-License-Identifier: GPL-2.0-or-later

/***************************************************************************
 *   Copyright (C) 2018 by Mickaël Thomas                                  *
 *   mickael9@gmail.com                                                    *
 *                                                                         *
 *   Copyright (C) 2016 by Maksym Hilliaka                                 *
 *   oter@frozen-team.com                                                  *
 *                                                                         *
 *   Copyright (C) 2016 by Phillip Pearson                                 *
 *   pp@myelin.co.nz                                                       *
 *                                                                         *
 *   Copyright (C) 2014 by Paul Fertser                                    *
 *   fercerpav@gmail.com                                                   *
 *                                                                         *
 *   Copyright (C) 2013 by mike brown                                      *
 *   mike@theshedworks.org.uk                                              *
 *                                                                         *
 *   Copyright (C) 2013 by Spencer Oliver                                  *
 *   spen@spen-soft.co.uk                                                  *
 ***************************************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <helper/system.h>
#include <libusb.h>
#include <helper/log.h>
#include <helper/replacements.h>
#include <jtag/jtag.h> /* ERROR_JTAG_DEVICE_ERROR only */

#include "libusb_helper.h"

typedef struct
{
    uint8_t len;
    uint8_t opcode;
    uint16_t bits;
    uint8_t data[];
} message;

struct libusb_context *ctx;
libusb_device_handle *dev_handle;

#define ED_OUT (0x01)
#define ED_IN (ED_OUT | 0x80)

int klink_usb_open(uint16_t vid, uint16_t pid)
{
    libusb_init(&ctx);

    dev_handle = libusb_open_device_with_vid_pid(ctx, vid, pid);

    if (dev_handle)
    {
        libusb_claim_interface(dev_handle, 0);
        libusb_set_interface_alt_setting(dev_handle, 0, 0);
        return 1;
    }

    return -1;
}

static int __write_serial_port(uint8_t *data, size_t length)
{
    int actual_length = 0;
    int res = 0;
    while (length > 0)
    {
        res = libusb_bulk_transfer(dev_handle, ED_OUT, data, length, &actual_length, 1000);
        if (res == 0)
        {
            length -= actual_length;
            data += actual_length;
        }
        else
        {
            return -1;
        }
    }
    return length;
}

// 读取串口数据
int klink_read_message(uint8_t *buffer, size_t length)
{
    int actual_length = 0;
    int res = 0;
    while (length > 0)
    {
        res = libusb_bulk_transfer(dev_handle, ED_IN, buffer, length, &actual_length, 1000);
        if (res == 0)
        {
            length -= actual_length;
            buffer += actual_length;
        }
        else
        {
            return -1;
        }
    }
    return length;
}

int klink_send_message(uint8_t opcode, uint16_t bits, uint8_t *ucData)
{
    static uint8_t ucBuffer[0x1000] = {0};
    static uint32_t nBufferLen = 0;

    message *msg = (message *)(ucBuffer + nBufferLen);
    uint8_t data_len = ucData ? (bits + 7) / 8 : 0;
    msg->opcode = opcode;
    msg->bits = bits;

    msg->len = sizeof(message) + data_len;
    memcpy(&msg->data[0], ucData, data_len);

    nBufferLen += msg->len;
    // if (opcode == 0)
    // {
    //     printf("opcode 0x%X\r\n", msg->opcode);
    //     printf("msg->len %d\r\n", msg->len);
    //     for (size_t i = 0; i < msg->len; i++)
    //     {
    //         printf("%02X ", ((uint8_t *)msg)[i]);
    //     }
    //     printf("\r\n");
    //     printf("----------\r\n");
    // }

    if (opcode == 0)
    {
        __write_serial_port(ucBuffer, nBufferLen);
        // for (size_t i = 0; i < 10; i++)
        // {
        //     ucBuffer[0] = i;
        //     printf("++++++++++++++++++++++clear buffer %d++++++++++++++++++++++\r\n", nBufferLen);
        //     __write_serial_port(ucBuffer, nBufferLen);
            
        // }
        // exit(0);
        // printf("++++++++++++++++++++++clear buffer %d++++++++++++++++++++++\r\n", nBufferLen);
        nBufferLen = 0;
    }
    else
    {
        // printf("++++++++++++++++++++++add in buffer %d++++++++++++++++++++++\r\n", msg->len);
    }

    return 1;
}
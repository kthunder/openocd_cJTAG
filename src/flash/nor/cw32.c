// SPDX-License-Identifier: GPL-2.0-or-later

/***************************************************************************
 *   Copyright (C) 2005 by Dominic Rath                                    *
 *   Dominic.Rath@gmx.de                                                   *
 *                                                                         *
 *   Copyright (C) 2008 by Spencer Oliver                                  *
 *   spen@spen-soft.co.uk                                                  *
 *                                                                         *
 *   Copyright (C) 2011 by Andreas Fritiofson                              *
 *   andreas.fritiofson@gmail.com                                          *
 ***************************************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>

#include "imp.h"
#include <helper/binarybuffer.h>
#include <target/algorithm.h>
#include <target/cortex_m.h>

#include <helper/time_support.h>
/* timeout values */

#define FLASH_WRITE_TIMEOUT 10
#define FLASH_ERASE_TIMEOUT 100

#define PROGRAM_BKPT_OFFSET 0x08
#define PROGRAM_ARGS_OFFSET 0x10
#define SECTOR_SIZE 0x200  // 512 bytes sector size

struct cw32_options
{
    uint8_t rdp;
    uint8_t user;
    uint16_t data;
    uint32_t protection;
};

struct fls_algo_param
{
    uint32_t start_addr;
    uint32_t __bkpt_label;
    uint32_t g_func;
    uint32_t g_rwBuffer;
    uint32_t g_rwBuffer_size;
    uint32_t g_dstAddress;
    uint32_t g_length;
    uint32_t g_checksum;
    uint32_t g_flashIndex;
    uint32_t g_error;
    bool init;

    uint8_t cached_sector[2][SECTOR_SIZE];
    uint32_t cached_sector_addr[2];
    bool cacheed_sector_valid[2];
} fls_algo_params = {0};

struct cw32_flash_bank
{
    struct cw32_options option_bytes;
    int ppage_size;
    bool probed;

    bool has_dual_banks;
    /* used to access dual flash bank stm32xl */
    bool can_load_options;
    uint32_t register_base;
    uint8_t default_rdp;
    int user_data_offset;
    int option_offset;
    uint32_t user_bank_size;
};

uint8_t flash_index=0;

static int cw32_write_block(struct flash_bank *bank, const uint8_t *buffer,
                              uint32_t address, uint32_t hwords_count);
static int cw32_load_fls_algo(struct flash_bank *bank);
static int cw32_load_elf(struct flash_bank *bank, char *path, struct image *image);
/* flash bank stm32x <base> <size> 0 0 <target#>
 */
FLASH_BANK_COMMAND_HANDLER(cw32_flash_bank_command)
{
    if (CMD_ARGC < 6)
        return ERROR_COMMAND_SYNTAX_ERROR;

    struct cw32_flash_bank * cw32_info = malloc(sizeof(struct cw32_flash_bank));

    /* set page size, protection granularity and max flash size depending on family */
    cw32_info->ppage_size = 32;
    cw32_info->probed = false;

    cw32_info->has_dual_banks = false;
    cw32_info->can_load_options = false;
    cw32_info->register_base = 0;// UNUSED set zero
    /* default factory read protection level 0 */
    cw32_info->default_rdp = 0;
    cw32_info->user_data_offset = 0;
    cw32_info->option_offset = 0;
    cw32_info->user_bank_size = bank->size;

    bank->driver_priv = cw32_info;
    /* The flash write must be aligned to a halfword boundary */
    bank->write_start_alignment = bank->write_end_alignment = 2;

    return ERROR_OK;
}

static int cw32_erase(struct flash_bank *bank, unsigned int first,
                        unsigned int last)
{
    // log_info("%s", __func__);
    int retval;
    struct target *target = bank->target;
    if (!fls_algo_params.init) 
    {
        cw32_load_fls_algo(bank);
    }

    uint32_t func = 3;
    uint32_t addr = bank->base + bank->sectors[first].offset;
    uint32_t len = bank->sectors[last].offset + bank->sectors[last].size - bank->sectors[first].offset;

    {
        fls_algo_params.cached_sector_addr[0] = bank->base + bank->sectors[first].offset;
        fls_algo_params.cached_sector_addr[1] = bank->base + bank->sectors[last].offset;
        fls_algo_params.cacheed_sector_valid[0] = true;
        fls_algo_params.cacheed_sector_valid[1] = true;

        if(addr >= 0x01080000) {

            target_read_buffer(target, fls_algo_params.cached_sector_addr[0] + ((flash_index-2)*0x20000), SECTOR_SIZE, fls_algo_params.cached_sector[0]);
            target_read_buffer(target, fls_algo_params.cached_sector_addr[1] + ((flash_index-2)*0x20000), SECTOR_SIZE, fls_algo_params.cached_sector[1]);
        }
        else {
            target_read_buffer(target, fls_algo_params.cached_sector_addr[0], SECTOR_SIZE, fls_algo_params.cached_sector[0]);
            target_read_buffer(target, fls_algo_params.cached_sector_addr[1], SECTOR_SIZE, fls_algo_params.cached_sector[1]);
        }
    }

    if (fls_algo_params.g_flashIndex != 0) {
        uint32_t flashIndex = (addr >= 0x01080000 ? flash_index : 2);
        target_write_buffer(target, fls_algo_params.g_flashIndex, 4, &flashIndex);
    }

    retval = target_write_buffer(target, fls_algo_params.g_dstAddress, 4, &addr);
    retval = target_write_buffer(target, fls_algo_params.g_length, 4, &len);
    retval = target_write_buffer(target, fls_algo_params.g_func, 4, &func);

    int64_t run_algo_start = timeval_ms();
    retval = target_run_algorithm(target,
                                  0, NULL,
                                  0, NULL,
                                  fls_algo_params.__bkpt_label+2,
                                  fls_algo_params.__bkpt_label,
                                  10000, NULL);
    log_info("run erase algo %" PRId64 " ms.[%d sectors]", timeval_ms() - run_algo_start, last - first + 1);

    if (retval != ERROR_OK)
    {
        LOG_ERROR("Failed to execute algorithm at 0x%" PRIx32 ": %"PRId32"",
                  fls_algo_params.__bkpt_label, retval);
    }

    return retval;
}

static int cw32_protect(struct flash_bank *bank, int set, unsigned int first, unsigned int last)
{
    // log_info("%s", __func__);
    return ERROR_FLASH_OPER_UNSUPPORTED;
}

static int cw32_fls_algo_write(struct flash_bank *bank, const uint8_t *buffer, uint32_t address, uint32_t len)
{
    int retval = 0;
    struct target *target = bank->target;
    uint32_t func = 1;
    retval = target_write_buffer(target, fls_algo_params.g_rwBuffer, len, buffer);
    retval = target_write_buffer(target, fls_algo_params.g_func, 4, &func);
    retval = target_write_buffer(target, fls_algo_params.g_dstAddress, 4, &address);
    retval = target_write_buffer(target, fls_algo_params.g_length, 4, &len);
    retval = target_run_algorithm(target,
                                  0, NULL,
                                  0, NULL,
                                  fls_algo_params.__bkpt_label+2,
                                  fls_algo_params.__bkpt_label,
                                  10000, NULL);
    return retval;
}

static int cw32_write_block_riscv(struct flash_bank *bank, const uint8_t *buffer,
                                    uint32_t address, uint32_t words_count)
{
    int64_t write_block_start = timeval_ms();
    
    uint32_t retval = ERROR_OK;
    uint32_t all = words_count*4;
    uint32_t total_bytes = words_count*4;
    // log_info("%s", __func__);
    if (address>fls_algo_params.cached_sector_addr[0] && address<(fls_algo_params.cached_sector_addr[0]+SECTOR_SIZE)) {
        // log_info("first need padding addr %08X", address);
        uint32_t padd_len=address%SECTOR_SIZE;
        memcpy(fls_algo_params.cached_sector[0]+padd_len, buffer, (SECTOR_SIZE - padd_len));
        cw32_fls_algo_write(bank, fls_algo_params.cached_sector[0], fls_algo_params.cached_sector_addr[0], SECTOR_SIZE);

        buffer += (SECTOR_SIZE - padd_len);
        address += (SECTOR_SIZE - padd_len);
        total_bytes -= (SECTOR_SIZE - padd_len);
    }
    
    for ( ; total_bytes>0; total_bytes-=fls_algo_params.g_rwBuffer_size) {
        if (total_bytes>=fls_algo_params.g_rwBuffer_size) {
            cw32_fls_algo_write(bank, buffer, address, fls_algo_params.g_rwBuffer_size);
            address+=fls_algo_params.g_rwBuffer_size;
            buffer+=fls_algo_params.g_rwBuffer_size;
        }
        else {
            uint32_t curr_len = total_bytes-total_bytes%SECTOR_SIZE;
            cw32_fls_algo_write(bank, buffer, address, curr_len);
            address+=curr_len;
            buffer+=curr_len;
            total_bytes -= curr_len;
            break;
        }
    }

    if (total_bytes>0) {
        if (address == fls_algo_params.cached_sector_addr[1]) {
            // log_info("last need padding addr %08X", address);
            uint8_t* new_buffer = malloc(SECTOR_SIZE);
            // uint32_t padd_len=address%SECTOR_SIZE;
            memcpy(new_buffer, buffer, total_bytes);
            memcpy(new_buffer+total_bytes, fls_algo_params.cached_sector[1]+total_bytes, (SECTOR_SIZE - total_bytes));
            cw32_fls_algo_write(bank, new_buffer, address, SECTOR_SIZE);
        }
        else {
            cw32_fls_algo_write(bank, buffer, address, total_bytes);
        }
    }

    log_info("write block %" PRId64 " ms.[0x%X bytes]", timeval_ms() - write_block_start, all);

    return retval;
}

/** Writes a block to flash either using target algorithm
 *  or use fallback, host controlled halfword-by-halfword access.
 *  Flash controller must be unlocked before this call.
 */
static int cw32_write_block(struct flash_bank *bank,
                              const uint8_t *buffer, uint32_t address, uint32_t words_count)
{
    struct target *target = bank->target;

    /* The flash write must be aligned to a halfword boundary.
     * The flash infrastructure ensures it, do just a security check
     */
    assert(address % 4 == 0);
    if (fls_algo_params.g_flashIndex != 0) {
        uint32_t flashIndex = (address >= 0x01080000 ? flash_index : 2);
        target_write_buffer(target, fls_algo_params.g_flashIndex, 4, &flashIndex);
    }

    int retval;
    retval = cw32_write_block_riscv(bank, buffer, address, words_count);

    if (retval == ERROR_TARGET_RESOURCE_NOT_AVAILABLE)
    {
        /* if block write failed (no sufficient working area),
         * we use normal (slow) single halfword accesses */
        LOG_WARNING("couldn't use block writes, falling back to single memory accesses");

        while (words_count > 0)
        {
            retval = target_write_memory(target, address, 4, 1, buffer);
            if (retval != ERROR_OK)
                return retval;

            words_count--;
            buffer += 4;
            address += 4;
        }
    }
    return retval;
}

static int cw32_write(struct flash_bank *bank, const uint8_t *buffer,
                        uint32_t offset, uint32_t count)
{
    // log_info("%s", __func__);
    struct target *target = bank->target;
    if (!fls_algo_params.init) 
    {
        cw32_load_fls_algo(bank);
    }
    if (bank->target->state != TARGET_HALTED)
    {
        LOG_ERROR("Target not halted");
        return ERROR_TARGET_NOT_HALTED;
    }

    /* The flash write must be aligned to a word boundary.
     * The flash infrastructure ensures it, do just a security check
     */
    assert(offset % 4 == 0);
    assert(count % 4 == 0);

    int retval;

    /* write to flash */
    retval = cw32_write_block(bank, buffer, bank->base + offset, count / 4);

    return retval;
}

struct cw32_property_addr
{
    uint32_t device_id;
    uint32_t flash_size;
};

static int cw32_get_property_addr(struct target *target, struct cw32_property_addr *addr)
{
    return ERROR_NOT_IMPLEMENTED;
}

static int cw32_get_device_id(struct flash_bank *bank, uint32_t *device_id)
{
    return ERROR_NOT_IMPLEMENTED;
}

static int cw32_get_flash_size(struct flash_bank *bank, uint16_t *flash_size_in_kb)
{
    return ERROR_NOT_IMPLEMENTED;
}

static int cw32_probe(struct flash_bank *bank)
{
    // log_info("%s", __func__);
    struct cw32_flash_bank *cw32_info = bank->driver_priv;
    uint16_t sector_size = 0x200;

    // LOG_INFO("flash size = %d KiB", bank->size);

    /* did we assign flash size? */
    assert(bank->size != 0xffff);

    free(bank->sectors);
    bank->sectors = NULL;

    free(bank->prot_blocks);
    bank->prot_blocks = NULL;

    bank->num_sectors = bank->size / sector_size;
    bank->sectors = alloc_block_array(0, sector_size, bank->num_sectors);
    if (!bank->sectors)
        return ERROR_FAIL;

    cw32_info->probed = true;

    return ERROR_OK;
}

static int cw32_auto_probe(struct flash_bank *bank)
{
    // log_info("%s", __func__);
    if (((struct cw32_flash_bank *)bank->driver_priv)->probed)
        return ERROR_OK;
    return cw32_probe(bank);
}

static int cw32_protect_check(struct flash_bank *bank)
{
    // log_info("%s", __func__);
    return ERROR_OK;
}

static int get_cw32_info(struct flash_bank *bank, struct command_invocation *cmd)
{
    // log_info("%s", __func__);
    const char *device_str;
    const char *rev_str = NULL;

    device_str = "cw32";
    rev_str = "B";
    command_print_sameline(cmd, "%s - Rev: %s", device_str, rev_str);

    return ERROR_OK;
}

COMMAND_HANDLER(cw32_handle_user_command)
{
    struct target *target = NULL;
    struct cw32_flash_bank *cw32_info = NULL;
    struct flash_bank *bank;

    if (CMD_ARGC < 1)
        return ERROR_COMMAND_SYNTAX_ERROR;

    int retval = CALL_COMMAND_HANDLER(flash_command_get_bank, 0, &bank);
    if (retval != ERROR_OK)
        return retval;
    return retval;
}

COMMAND_HANDLER(cw32_handle_set_flash_index_command)
{
    if (CMD_ARGC != 1)
        return ERROR_COMMAND_SYNTAX_ERROR;

    LOG_DEBUG("set flash index %d", atoi(CMD_ARGV[0]));
    flash_index = atoi(CMD_ARGV[0]);
    return ERROR_OK;
}

#include <target/image.h>
#include <helper/configuration.h>

static int cw32_load_elf(struct flash_bank *bank, char *path, struct image *image)
{
    int retval = ERROR_FAIL;
    char *full_path = find_file(path);
    if (full_path != NULL) {
        retval = image_open(image, full_path, "elf");
        if (retval == ERROR_OK) {
            uint32_t image_size = 0;
            struct duration bench;
            duration_start(&bench);
            // log_info("%s : section cnt %u",full_path, image->num_sections);
            for (unsigned int i = 0; i < image->num_sections; i++) {

                struct imagesection *section = &image->sections[i];
                // log_info("section[%d] addr 0x%08x size %u", i, (uint32_t)section->base_address, section->size);

                uint8_t *buffer = malloc(section->size);
                if (!buffer) {
                    retval = ERROR_FAIL;
                    break;
                }

                uint8_t *read_buffer = malloc(section->size);
                if (!buffer) {
                    retval = ERROR_FAIL;
                    break;
                }

                size_t buf_cnt;
                retval = image_read_section(image, i, 0x0, section->size, buffer, &buf_cnt);
                if (retval != ERROR_OK) {
                    free(buffer);
                    break;
                }

                retval = target_read_buffer(bank->target, (uint32_t)section->base_address, 0x80, read_buffer);
                if (memcmp(read_buffer, buffer, 0x10)==0) {
                    retval = ERROR_OK;
                    return retval;
                    break;
                }

                retval = target_write_buffer(bank->target, (uint32_t)section->base_address,buf_cnt, buffer);
                free(buffer);
                if (retval != ERROR_OK)
                    break;

                image_size += buf_cnt;
            }
            duration_measure(&bench);
            float time = duration_elapsed(&bench);
            float speed = duration_kbps(&bench, image_size);
            log_info("load %s %u bytes in %fs (%0.3f KiB/s)", path, image_size, time, speed);
        }
        else 
        {
            // log_info("%s open failed", full_path);
        }
        free(full_path);
    }
    else
    {
        // log_info("file %s not find", path);
    }
    return retval;
}

static int cw32_load_rom(struct flash_bank *bank)
{
    // log_info("%s", __func__);
    int retval = ERROR_OK;
    struct image image = {.base_address_set = false};
    char rom_path[100] = {0};
    sprintf(rom_path, "../cw_fls_algo/%s_rom.elf", bank->driver->name);

    retval = cw32_load_elf(bank, rom_path, &image);
    if (retval == ERROR_OK) {
        image_close(&image);
    }

    return retval;
}

extern int image_find_symbol(struct image *image, const char *symbol_name, uint32_t *address, uint32_t *size);
static int cw32_load_fls_algo(struct flash_bank *bank)
{
    cw32_load_rom(bank);
    // log_info("%s", __func__);

    struct target *target = bank->target;

    int retval = 0;
    uint32_t size = 0;
    struct image image = {.base_address_set = false};
    char alhgo_path[100] = {0};
    sprintf(alhgo_path, "../cw_fls_algo/%s_flash_algo.elf", bank->driver->name);

    retval = cw32_load_elf(bank, alhgo_path, &image);
    if (retval == ERROR_OK) {
        retval = image_find_symbol(&image, "Reset_Handler", &fls_algo_params.start_addr,  &size);
        // log_info("Symbol '%s' found at address: 0x%08x size %d", "Reset_Handler", fls_algo_params.start_addr,  size);
        retval = image_find_symbol(&image, "__bkpt_label", &fls_algo_params.__bkpt_label,  &size);
        // log_info("Symbol '%s' found at address: 0x%08x size %d", "__bkpt_label", fls_algo_params.__bkpt_label,  size);
        retval = image_find_symbol(&image, "g_rwBuffer", &fls_algo_params.g_rwBuffer,  &fls_algo_params.g_rwBuffer_size);
        // log_info("Symbol '%s' found at address: 0x%08x size %d", "g_rwBuffer", fls_algo_params.g_rwBuffer,  fls_algo_params.g_rwBuffer_size);
        retval = image_find_symbol(&image, "g_dstAddress", &fls_algo_params.g_dstAddress,  &size);
        // log_info("Symbol '%s' found at address: 0x%08x size %d", "g_dstAddress", fls_algo_params.g_dstAddress,  size);
        retval = image_find_symbol(&image, "g_length", &fls_algo_params.g_length,  &size);
        // log_info("Symbol '%s' found at address: 0x%08x size %d", "g_length", fls_algo_params.g_length,  size);
        retval = image_find_symbol(&image, "g_func", &fls_algo_params.g_func,  &size);
        // log_info("Symbol '%s' found at address: 0x%08x size %d", "g_func", fls_algo_params.g_func,  size);
        retval = image_find_symbol(&image, "g_flashIndex", &fls_algo_params.g_flashIndex,  &size);
        // log_info("Symbol '%s' found at address: 0x%08x size %d", "g_flashIndex", fls_algo_params.g_flashIndex,  size);
        retval = image_find_symbol(&image, "g_error", &fls_algo_params.g_error,  &size);
        // log_info("Symbol '%s' found at address: 0x%08x size %d", "g_error", fls_algo_params.g_error,  size);

        retval = target_run_algorithm(target,
                                    0, NULL,
                                    0, NULL,
                                    fls_algo_params.start_addr,
                                    fls_algo_params.__bkpt_label,
                                    100, NULL);

        image_close(&image);
    }

    return retval;
}

    // cw32 load_fls_algo cw3065_flash_algo.elf
    // program cw3065_sdk_production.hex
static const struct command_registration cw32_exec_command_handlers[] = {
    {
        .name = "user",
        .handler = cw32_handle_user_command,
        .mode = COMMAND_EXEC,
        .usage = "bank_id",
        .help = "user",
    },
    {
        .name = "set_flash_index",
        .handler = cw32_handle_set_flash_index_command,
        .mode = COMMAND_EXEC,
        .help = "set flash index",
        .usage = "index",
    },
    COMMAND_REGISTRATION_DONE};

static const struct command_registration cw32_command_handlers[] = {
    {
        .name = "cw32",
        .mode = COMMAND_ANY,
        .help = "cw32 flash command group",
        .usage = "",
        .chain = cw32_exec_command_handlers,
    },
    COMMAND_REGISTRATION_DONE};

const struct flash_driver cw2225_flash = {
    .name = "cw2225",
    .commands = cw32_command_handlers,
    .flash_bank_command = cw32_flash_bank_command,
    .erase = cw32_erase,
    .protect = cw32_protect,
    .write = cw32_write,
    .read = default_flash_read,
    .probe = cw32_probe,
    .auto_probe = cw32_auto_probe,
    .erase_check = default_flash_blank_check,
    .protect_check = cw32_protect_check,
    .info = get_cw32_info,
    .free_driver_priv = default_flash_free_driver_priv,
};

const struct flash_driver cw2245_flash = {
    .name = "cw2245",
    .commands = cw32_command_handlers,
    .flash_bank_command = cw32_flash_bank_command,
    .erase = cw32_erase,
    .protect = cw32_protect,
    .write = cw32_write,
    .read = default_flash_read,
    .probe = cw32_probe,
    .auto_probe = cw32_auto_probe,
    .erase_check = default_flash_blank_check,
    .protect_check = cw32_protect_check,
    .info = get_cw32_info,
    .free_driver_priv = default_flash_free_driver_priv,
};

const struct flash_driver cw3065_flash = {
    .name = "cw3065",
    .commands = cw32_command_handlers,
    .flash_bank_command = cw32_flash_bank_command,
    .erase = cw32_erase,
    .protect = cw32_protect,
    .write = cw32_write,
    .read = default_flash_read,
    .probe = cw32_probe,
    .auto_probe = cw32_auto_probe,
    .erase_check = default_flash_blank_check,
    .protect_check = cw32_protect_check,
    .info = get_cw32_info,
    .free_driver_priv = default_flash_free_driver_priv,
};

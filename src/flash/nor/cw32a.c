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

#include "helper/log.h"
#include "target/armv7m.h"
#include "target/target.h"
#include <stdint.h>
#include <time.h>
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>

#include "imp.h"
#include <helper/binarybuffer.h>
#include <target/algorithm.h>
#include <target/cortex_m.h>

#include <target/image.h>
#include <helper/configuration.h>
#include "target/breakpoints.h"

#include <helper/time_support.h>
/* timeout values */

struct cw32_options
{
	uint8_t rdp;
	uint8_t user;
	uint16_t data;
	uint32_t protection;
};

struct fls_algo_param
{
	volatile uint32_t bkpt;
	volatile uint32_t func;
	volatile uint32_t func_size;
	volatile uint32_t func_end;
	volatile bool init;
};
static struct fls_algo_param fls_algo_params = {0};
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

static struct armv7m_algorithm armv7m_info = 
{
	.common_magic = ARMV7M_COMMON_MAGIC,
	.core_mode = ARM_MODE_THREAD,
};	

#define CW2213_FLS_ALGO_FILE		"../cw_fls_algo/cw2213_flash_algo.elf"
#define CW2213_ROM_FILE				"../cw_fls_algo/cw2213_rom.elf"
static int cw32_write_block(struct flash_bank *bank, const uint8_t *buffer,
							  uint32_t address, uint32_t hwords_count);
static int cw32_load_fls_algo(struct flash_bank *bank);
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
	int retval = 0;
	struct target *target = bank->target;
	if (!fls_algo_params.init) 
	{
		fls_algo_params.init = true;
		cw32_load_fls_algo(bank);
	}

	int64_t run_algo_start = timeval_ms();

	uint32_t addr = bank->base + bank->sectors[first].offset;
	uint32_t len = bank->base + bank->sectors[last].offset + bank->sectors[last].size - addr;

	/* use alg to write data from work area to NAND chip */
	log_info("erase start addr %08X.[0x%X bytes]", addr, len);

	target_write_u32(target, 0x20000, 1);
	target_write_u32(target, 0x20004, addr);
	target_write_u32(target, 0x20008, len);
	retval = target_run_algorithm(target,
								0, NULL,
								0, NULL,
								fls_algo_params.func,
								fls_algo_params.bkpt,
								10000, &armv7m_info);

	log_info("run erase algo %" PRId64 " ms.[%d sectors]", timeval_ms() - run_algo_start, last - first + 1);

	if (retval != ERROR_OK)
	{
		LOG_ERROR("Failed to execute algorithm at 0x%" PRIx32 ": %"PRId32"",
				  fls_algo_params.func_end, retval);
	}

	return retval;
}

static int cw32_protect(struct flash_bank *bank, int set, unsigned int first, unsigned int last)
{
	// log_info("%s", __func__);
	return ERROR_FLASH_OPER_UNSUPPORTED;
}

static int cw32_write_block_riscv(struct flash_bank *bank, const uint8_t *buffer,
									uint32_t address, uint32_t words_count)
{
	int64_t write_block_start = timeval_ms();
	// log_info("%s", __func__);
	struct target *target = bank->target;
	int retval = 0;
	uint32_t total_bytes = words_count*4;

	while (words_count > 0)
	{
		uint32_t thisrun_words = 0x800 / 4;

		/* Limit to the amount of data we actually want to write */
		if (thisrun_words > words_count)
			thisrun_words = words_count;

		/* Write data to buffer */
		uint32_t len = thisrun_words * 4;

		// int64_t write_data_start = timeval_ms();
		/* Write data to buffer */
		retval = target_write_buffer(target, 0x21000,len, buffer);
		// log_info("load program data in ram buff %08X.[%d bytes]", 0x21000, len);
		// log_info("program fls %08X.[%d bytes]", address, len);

		target_write_u32(target, 0x20000, 2);
		target_write_u32(target, 0x20004, address);
		target_write_u32(target, 0x20008, len);
		target_write_u32(target, 0x2000C, 0x21000);
		retval = target_run_algorithm(target,
									0, NULL,
									0, NULL,
									fls_algo_params.func,
									fls_algo_params.bkpt,
									10000, &armv7m_info);

		if (retval != ERROR_OK)
		{
			LOG_ERROR("Failed to execute algorithm at 0x%" PRIx32 ": %"PRId32"",
					  address, retval);
			break;
		}

		/* Update counters */
		buffer += thisrun_words * 4;
		address += thisrun_words * 4;
		words_count -= thisrun_words;
	}
	log_info("write block %" PRId64 " ms.[0x%X bytes]", timeval_ms() - write_block_start, total_bytes);

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
	log_info("%s", __func__);

	// struct target *target = bank->target;
	if (!fls_algo_params.init) 
	{
		fls_algo_params.init = true;
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

static int cw32_load_fls_algo(struct flash_bank *bank)
{
	struct target *target = bank->target;

	int retval;
	const char *algo_path;
	char *full_path = NULL;
	if (strcmp(bank->driver->name, "cw2213") == 0) {
		algo_path = CW2213_FLS_ALGO_FILE;
	}else {
		return ERROR_FAIL;
	}

	LOG_DEBUG("load fls algo %s", algo_path);
	full_path = find_file(algo_path);
	if (full_path == NULL) 
	{
		LOG_ERROR("Cannot find %s", algo_path);
		return ERROR_FAIL;
    }
    
    struct image image;
    uint32_t size;
    log_info("algo = %s", full_path);
    retval = image_open(&image, full_path, "elf");

	for (unsigned int i = 0; i < image.num_sections; i++) {
		log_info("+a section %d sec_address = 0x%08x, size = %d", i, &image.sections[i], image.sections[i].size);
	}

extern int image_find_symbol(struct image *image, const char *symbol_name, 
                                uint32_t *address, uint32_t *size);
	retval = image_find_symbol(&image, "BKPT", &fls_algo_params.bkpt,  &size);
	fls_algo_params.bkpt--;
	retval = image_find_symbol(&image, "main", &fls_algo_params.func,  &fls_algo_params.func_size);
	fls_algo_params.func_end = fls_algo_params.func + fls_algo_params.func_size -2 -1;
    // log_info("Symbol '%s' found at address: 0x%08x size %d", "main", fls_algo_params.func,  fls_algo_params.func_size);

	uint8_t *buffer;
	size_t buf_cnt;
	uint32_t image_size;

	struct duration bench;
	duration_start(&bench);
	image_size = 0x0;
	retval = ERROR_OK;
	for (unsigned int i = 0; i < image.num_sections; i++) {

		buffer = malloc(image.sections[i].size);
		if (!buffer) {
			log_error("error allocating buffer for section (%d bytes)",
						  (int)(image.sections[i].size));
			retval = ERROR_FAIL;
			break;
		}

		retval = image_read_section(&image, i, 0x0, image.sections[i].size, buffer, &buf_cnt);
		
		if (retval != ERROR_OK) {
			free(buffer);
			break;
		}

		retval = target_write_buffer(target, 0x20000, buf_cnt, buffer);
		log_info("retval %d", retval);
		if (retval != ERROR_OK) {
			free(buffer);
			break;
		}
		
		image_size += image.sections[i].size;

		free(buffer);
	}

	if ((retval == ERROR_OK) && (duration_measure(&bench) == ERROR_OK)) {
		log_info("load flash algo %" PRIu32 " bytes "
				"in %fs (%0.3f KiB/s)", image_size,
				duration_elapsed(&bench), duration_kbps(&bench, image_size));
	}

    image_close(&image);

	return retval;
}

static const struct command_registration cw32_exec_command_handlers[] = {
	{
		.name = "user",
		.handler = cw32_handle_user_command,
		.mode = COMMAND_EXEC,
		.usage = "bank_id",
		.help = "user",
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

const struct flash_driver cw2213_flash = {
	.name = "cw2213",
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

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

struct cw32_options
{
	uint8_t rdp;
	uint8_t user;
	uint16_t data;
	uint32_t protection;
};

struct fls_algo_param
{
	volatile uint32_t start_addr;
	volatile uint32_t __bkpt_label;
	volatile uint32_t g_func;
	volatile uint32_t g_rwBuffer;
	volatile uint32_t g_rwBuffer_size;
	volatile uint32_t g_dstAddress;
	volatile uint32_t g_length;
	volatile uint32_t g_checksum;
	volatile uint32_t g_flashIndex;
	volatile bool init;
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

#define CW2225_FLS_ALGO_FILE		"../cw_fls_algo/cw2225_flash_algo.elf"
#define CW2245_FLS_ALGO_FILE		"../cw_fls_algo/cw2245_flash_algo.elf"
#define CW3065_FLS_ALGO_FILE		"../cw_fls_algo/cw3065_flash_algo.elf"
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
	// log_info("%s", __func__);
	int retval;
	struct target *target = bank->target;
	if (!fls_algo_params.init) 
	{
		fls_algo_params.init = true;
		cw32_load_fls_algo(bank);
	}

	uint32_t func = 3;
	uint32_t addr = bank->base + bank->sectors[first].offset;
	uint32_t len = bank->sectors[last].offset + bank->sectors[last].size - bank->sectors[first].offset;

	log_info("run erase algo , target addr : 0x%08X len : 0x%04X", addr, len);
	retval = target_write_buffer(target, fls_algo_params.g_dstAddress, 4, &addr);
	retval = target_write_buffer(target, fls_algo_params.g_length, 4, &len);
	retval = target_write_buffer(target, fls_algo_params.g_func, 4, &func);

	int64_t run_algo_start = timeval_ms();
	retval = target_run_algorithm(target,
								  0, NULL,
								  0, NULL,
								  fls_algo_params.__bkpt_label,
								  fls_algo_params.__bkpt_label,
								  10000, NULL);
	log_info("run erase algo %" PRId64 " ms.[%d sectors]", timeval_ms() - run_algo_start, last - first + 1);

	if (retval != ERROR_OK)
	{
		LOG_ERROR("Failed to execute algorithm at 0x%" TARGET_PRIxADDR ": %d",
				  fls_algo_params.__bkpt_label, retval);
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
	uint32_t func = 1;
	retval = target_write_buffer(target, fls_algo_params.g_func, 4, &func);
	while (words_count > 0)
	{
		uint32_t thisrun_words = fls_algo_params.g_rwBuffer_size / 4;

		/* Limit to the amount of data we actually want to write */
		if (thisrun_words > words_count)
			thisrun_words = words_count;

		/* Write data to buffer */
		uint32_t len = thisrun_words * 4;
		// int64_t write_data_start = timeval_ms();
		retval = target_write_buffer(target, fls_algo_params.g_rwBuffer, len, buffer);
		// log_info("load program data %" PRId64 " ms.[0x%X bytes]", timeval_ms() - write_data_start, len);
		if (retval != ERROR_OK)
			break;

		// write_data_start = timeval_ms();
		retval = target_write_buffer(target, fls_algo_params.g_dstAddress, 4, &address);
		// log_info("load data %" PRId64 " ms.[0x%X bytes]",  timeval_ms() - write_data_start, 4);
		retval = target_write_buffer(target, fls_algo_params.g_length, 4, &len);
		retval = target_run_algorithm(target,
									  0, NULL,
									  0, NULL,
									  fls_algo_params.__bkpt_label,
									  fls_algo_params.__bkpt_label,
									  10000, NULL);
		// log_info("run program algo %" PRId64 " ms.", timeval_ms() - write_data_start);
		// uint32_t res[1] = {0};
		// target_read_buffer(target, write_algorithm->address + PROGRAM_ARGS_OFFSET, sizeof(res), res);
		// log_info("++++++++res [%08X]+++++++++.", res[0]);

		if (retval != ERROR_OK)
		{
			LOG_ERROR("Failed to execute algorithm at 0x%" TARGET_PRIxADDR ": %d",
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
	// log_info("%s", __func__);
	struct target *target = bank->target;
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

#include <target/image.h>
#include <helper/configuration.h>
static int cw32_load_fls_algo(struct flash_bank *bank)
{
	struct target *target = bank->target;

	int retval;
	const char *algo_path;
	char *full_path = NULL;
	if (strcmp(bank->driver->name, "cw2225") == 0) {
		algo_path = CW2225_FLS_ALGO_FILE;
	}else if (strcmp(bank->driver->name, "cw2245") == 0) {
		algo_path = CW2245_FLS_ALGO_FILE;
	}else if (strcmp(bank->driver->name, "cw3065") == 0) {
		algo_path = CW3065_FLS_ALGO_FILE; 
	}else {
		return ERROR_FAIL;
	}

	full_path = find_file(algo_path);
	if (full_path == NULL) 
	{
		LOG_ERROR("Cannot find %s", algo_path);
		return ERROR_FAIL;
    }
    
    struct image image;
    uint32_t size;
    
    retval = image_open(&image, full_path, "elf");

extern int image_find_symbol(struct image *image, const char *symbol_name, 
                                uint32_t *address, uint32_t *size);
	retval = image_find_symbol(&image, "Reset_Handler", &fls_algo_params.start_addr,  &size);
	retval = image_find_symbol(&image, "__bkpt_label", &fls_algo_params.__bkpt_label,  &size);
	retval = image_find_symbol(&image, "g_rwBuffer", &fls_algo_params.g_rwBuffer,  &fls_algo_params.g_rwBuffer_size);
	retval = image_find_symbol(&image, "g_dstAddress", &fls_algo_params.g_dstAddress,  &size);
	retval = image_find_symbol(&image, "g_length", &fls_algo_params.g_length,  &size);
	retval = image_find_symbol(&image, "g_func", &fls_algo_params.g_func,  &size);

    log_info("Symbol '%s' found at address: 0x%08x size %d", "Reset_Handler", fls_algo_params.start_addr,  size);
    log_info("Symbol '%s' found at address: 0x%08x size %d", "__bkpt_label", fls_algo_params.__bkpt_label,  size);
    log_info("Symbol '%s' found at address: 0x%08x size %d", "g_rwBuffer", fls_algo_params.g_rwBuffer,  fls_algo_params.g_rwBuffer_size);
    log_info("Symbol '%s' found at address: 0x%08x size %d", "g_dstAddress", fls_algo_params.g_dstAddress,  size);
    log_info("Symbol '%s' found at address: 0x%08x size %d", "g_length", fls_algo_params.g_length,  size);
    log_info("Symbol '%s' found at address: 0x%08x size %d", "g_func", fls_algo_params.g_func,  size);

	uint8_t *buffer;
	size_t buf_cnt;
	uint32_t image_size;
	target_addr_t min_address = 0;
	target_addr_t max_address = -1;

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

		uint32_t offset = 0;
		uint32_t length = buf_cnt;

		/* DANGER!!! beware of unsigned comparison here!!! */

		if ((image.sections[i].base_address + buf_cnt >= min_address) &&
				(image.sections[i].base_address < max_address)) {

			if (image.sections[i].base_address < min_address) {
				/* clip addresses below */
				offset += min_address-image.sections[i].base_address;
				length -= offset;
			}

			if (image.sections[i].base_address + buf_cnt > max_address)
				length -= (image.sections[i].base_address + buf_cnt)-max_address;

			retval = target_write_buffer(target,
					image.sections[i].base_address + offset, length, buffer + offset);
			if (retval != ERROR_OK) {
				free(buffer);
				break;
			}
			image_size += length;
			log_info("%u bytes written at address " TARGET_ADDR_FMT "",
					(unsigned int)length,
					image.sections[i].base_address + offset);
		}

		free(buffer);
	}

	if ((retval == ERROR_OK) && (duration_measure(&bench) == ERROR_OK)) {
		log_info("load flash algo %" PRIu32 " bytes "
				"in %fs (%0.3f KiB/s)", image_size,
				duration_elapsed(&bench), duration_kbps(&bench, image_size));
	}
	
	retval = target_run_algorithm(target,
								  0, NULL,
								  0, NULL,
								  fls_algo_params.start_addr,
								  fls_algo_params.__bkpt_label,
								  10000, NULL);

    image_close(&image);

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

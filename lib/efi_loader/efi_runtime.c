/*
 *  EFI application runtime services
 *
 *  Copyright (c) 2016 Alexander Graf
 *
 *  SPDX-License-Identifier:     GPL-2.0+
 */

#include <common.h>
#include <efi_loader.h>
#include <command.h>
#include <asm/global_data.h>
#include <rtc.h>

/* For manual relocation support */
DECLARE_GLOBAL_DATA_PTR;

static efi_status_t EFI_RUNTIME_TEXT efi_unimplemented(void);
static efi_status_t EFI_RUNTIME_TEXT efi_device_error(void);
static efi_status_t EFI_RUNTIME_TEXT efi_invalid_parameter(void);

#if defined(CONFIG_ARM64)
#define R_RELATIVE	1027
#define R_MASK		0xffffffffULL
#define IS_RELA		1
#elif defined(CONFIG_ARM)
#define R_RELATIVE	23
#define R_MASK		0xffULL
#else
#error Need to add relocation awareness
#endif

struct elf_rel {
	ulong *offset;
	ulong info;
};

struct elf_rela {
	ulong *offset;
	ulong info;
	long addend;
};

/*
 * EFI Runtime code lives in 2 stages. In the first stage, U-Boot and an EFI
 * payload are running concurrently at the same time. In this mode, we can
 * handle a good number of runtime callbacks
 */

static void efi_reset_system(enum efi_reset_type reset_type,
			efi_status_t reset_status, unsigned long data_size,
			void *reset_data)
{
	EFI_ENTRY("%d %lx %lx %p", reset_type, reset_status, data_size, reset_data);

	switch (reset_type) {
	case EFI_RESET_COLD:
	case EFI_RESET_WARM:
		do_reset(NULL, 0, 0, NULL);
		break;
	case EFI_RESET_SHUTDOWN:
		/* We don't have anything to map this to */
		break;
	}

	EFI_EXIT(EFI_SUCCESS);
}

static efi_status_t efi_get_time(struct efi_time *time,
			  struct efi_time_cap *capabilities)
{
#ifdef CONFIG_CMD_DATE

	struct rtc_time tm;
	int r;
#ifdef CONFIG_DM_RTC
	struct udevice *dev;
#endif

	EFI_ENTRY("%p %p", time, capabilities);

#ifdef CONFIG_DM_RTC
	r = uclass_get_device(UCLASS_RTC, 0, &dev);
	if (r)
		return EFI_EXIT(EFI_UNSUPPORTED);
#endif

#ifdef CONFIG_DM_RTC
	r = dm_rtc_get(dev, &tm);
#else
	r = rtc_get(&tm);
#endif
	if (r)
		return EFI_EXIT(EFI_UNSUPPORTED);

	memset(time, 0, sizeof(*time));
	time->year = tm.tm_year;
	time->month = tm.tm_mon;
	time->day = tm.tm_mday;
	time->hour = tm.tm_hour;
	time->minute = tm.tm_min;
	time->daylight = tm.tm_isdst;

	return EFI_EXIT(EFI_SUCCESS);

#else /* CONFIG_CMD_DATE */

	return EFI_DEVICE_ERROR;

#endif /* CONFIG_CMD_DATE */
}

struct efi_runtime_detach_list_struct {
	void *ptr;
	void *patchto;
};

static const struct efi_runtime_detach_list_struct efi_runtime_detach_list[] = {
	{
		/* do_reset is gone */
		.ptr = &efi_runtime_services.reset_system,
		.patchto = &efi_unimplemented,
	}, {
		/* invalidate_*cache_all are gone */
		.ptr = &efi_runtime_services.set_virtual_address_map,
		.patchto = &efi_invalid_parameter,
	}, {
		/* RTC accessors are gone */
		.ptr = &efi_runtime_services.get_time,
		.patchto = &efi_device_error,
	},
};

static bool efi_runtime_tobedetached(void *p)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(efi_runtime_detach_list); i++)
		if (efi_runtime_detach_list[i].ptr == p)
			return true;

	return false;
}

static void efi_runtime_detach(ulong offset)
{
	int i;
	ulong patchoff = offset - (ulong)gd->relocaddr;

	for (i = 0; i < ARRAY_SIZE(efi_runtime_detach_list); i++) {
		ulong patchto = (ulong)efi_runtime_detach_list[i].patchto;
		ulong *p = efi_runtime_detach_list[i].ptr;
		ulong newaddr = patchto + patchoff;

#ifdef DEBUG_EFI
		printf("%s: Setting %p to %lx\n", __func__, p, newaddr);
#endif
		*p = newaddr;
	}
}

/* Relocate EFI runtime to uboot_reloc_base = offset */
void efi_runtime_relocate(ulong offset, struct efi_mem_desc *map)
{
#ifdef IS_RELA
	struct elf_rela *rel = (void*)&__efi_runtime_rel_start;
#else
	struct elf_rel *rel = (void*)&__efi_runtime_rel_start;
	static ulong lastoff = CONFIG_SYS_TEXT_BASE;
#endif

#ifdef DEBUG_EFI
	printf("%s: Relocating to offset=%lx\n", __func__, offset);
#endif

	for (; (ulong)rel < (ulong)&__efi_runtime_rel_stop; rel++) {
		ulong base = CONFIG_SYS_TEXT_BASE;
		ulong *p;
		ulong newaddr;

		p = (void*)((ulong)rel->offset - base) + gd->relocaddr;

		if ((rel->info & R_MASK) != R_RELATIVE) {
			continue;
		}

#ifdef IS_RELA
		newaddr = rel->addend + offset - CONFIG_SYS_TEXT_BASE;
#else
		newaddr = *p - lastoff + offset;
#endif

		/* Check if the relocation is inside bounds */
		if (map && ((newaddr < map->virtual_start) ||
		    newaddr > (map->virtual_start + (map->num_pages << 12)))) {
			if (!efi_runtime_tobedetached(p))
				printf("U-Boot EFI: Relocation at %p is out of "
				       "range (%lx)\n", p, newaddr);
			continue;
		}

#ifdef DEBUG_EFI
		printf("%s: Setting %p to %lx\n", __func__, p, newaddr);
#endif

		*p = newaddr;
		flush_dcache_range((ulong)p, (ulong)&p[1]);
	}

#ifndef IS_RELA
	lastoff = offset;
#endif

        invalidate_icache_all();
}

static efi_status_t efi_set_virtual_address_map(
			unsigned long memory_map_size,
			unsigned long descriptor_size,
			uint32_t descriptor_version,
			struct efi_mem_desc *virtmap)
{
	ulong runtime_start = (ulong)&__efi_runtime_start & ~0xfffULL;
	int n = memory_map_size / descriptor_size;
	int i;

	EFI_ENTRY("%lx %lx %x %p", memory_map_size, descriptor_size,
		  descriptor_version, virtmap);

	for (i = 0; i < n; i++) {
		struct efi_mem_desc *map;

		map = (void*)virtmap + (descriptor_size * i);
		if (map->type == EFI_RUNTIME_SERVICES_CODE) {
			ulong new_offset = map->virtual_start - (runtime_start - gd->relocaddr);

			efi_runtime_relocate(new_offset, map);
			/* Once we're virtual, we can no longer handle
			   complex callbacks */
			efi_runtime_detach(new_offset);
			return EFI_EXIT(EFI_SUCCESS);
		}
	}

	return EFI_EXIT(EFI_INVALID_PARAMETER);
}

/*
 * In the second stage, U-Boot has disappeared. To isolate our runtime code
 * that at this point still exists from the rest, we put it into a special
 * section.
 *
 *        !!WARNING!!
 *
 * This means that we can not rely on any code outside of this file in any
 * function or variable below this line.
 *
 * Please keep everything fully self-contained and annotated with
 * EFI_RUNTIME_TEXT and EFI_RUNTIME_DATA markers.
 */

/*
 * Relocate the EFI runtime stub to a different place. We need to call this
 * the first time we expose the runtime interface to a user and on set virtual
 * address map calls.
 */

static efi_status_t EFI_RUNTIME_TEXT efi_unimplemented(void)
{
	return EFI_UNSUPPORTED;
}

static efi_status_t EFI_RUNTIME_TEXT efi_device_error(void)
{
	return EFI_DEVICE_ERROR;
}

static efi_status_t EFI_RUNTIME_TEXT efi_invalid_parameter(void)
{
	return EFI_INVALID_PARAMETER;
}

struct efi_runtime_services EFI_RUNTIME_DATA efi_runtime_services = {
	.hdr = {
		.signature = EFI_RUNTIME_SERVICES_SIGNATURE,
		.revision = EFI_RUNTIME_SERVICES_REVISION,
		.headersize = sizeof(struct efi_table_hdr),
	},
	.get_time = &efi_get_time,
	.set_time = (void *)&efi_device_error,
	.get_wakeup_time = (void *)&efi_unimplemented,
	.set_wakeup_time = (void *)&efi_unimplemented,
	.set_virtual_address_map = &efi_set_virtual_address_map,
	.convert_pointer = (void *)&efi_invalid_parameter,
	.get_variable = (void *)&efi_device_error,
	.get_next_variable = (void *)&efi_device_error,
	.set_variable = (void *)&efi_device_error,
	.get_next_high_mono_count = (void *)&efi_device_error,
	.reset_system = &efi_reset_system,
};
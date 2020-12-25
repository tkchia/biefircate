#include <efi.h>
#include <efilib.h>
#include "efi-stuff.h"
#include "loader.h"

extern EFI_HANDLE LibImageHandle;

static UINT64 base_mem_start = 0, base_mem_end = 0;

static UINT64 carve_out_base_mem(unsigned kib)
{
	base_mem_end -= kib * 1024ULL;
	return base_mem_end;
}

void mem_map_init(UINTN *mem_map_key)
{
	EFI_MEMORY_DESCRIPTOR *desc;
	UINTN num_entries = 0, desc_sz;
	UINT32 desc_ver;
	desc = LibMemoryMap(&num_entries, mem_map_key, &desc_sz, &desc_ver);
	if (!num_entries)
		error(u"cannot get memory map!");
	cputws(u"memory map below 16 MiB:\n"
		"  start    end       type attrs\n");
	while (num_entries-- != 0) {
		EFI_PHYSICAL_ADDRESS start, end;
		start = desc->PhysicalStart;
		if (start > 0xffffffUL)
			continue;
		end = start + desc->NumberOfPages * EFI_PAGE_SIZE;
		cwprintf(u"  0x%06lx 0x%06lx%c %4u 0x%016lx\n", start,
		    end > 0x1000000ULL ? (UINT64)0x1000000ULL - 1 : end - 1,
		    end > 0x1000000ULL ? u'+' : u' ',
		    (UINT32)desc->Type, desc->Attribute);
		switch (desc->Type) {
		    default:
			break;
		    case EfiBootServicesCode:
		    case EfiBootServicesData:
		    case EfiConventionalMemory:
			if (start < 0xf0000ULL) {
				if (end > 0xf0000ULL)
					end = 0xf0000ULL;
				if (!base_mem_end) {
					base_mem_start = start;
					base_mem_end = end;
				} else if (end == base_mem_start)
					base_mem_start = start;
				  else if (start == base_mem_end)
					base_mem_end = end;
			}
		}
		/* :-| */
		desc = (EFI_MEMORY_DESCRIPTOR *)((char *)desc + desc_sz);
	}
}

void stage1_done(UINTN mem_map_key)
{
	EFI_STATUS status = BS->ExitBootServices(LibImageHandle, mem_map_key);
	UINT64 reserved_base_mem;
	if (EFI_ERROR(status))
		error_with_status(u"cannot exit UEFI boot services", status);
	fb_con_instate();
	reserved_base_mem = carve_out_base_mem(1);
	lm86_rm86_init((uint16_t)(reserved_base_mem >> 4));
}
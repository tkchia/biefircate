#define GNU_EFI_USE_MS_ABI

#include <efi.h>
#include <efilib.h>
#include <string.h>
#include "rm86.h"

typedef struct _EFI_LEGACY_BIOS_PROTOCOL EFI_LEGACY_BIOS_PROTOCOL;

extern EFI_HANDLE LibImageHandle;
extern EFI_GUID gEfiLoadedImageProtocolGuid, gEfiGlobalVariableGuid;

static BOOLEAN secure_boot_p = FALSE;
static EFI_HANDLE boot_media_handle;
static UINT64 base_mem_start = 0, base_mem_end = 0;

static void wait_and_exit(EFI_STATUS status)
{
	Output(u"press a key to exit\r\n");
	WaitForSingleEvent(ST->ConIn->WaitForKey, 0);
	Exit(status, 0, NULL);
}

static void error_with_status(IN CONST CHAR16 *msg, EFI_STATUS status)
{
	Print(u"error: %s: %d\r\n", msg, (INT32)status);
	wait_and_exit(status);
}

static void error(IN CONST CHAR16 *msg)
{
	Print(u"error: %s\r\n", msg);
	wait_and_exit(EFI_ABORTED);
}

static void process_memory_map(void)
{
	EFI_MEMORY_DESCRIPTOR *desc;
	UINTN num_entries = 0, map_key, desc_sz;
	UINT32 desc_ver;
	desc = LibMemoryMap(&num_entries, &map_key, &desc_sz, &desc_ver);
	if (!num_entries)
		error(u"cannot get memory map!");
	Output(u"memory map below 16 MiB:\r\n"
		"  start    end       type attrs\r\n");
	while (num_entries-- != 0) {
		EFI_PHYSICAL_ADDRESS start, end;
		start = desc->PhysicalStart;
		if (start > 0xffffffUL)
			continue;
		end = start + desc->NumberOfPages * EFI_PAGE_SIZE;
		Print(u"  0x%06lx 0x%06lx%c %4u 0x%016lx\r\n", start,
		    end > 0x1000000ULL ? (UINT64)0x1000000ULL - 1 : end - 1,
		    end > 0x1000000ULL ? u'+' : u' ',
		    (UINT32)desc->Type, desc->Attribute);
		if (!base_mem_start &&
		    desc->Type == EfiConventionalMemory &&
		    start < 0xf0000ULL) {
			if (start > EFI_PAGE_SIZE)
				base_mem_start = start;
			else
				base_mem_start = EFI_PAGE_SIZE;
			if (end < 0xf0000ULL)
				base_mem_end = end;
			else
				base_mem_end = 0xf0000ULL;
		}
		/* :-| */
		desc = (EFI_MEMORY_DESCRIPTOR *)((char *)desc + desc_sz);
	}
}

static void find_boot_media(void)
{
	EFI_STATUS status;
	EFI_LOADED_IMAGE_PROTOCOL *li;
	status = BS->HandleProtocol(LibImageHandle,
	    &gEfiLoadedImageProtocolGuid, (void **)&li);
	if (EFI_ERROR(status))
		error_with_status(u"cannot get EFI_LOADED_IMAGE_PROTOCOL",
		    status);
	boot_media_handle = li->DeviceHandle;
}

static void test_if_secure_boot(void)
{
	UINT8 data = 0;
	UINTN data_sz = sizeof(data);
	EFI_STATUS status = RT->GetVariable(u"SecureBoot",
	    &gEfiGlobalVariableGuid, NULL, &data_sz, &data);
	if (!EFI_ERROR(status) && data)
		secure_boot_p = TRUE;
	Print(u"secure boot: %s\r\n", secure_boot_p ? u"yes" : u"no");
}

static void find_pci(void)
{
	EFI_HANDLE *handles;
	UINTN num_handles;
	EFI_STATUS status = LibLocateHandle(ByProtocol,
	    &gEfiPciRootBridgeIoProtocolGuid, NULL, &num_handles, &handles);
	if (EFI_ERROR(status))
	        error_with_status(u"no PCI root bridges found", status);
	Print(u"PCI root bridges: %lu\n", num_handles);
	FreePool(handles);
}

static void init_trampolines(void)
{
	EFI_PHYSICAL_ADDRESS addr = base_mem_end - EFI_PAGE_SIZE;
	EFI_STATUS status = BS->AllocatePages(AllocateAddress, EfiLoaderData,
	    1, &addr);
	if (EFI_ERROR(status))
		error_with_status(u"cannot allocate trampoline memory",
		    status);
	Print(u"allocated real mode trampolines @0x%lx\r\n", addr);
	rm86_set_trampolines_seg(addr >> 4);
}

static char *alloc_dos_mem(UINTN bytes)
{
	EFI_PHYSICAL_ADDRESS addr = base_mem_start;
	UINTN pages = (bytes + EFI_PAGE_SIZE - 1) / EFI_PAGE_SIZE;
	EFI_STATUS status = BS->AllocatePages(AllocateAddress, EfiLoaderData,
	    pages, &addr);
	if (EFI_ERROR(status))
		error_with_status(u"cannot allocate DOS memory", status);
	Print(u"allocated 0x%x pages of DOS memory @0x%lx\r\n", pages, addr);
	return (char *)addr;
}

static char *alloc_dos_mem_with_psp(UINTN bytes)
{
	char *mem = alloc_dos_mem(bytes);
	mem[0x0000] = 0xcd;
	mem[0x0001] = 0x20;
	mem[0x0050] = 0xcd;
	mem[0x0051] = 0x21;
	mem[0x0052] = 0xcb;
	mem[0x0080] = 0x03;
	mem[0x0081] = ' ';
	mem[0x0082] = '/';
	mem[0x0083] = 'P';
	mem[0x0084] = '\r';
	Print(u"filled in stub PSP @0x%lx\r\n", mem);
	return mem;
}

#define STAGE2		u"biefirc2.sys"

static void load_stage2(void)
{
	const UINTN dos_mem_size = 0x10000UL;
	UINTN read_size = dos_mem_size - 0x200UL;
	EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs;
	EFI_FILE_PROTOCOL *vol, *prog;
	EFI_FILE_INFO *info;
	EFI_STATUS status;
	rm86_regs_t *regs;
	char *psp = alloc_dos_mem_with_psp(dos_mem_size);
	status = BS->HandleProtocol(boot_media_handle,
	    &gEfiSimpleFileSystemProtocolGuid, (void **)&fs);
	if (EFI_ERROR(status))
		error_with_status(u"cannot get "
		    "EFI_SIMPLE_FILE_SYSTEM_PROTOCOL", status);
	status = fs->OpenVolume(fs, &vol);
	if (EFI_ERROR(status))
		error_with_status(u"cannot get EFI_FILE_PROTOCOL", status);
	status = vol->Open(vol, &prog, STAGE2, EFI_FILE_MODE_READ, 0);
	if (EFI_ERROR(status)) {
		vol->Close(vol);
		error_with_status(u"cannot open " STAGE2, status);
	}
	info = LibFileInfo(prog);
	if (!info)
		error(u"cannot get info on " STAGE2);
	Print(u"stage2: %s  size: 0x%lx  attrs.: 0x%lx\n",
	    STAGE2, info->FileSize, info->Attribute);
	FreePool(info);
	status = prog->Read(prog, &read_size, psp + 0x100);
	prog->Close(prog);
	vol->Close(vol);
	if (EFI_ERROR(status))
		error_with_status(u"cannot read " STAGE2, status);
	Print(u"read 0x%x byte%s from %s\r\n", read_size,
	    read_size == 1 ? u"" : u"s", STAGE2);
	psp[0xfffe] = psp[0xffff] = 0;
	regs = rm86_regs();
	memset(regs, 0, sizeof *regs);
	regs->cs = regs->ss = (UINT32)(UINT64)psp >> 4;
	regs->ip = 0x0100;
	regs->esp = 0xfffe;
}

static void run_stage2(void)
{
	rm86();
	Output(STAGE2 u" finished\r\n");
}

EFI_STATUS efi_main(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *system_table)
{
	InitializeLib(image_handle, system_table);
	Output(u".:. biefircate " VERSION " .:.\r\n");
	process_memory_map();
	find_boot_media();
	test_if_secure_boot();
	find_pci();
	init_trampolines();
	load_stage2();
	run_stage2();
	wait_and_exit(0);
	return 0;
}

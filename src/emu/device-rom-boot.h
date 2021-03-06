//
//  device-boot.h
//

#ifndef rv_device_boot_h
#define rv_device_boot_h

#include "boot-rom-bin.h"

namespace riscv {

	/* BOOT MMIO device */

	template <typename P>
	struct boot_mmio_device : memory_segment<typename P::ux>
	{
		typedef typename P::ux UX;
		typedef memory_segment<typename P::ux> seg_type;

		P &proc;

		constexpr u8* as_u8() { return (u8*)&build_riscv64_unknown_elf_bin_boot_rom_bin[0]; }
		constexpr u16* as_u16() { return (u16*)&build_riscv64_unknown_elf_bin_boot_rom_bin[0]; }
		constexpr u32* as_u32() { return (u32*)&build_riscv64_unknown_elf_bin_boot_rom_bin[0]; }
		constexpr u64* as_u64() { return (u64*)&build_riscv64_unknown_elf_bin_boot_rom_bin[0]; }

		/* BOOT constructor */

		boot_mmio_device(P &proc, UX mpa) :
			memory_segment<UX>("ROM", mpa, /*uva*/0,
				/*size*/build_riscv64_unknown_elf_bin_boot_rom_bin_len,
				pma_type_main | pma_prot_read | pma_prot_execute), proc(proc) {}

		/* BOOT MMIO */

		void load_8 (UX va, u8  &val)
		{
			val = (va < seg_type::size) ? *(as_u8() + va) : 0;
			if (proc.log & proc_log_mmio) {
				printf("boot_mmio:0x%04llx -> 0x%02hhx\n", addr_t(va), val);
			}
		}

		void load_16(UX va, u16 &val)
		{
			val = (va < seg_type::size - 1) ? *(as_u16() + (va>>1)) : 0;
			if (proc.log & proc_log_mmio) {
				printf("boot_mmio:0x%04llx -> 0x%04hx\n", addr_t(va), val);
			}
		}

		void load_32(UX va, u32 &val)
		{
			val = (va < seg_type::size - 3) ? *(as_u32() + (va>>2)) : 0;
			if (proc.log & proc_log_mmio) {
				printf("boot_mmio:0x%04llx -> 0x%08x\n", addr_t(va), val);
			}
		}

		void load_64(UX va, u64 &val)
		{
			val = (va < seg_type::size - 7) ? *(as_u64() + (va>>3)) : 0;
			if (proc.log & proc_log_mmio) {
				printf("boot_mmio:0x%04llx -> 0x%016llx\n", addr_t(va), val);
			}
		}

		void store_8 (UX va, u8  val)
		{
			if (proc.log & proc_log_mmio) {
				printf("boot_mmio:0x%04llx <- 0x%02hhx\n", addr_t(va), val);
			}
			if (va < seg_type::size) *(as_u8() + va) = val;
		}

		void store_16(UX va, u16 val)
		{
			if (proc.log & proc_log_mmio) {
				printf("boot_mmio:0x%04llx <- 0x%04hx\n", addr_t(va), val);
			}
			if (va < seg_type::size - 1) *(as_u16() + (va>>1)) = val;
		}

		void store_32(UX va, u32 val)
		{
			if (proc.log & proc_log_mmio) {
				printf("boot_mmio:0x%04llx <- 0x%08x\n", addr_t(va), val);
			}
			if (va < seg_type::size - 3) *(as_u32() + (va>>2)) = val;
		}

		void store_64(UX va, u64 val)
		{
			if (proc.log & proc_log_mmio) {
				printf("boot_mmio:0x%04llx <- 0x%016llx\n", addr_t(va), val);
			}
			if (va < seg_type::size - 7) *(as_u64() + (va>>3)) = val;
		}

	};

}

#endif

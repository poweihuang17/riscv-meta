//
//  mmu-soft.h
//

#ifndef rv_mmu_soft_h
#define rv_mmu_soft_h

namespace riscv {

	template <typename UX, typename TLB, typename PMA, typename MEMORY = user_memory<UX>>
	struct mmu_soft
	{
		typedef TLB    tlb_type;
		typedef PMA    pma_type;

		typedef std::shared_ptr<MEMORY> memory_type;

		enum mmu_op {
			op_fetch,
			op_load,
			op_store
		};

		/* MMU properties */

		tlb_type       l1_itlb;     /* L1 Instruction TLB */
		tlb_type       l1_dtlb;     /* L1 Data TLB */
		pma_type       pma;         /* PMA table */
		memory_type    mem;         /* memory device */

		/* MMU constructor */

		mmu_soft() : mem(std::make_shared<MEMORY>()) {}
		mmu_soft(memory_type mem) : mem(mem) {}

		/* MMU methods */

		template <typename T> constexpr bool misaligned(UX va)
		{
			return (va & (sizeof(T) - 1)) != 0;
		}

		template <typename P> constexpr bool fetch_access_fault(
			P &proc, UX privilege_level, addr_t pa,
			typename tlb_type::tlb_entry_t* tlb_ent)
		{
			/*
			 * Checks for X=1
			 * U=1 pages fault if effective translation privilevel level is > U
			 * U=0 pages fault if effective translation privilevel level is < S
			 *
			 * MXR and PUM do not affect instruction fetches
			 */
			return
				(tlb_ent && (
					!(tlb_ent->pteb & pte_flag_X) ||
					((tlb_ent->pteb & pte_flag_U) ?
						privilege_level > rv_mode_U :
						privilege_level < rv_mode_S)
				)
			);
		}

		template <typename P> constexpr bool load_access_fault(
			P &proc, UX privilege_level, addr_t pa,
			typename tlb_type::tlb_entry_t* tlb_ent)
		{
			/*
			 * Checks for R=1 or (X=1 and MXR=1)
			 * U=1 pages fault if effective translation privilevel level is > U && PUM=1
			 * U=0 pages fault if effective translation privilevel level is < S
			 */
			return
				(tlb_ent && (
					(!(tlb_ent->pteb & pte_flag_R) &&
					 !(tlb_ent->pteb & pte_flag_X && proc.mstatus.r.mxr)) ||
					((tlb_ent->pteb & pte_flag_U) ?
						privilege_level > rv_mode_U && proc.mstatus.r.pum :
						privilege_level < rv_mode_S)
				)
			);
		}

		template <typename P> constexpr bool store_access_fault(
			P &proc, UX privilege_level, addr_t pa,
			typename tlb_type::tlb_entry_t* tlb_ent)
		{
			/*
			 * Checks for W=1
			 * U=1 pages fault if effective translation priv_level is > U && PUM=1
			 * U=0 pages fault if effective translation priv_level is < S
			 */
			return
				(tlb_ent && (
					!(tlb_ent->pteb & pte_flag_W) ||
					((tlb_ent->pteb & pte_flag_U) ?
						privilege_level > rv_mode_U && proc.mstatus.r.pum :
						privilege_level < rv_mode_S)
				)
			);
		}

		/* instruction fetch */
		template <typename P, const mmu_op op = op_fetch>
		inst_t inst_fetch(P &proc, UX pc, addr_t &pc_offset)
		{
			typename tlb_type::tlb_entry_t* tlb_ent = nullptr;
			memory_segment<UX> *segment = nullptr;
			inst_t inst = 0;

			/* raise exception if address is misalligned */
			if (unlikely(misaligned<u16>(pc))) {
				proc.raise(rv_cause_misaligned_fetch, pc);
			}

			/* translate to machine physical (raises exception on fault) */
			addr_t mpa = translate_addr<P,op>(proc, pc, tlb_ent);

			/* translate to user virtual (null segment indicates no mapping) */
			addr_t uva = mem->mpa_to_uva(segment, mpa);

			/* Check PTE flags */
			if (unlikely(!segment ||
				fetch_access_fault(proc, proc.mode, uva, tlb_ent)))
			{
				proc.raise(rv_cause_fault_fetch, pc);
			} else {

				/* record pc histogram using machine physical address */
				if (proc.log & proc_log_hist_pc) {
					proc.histogram_add_pc(mpa);
				}

				/* fetch instruction using memory segment interface */
				u32 inst_32;
				segment->load(uva, inst_32);
				inst = htole32(inst_32);
				if ((inst & 0b11) != 0b11) {
					inst &= 0xffff; // mask to 16-bits
					pc_offset = 2;
				} else if ((inst & 0b11100) != 0b11100) {
					pc_offset = 4;
				} else if ((inst & 0b111111) == 0b011111) {
					u16 inst_16;
					segment->load(uva + 4, inst_16);
					inst |= inst_t(htole16(inst_16)) << 32;
					pc_offset = 6;
				} else if ((inst & 0b1111111) == 0b0111111) {
					segment->load(uva + 4, inst_32);
					inst |= inst_t(htole32(inst_32)) << 32;
					pc_offset = 8;
				} else {
					proc.raise(rv_cause_fault_fetch, pc);
				}
			}
			return inst;
		}

		/* amo */
		template <typename P, typename T, const mmu_op op = op_load>
		void amo(P &proc, const amo_op a_op, UX va, T &val1, T val2)
		{
			typename tlb_type::tlb_entry_t* tlb_ent = nullptr;
			memory_segment<UX> *segment = nullptr;

			/* raise exception if address is misalligned */
			if (unlikely(misaligned<T>(va))) {
				proc.raise(rv_cause_misaligned_load, va);
			}

			/* translate to machine physical (raises exception on fault) */
			addr_t mpa = translate_addr<P,op>(proc, va, tlb_ent);

			/* translate to user virtual (null segment indicates no mapping) */
			addr_t uva = mem->mpa_to_uva(segment, mpa);

			/* Check PTE flags */
			if (unlikely(!segment ||
				load_access_fault(proc, proc.mode, uva, tlb_ent)) ||
				store_access_fault(proc, proc.mode, uva, tlb_ent))
			{
				proc.raise(rv_cause_fault_load, va);
			} else {
				/* TODO - we need some locking magic for SMP on non RISC-V */
				segment->load(uva, val1);
				val2 = amo_fn<UX>(a_op, val1, val2);
				segment->store(uva, val2);
			}
		}

		/* load */
		template <typename P, typename T, const mmu_op op = op_load>
		void load(P &proc, UX va, T &val)
		{
			typename tlb_type::tlb_entry_t* tlb_ent = nullptr;
			memory_segment<UX> *segment = nullptr;

			/* raise exception if address is misalligned */
			if (unlikely(misaligned<T>(va))) {
				proc.raise(rv_cause_misaligned_load, va);
			}

			/* translate to machine physical (raises exception on fault) */
			addr_t mpa = translate_addr<P,op>(proc, va, tlb_ent);

			/* translate to user virtual (null segment indicates no mapping) */
			addr_t uva = mem->mpa_to_uva(segment, mpa);

			/* Check PTE flags */
			if (unlikely(!segment ||
				load_access_fault(proc, proc.mode, uva, tlb_ent)))
			{
				proc.raise(rv_cause_fault_load, va);
			} else {
				segment->load(uva, val);
			}
		}

		/* store */
		template <typename P, typename T, const mmu_op op = op_store>
		void store(P &proc, UX va, T val)
		{
			typename tlb_type::tlb_entry_t* tlb_ent = nullptr;
			memory_segment<UX> *segment = nullptr;

			/* raise exception if address is misalligned */
			if (unlikely(misaligned<T>(va))) {
				proc.raise(rv_cause_misaligned_store, va);
			}

			/* translate to machine physical (raises exception on fault) */
			addr_t mpa = translate_addr<P,op>(proc, va, tlb_ent);

			/* translate to user virtual (null segment indicates no mapping) */
			addr_t uva = mem->mpa_to_uva(segment, mpa);

			/* Check PTE flags */
			if (unlikely(!segment ||
				store_access_fault(proc, proc.mode, uva, tlb_ent)))
			{
				proc.raise(rv_cause_fault_store, va);
			} else {
				segment->store(uva, val);
			}
		}

		template <typename P> constexpr UX effective_mode(P &proc, const mmu_op op)
		{
			/*
			 * effective privilege mode for page translation is either the current
			 * privilege mode (U or S) or M mode with MPRV set and the mode in MPP
			 * (U or S). M mode instruction fetches are not page translated
			 */
			return ((proc.mode >= rv_mode_M) &&
				(proc.mstatus.r.mprv == 1) &&
				(op != op_fetch) &&
				(proc.mstatus.r.mpp <= rv_mode_S))
				? proc.mstatus.r.mpp
				: proc.mode;
		}

		/* translate address based on processor translation mode */
		template <typename P, mmu_op op> addr_t translate_addr(
			P &proc, UX va, typename tlb_type::tlb_entry_t* &tlb_ent)
		{
			UX effective_privilege_level = effective_mode(proc, op);
			if (effective_privilege_level >= rv_mode_M) {
				return va;
			} else {
				switch (proc.mstatus.r.vm) {
					case rv_vm_mbare:
						return va;
					case rv_vm_sv32:
						return page_translate_addr<P,sv32>(proc, va, op,
							op == op_fetch ? l1_itlb : l1_dtlb, tlb_ent);
					case rv_vm_sv39:
						return page_translate_addr<P,sv39>(proc, va, op,
							op == op_fetch ? l1_itlb : l1_dtlb, tlb_ent);
					case rv_vm_sv48:
						return page_translate_addr<P,sv48>(proc, va, op,
							op == op_fetch ? l1_itlb : l1_dtlb, tlb_ent);
					default:
						panic("unsupported vm mode");
				}
			}
			return 0; /* not reached */
		}

		template <typename PTM>
		inline constexpr addr_t page_translate_offset(UX ppn, UX va, UX level)
		{
			const UX shift = PTM::bits * level + page_shift;
			return (ppn << page_shift) + (va & ((1ULL << shift) - 1));
		}

		/* translate address using a TLB and a paged addressing mode */
		template <typename P, typename PTM> addr_t page_translate_addr(
			P &proc, UX va, mmu_op op,
			tlb_type &tlb, typename tlb_type::tlb_entry_t* &tlb_ent
		)
		{
			tlb_ent = tlb.lookup(proc.pdid, proc.sptbr >> tlb_type::ppn_bits, va);
			if (tlb_ent) {
				/* update PTE accessed and dirty flags */
				update_pte_flags<PTM>(op, tlb_ent->uva);

				return page_translate_offset<PTM>(tlb_ent->ppn, va, tlb_ent->ptel);
			} else {
				return page_translate_addr_tlb_miss<P,PTM>(proc, va, op, tlb, tlb_ent);
			}
		}

		/* translate address using a TLB and a paged addressing mode
		 * TLB miss slow path that invokes the page table walker */
		template <typename P, typename PTM> addr_t page_translate_addr_tlb_miss(
			P &proc, UX va, mmu_op op,
			tlb_type &tlb, typename tlb_type::tlb_entry_t* &tlb_ent)
		{
			/*
			 * The simple direct mapped TLB implementation currently maps page_size
			 * entries so as a byproduct, the code will rewalk the page table every
			 * page_size interval, even if the PTE points to a megapage or gigapage.
			 * Can be solved by adding a secondary TLB with larger scoped entries.
			 */

			typename PTM::pte_type pte;
			addr_t pte_uva;
			UX level;

			/* TODO: TLB statistics */

			/* Walk the page table to find a leaf PTE entry
			 * (access fault is raised if leaf PTE is not found) */
			addr_t pa = walk_page_table<P,PTM>(proc, va, op, tlb, tlb_ent,
				pte, pte_uva, level);

			/* Insert the virtual to physical mapping into the TLB */
			tlb_ent = tlb.insert(proc.pdid, proc.sptbr >> tlb_type::ppn_bits,
				va, level, pte.val.flags, pte.val.ppn, pte_uva);

			return pa;
		}

		/* update PTE accessed and dirty */
		template <typename PTM> void update_pte_flags(mmu_op op, addr_t pte_uva)
		{
			typedef typename PTM::pte_type pte_type;
			pte_type &pte = *(pte_type*)pte_uva;
			UX update_flags = (op == op_store ? (pte_flag_D | pte_flag_A) : pte_flag_A);
			if (pte.val.flags != (pte.val.flags | update_flags)) {
				pte.val.flags |= update_flags;
			}
		}

		/* walk the page table to find a PTE for a given virtual address */
		template <typename P, typename PTM> addr_t walk_page_table(
			P &proc, UX va, mmu_op op,
			tlb_type &tlb, typename tlb_type::tlb_entry_t* &tlb_ent,
			typename PTM::pte_type &pte, addr_t &pte_uva, UX &level)
		{
			typedef typename PTM::pte_type pte_type;

			UX ppn = (proc.sptbr & ((1ULL << tlb_type::ppn_bits) - 1)) << page_shift;
			UX vpn, pte_mpa, shift;

			/* TODO: canonical address check */

			/* walk the page table */
			for (level = PTM::levels - 1; level >= 0; level--) {

				/* calculate the shift for this page table level */
				shift = PTM::bits * level + page_shift;
				vpn = (va >> shift) & ((1ULL << PTM::bits) - 1);
				pte_mpa = ppn + vpn * sizeof(pte_type);

				/* map the ppn into the host address space */
				memory_segment<UX> *segment = nullptr;
				pte_uva = mem->mpa_to_uva(segment, pte_mpa);
				if (!segment) goto fault;
				segment->load(pte_uva, pte);

				/* check if this is a pointer PTE */
				if ((((pte.xu.val >> pte_shift_R) |
					  (pte.xu.val >> pte_shift_W) |
					  (pte.xu.val >> pte_shift_X)) & 1) == 0)
				{
					ppn = pte.val.ppn << page_shift;
					continue;
				};

				/* If pte.v = 0, or if pte.r = 0 and pte.w = 1, raise an access exception */
				if (((~pte.val.flags >> pte_shift_V) |
					((~pte.val.flags >> pte_shift_R) & (pte.val.flags >> pte_shift_W))) & 1) goto fault;

				/* translate address if we have a valid PTE */
				if ((pte.val.flags & (pte_flag_R | pte_flag_X))) {

					/* update PTE accessed and dirty flags */
					update_pte_flags<PTM>(op, pte_uva);

					if (proc.log & proc_log_pagewalk) {
						debug("walk_page_table va=0x%llx sptbr=0x%llx, level=%d "
							"ppn=0x%llx vpn=0x%llx pte=0x%llx -> addr=0x%llx",
							(addr_t)va, (addr_t)proc.sptbr, level, (addr_t)ppn,
							(addr_t)vpn, (addr_t)pte.xu.val,
							page_translate_offset<PTM>(pte.val.ppn, va, level));
					}

					/* translate address taking into account PTE level */
					return page_translate_offset<PTM>(pte.val.ppn, va, level);
				}
			}

		fault:
			if (proc.log & proc_log_pagewalk) {
				debug("walk_page_table va=0x%llx sptbr=0x%llx, level=%d "
					"ppn=0x%llx vpn=0x%llx pte=0x%llx -> translation fault",
					(addr_t)va, (addr_t)proc.sptbr, level, (addr_t)ppn,
					(addr_t)vpn, (addr_t)pte.xu.val);
			}

			switch (op) {
				case op_fetch: proc.raise(rv_cause_fault_fetch, va);
				case op_load:  proc.raise(rv_cause_fault_load, va);
				case op_store: proc.raise(rv_cause_fault_store, va);
			}

			return 0; /* not reached */
		}
	};

	typedef tagged_tlb_rv32<128> tlb_type_rv32;
	typedef tagged_tlb_rv64<128> tlb_type_rv64;

	typedef pma_table<u32,8> pma_table_rv32;
	typedef pma_table<u64,8> pma_table_rv64;

	using mmu_soft_rv32 = mmu_soft<u32,tlb_type_rv32,pma_table_rv32>;
	using mmu_soft_rv64 = mmu_soft<u64,tlb_type_rv64,pma_table_rv64>;

}

#endif

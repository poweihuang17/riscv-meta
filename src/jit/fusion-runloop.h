//
//  fusion-runloop.h
//

#ifndef rv_fusion_runloop_h
#define rv_fusion_runloop_h

namespace riscv {

	struct fusion_fault
	{
		static fusion_fault *current;
	};

	fusion_fault* fusion_fault::current = nullptr;

	template <typename P>
	struct fusion_runloop : fusion_fault, ErrorHandler, P
	{
		static const size_t inst_cache_size = 8191;
		static const int inst_step = 100000;

		struct rv_inst_cache_ent
		{
			inst_t inst;
			typename P::decode_type dec;
		};

		typedef void (*TraceFunc)(typename P::processor_type *);

		JitRuntime rt;
		google::dense_hash_map<addr_t,TraceFunc> trace_cache;
		std::shared_ptr<debug_cli<P>> cli;
		rv_inst_cache_ent inst_cache[inst_cache_size];

		fusion_runloop() : fusion_runloop(std::make_shared<debug_cli<P>>()) {}
		fusion_runloop(std::shared_ptr<debug_cli<P>> cli) : cli(cli), inst_cache()
		{
			trace_cache.set_empty_key(0);
			trace_cache.set_deleted_key(-1);
		}

		virtual bool handleError(Error err, const char* message, CodeEmitter* origin)
		{
			printf("%s", message);
			return false;
		}

		static void signal_handler(int signum, siginfo_t *info, void *)
		{
			static_cast<fusion_runloop<P>*>
				(fusion_fault::current)->signal_dispatch(signum, info);
		}

		void signal_dispatch(int signum, siginfo_t *info)
		{
			printf("SIGNAL   :%s pc:0x%0llx si_addr:0x%0llx\n",
				signal_name(signum), (addr_t)P::pc, (addr_t)info->si_addr);

			/* let the processor longjmp */
			P::signal(signum, info);
		}

		void init()
		{
			// block signals before so we don't deadlock in signal handlers
			sigset_t set;
			sigemptyset(&set);
			sigaddset(&set, SIGSEGV);
			sigaddset(&set, SIGTERM);
			sigaddset(&set, SIGQUIT);
			sigaddset(&set, SIGINT);
			sigaddset(&set, SIGHUP);
			sigaddset(&set, SIGUSR1);
			if (pthread_sigmask(SIG_BLOCK, &set, NULL) != 0) {
				panic("can't set thread signal mask: %s", strerror(errno));
			}

			// disable unwanted signals
			sigset_t sigpipe_set;
			sigemptyset(&sigpipe_set);
			sigaddset(&sigpipe_set, SIGPIPE);
			sigprocmask(SIG_BLOCK, &sigpipe_set, nullptr);

			// install signal handler
			struct sigaction sigaction_handler;
			memset(&sigaction_handler, 0, sizeof(sigaction_handler));
			sigaction_handler.sa_sigaction = &fusion_runloop<P>::signal_handler;
			sigaction_handler.sa_flags = SA_SIGINFO;
			sigaction(SIGSEGV, &sigaction_handler, nullptr);
			sigaction(SIGTERM, &sigaction_handler, nullptr);
			sigaction(SIGQUIT, &sigaction_handler, nullptr);
			sigaction(SIGINT, &sigaction_handler, nullptr);
			sigaction(SIGHUP, &sigaction_handler, nullptr);
			sigaction(SIGUSR1, &sigaction_handler, nullptr);
			fusion_fault::current = this;

			/* unblock signals */
			if (pthread_sigmask(SIG_UNBLOCK, &set, NULL) != 0) {
				panic("can't set thread signal mask: %s", strerror(errno));
			}

			/* processor initialization */
			P::init();
		}

		void run(exit_cause ex = exit_cause_continue)
		{
			u32 logsave = P::log;
			size_t count = inst_step;
			for (;;) {
				switch (ex) {
					case exit_cause_continue:
						break;
					case exit_cause_cli:
						P::debugging = true;
						count = cli->run(this);
						if (count == size_t(-1)) {
							P::debugging = false;
							P::log = logsave;
							count = inst_step;
						} else {
							P::log |= (proc_log_inst | proc_log_operands | proc_log_trap);
						}
						break;
					case exit_cause_poweroff:
						return;
				}
				ex = step(count);
				if (P::debugging && ex == exit_cause_continue) {
					ex = exit_cause_cli;
				}
			}
		}

		void jit_cache(CodeHolder &code, addr_t pc)
		{
			TraceFunc fn = nullptr;
			Error err = rt.add(&fn, &code);
			if (!err) trace_cache[pc] = fn;
		}

		bool jit_exec(P &proc, addr_t pc)
		{
			auto ti = trace_cache.find(pc);
			if (ti != trace_cache.end()) {
				ti->second(static_cast<typename P::processor_type *>(&proc));
				return true;
			}
			return false;
		}

		void jit_trace()
		{
			CodeHolder code;
			code.init(rt.getCodeInfo());
			code.setErrorHandler(this);
			fusion_emitter<P> emitter(*this, code);

			typename P::ux trace_pc = P::pc;
			typename P::ux trace_instret = P::instret;

			if (P::log & proc_log_jit_trace) {
				printf("jit-trace-begin pc=0x%016llx\n", P::pc);
			}

			P::log &= ~proc_log_jit_trap;

			emitter.emit_prolog();
			emitter.begin();
			for(;;) {
				typename P::decode_type dec;
				addr_t pc_offset, new_offset;
				inst_t inst = P::mmu.inst_fetch(*this, P::pc, pc_offset);
				P::inst_decode(dec, inst);
				dec.pc = P::pc;
				dec.inst = inst;
				if (emitter.emit(dec) == false) break;
				if ((new_offset = P::inst_exec(dec, pc_offset)) == -1) break;
				P::pc += new_offset;
				P::cycle++;
				P::instret++;
			}
			emitter.end();
			emitter.emit_epilog();

			P::log |= proc_log_jit_trap;

			if (P::log & proc_log_jit_trace) {
				printf("jit-trace-end   pc=0x%016llx\n", P::pc);
			}

			if (P::instret == trace_instret) {
				P::histogram_set_pc(trace_pc, P::hostspot_trace_skip);
			}
			else {
				jit_cache(code, trace_pc);
			}
		}

		void jit_audit(typename P::decode_type &dec, inst_t inst, addr_t pc_offset)
		{
			CodeHolder code;
			code.init(rt.getCodeInfo());
			code.setErrorHandler(this);
			fusion_emitter<P> emitter(*this, code);
			processor_rv64imafd pre_jit, post_jit;
			bool audited = false;

			/* jit instruction */
			emitter.emit_prolog();
			emitter.begin();
			addr_t save_pc = dec.pc = P::pc;
			dec.inst = inst;
			if (emitter.emit(dec)) {
				emitter.end();
				emitter.emit_epilog();
				TraceFunc fn;
				Error err = rt.add(&fn, &code);
				if (!err) {
					memcpy(&pre_jit, static_cast<processor_rv64imafd*>(this), sizeof(processor_rv64imafd));
					fn(static_cast<processor_rv64imafd*>(this));
					memcpy(&post_jit, static_cast<processor_rv64imafd*>(this), sizeof(processor_rv64imafd));
					memcpy(static_cast<processor_rv64imafd*>(this), &pre_jit, sizeof(processor_rv64imafd));
					audited = true;
					rt.release(fn);
				}
			}

			/* interpret instruction */
			addr_t new_offset;
			if ((new_offset = P::inst_exec(dec, pc_offset)) != -1  ||
				(new_offset = P::inst_priv(dec, pc_offset)) != -1)
			{
				if (P::log) P::print_log(dec, inst);
				P::pc += new_offset;
				P::cycle++;
				P::instret++;
			} else {
				P::raise(rv_cause_illegal_instruction, P::pc);
			}

			/* compare processor state */
			if (audited) {
				bool pass = true;
				for (size_t i = 0; i < P::ireg_count; i++) {
					if (post_jit.ireg[i].r.xu.val != P::ireg[i].r.xu.val) {
						pass = false;
						printf("ERROR interp-%s=0x%016llx jit-%s=0x%016llx\n",
							rv_ireg_name_sym[i], P::ireg[i].r.xu.val,
							rv_ireg_name_sym[i], post_jit.ireg[i].r.xu.val);
					}
				}
				if (post_jit.pc != P::pc) {
					printf("ERROR interp-pc=0x%016llx jit-pc=0x%016llx\n",
							P::pc, post_jit.pc);
					pass = false;
				}
				if (!pass) {
					printf("\t# 0x%016llx\t%s\n", save_pc, disasm_inst_simple(dec).c_str());
				}
			}
		}

		exit_cause step(size_t count)
		{
			typename P::decode_type dec;
			typename P::ux inststop = P::instret + count;
			addr_t pc_offset, new_offset;
			inst_t inst = 0, inst_cache_key;

			/* interrupt service routine */
			P::time = cpu_cycle_clock();
			P::isr();

			/* trap return path */
			int cause;
			if (unlikely((cause = setjmp(P::env)) > 0)) {
				cause -= P::internal_cause_offset;
				switch(cause) {
					case P::internal_cause_cli:
						return exit_cause_cli;
					case P::internal_cause_fatal:
						P::print_csr_registers();
						P::print_int_registers();
						return exit_cause_poweroff;
					case P::internal_cause_poweroff:
						return exit_cause_poweroff;
					case P::internal_cause_hotspot:
						jit_trace();
						return exit_cause_continue;
				}
				P::trap(dec, cause);
				if (!P::running) return exit_cause_poweroff;
			}

			/* step the processor */
			while (P::instret < inststop) {
				if ((P::log & proc_log_jit_trap) && jit_exec(*this, P::pc)) {
					continue;
				}
				if (P::pc == P::breakpoint && P::breakpoint != 0) {
					return exit_cause_cli;
				}
				inst = P::mmu.inst_fetch(*this, P::pc, pc_offset);
				inst_cache_key = inst % inst_cache_size;
				if (inst_cache[inst_cache_key].inst == inst) {
					dec = inst_cache[inst_cache_key].dec;
				} else {
					P::inst_decode(dec, inst);
					inst_cache[inst_cache_key].inst = inst;
					inst_cache[inst_cache_key].dec = dec;
				}
				if (P::log & proc_log_jit_audit) {
					jit_audit(dec, inst, pc_offset);
				}
				else if ((new_offset = P::inst_exec(dec, pc_offset)) != -1  ||
					(new_offset = P::inst_priv(dec, pc_offset)) != -1)
				{
					if (P::log & ~(proc_log_hist_pc | proc_log_jit_trap)) P::print_log(dec, inst);
					P::pc += new_offset;
					P::cycle++;
					P::instret++;
				} else {
					P::raise(rv_cause_illegal_instruction, P::pc);
				}
			}
			return exit_cause_continue;
		}
	};

}

#endif

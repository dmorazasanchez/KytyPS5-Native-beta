#include "loader/demonsScreamRecovery.h"
#include <array>
#include <cstdlib>
#include <sys/mman.h>

using namespace Loader::DemonsScreamRecovery;
using namespace Common::HostException;

static void Check(bool value, const char* why) {
	if (!value) { std::fprintf(stderr, "Scream recovery test failed: %s\n", why); std::abort(); }
}

int main() {
	constexpr std::array<uint64_t, 2> sites {0x901fa24d1ull, 0x901e22491ull};
	for (auto pc: sites) {
		void* p = mmap(reinterpret_cast<void*>(pc & ~0xfffull), 4096, PROT_READ | PROT_WRITE,
		               MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
		Check(p != MAP_FAILED, "reserve synthetic guest instruction page without replacing mappings");
		std::memcpy(reinterpret_cast<void*>(pc), "\xcd\x41\xeb\x4d", 4);
	}
	ucontext_t context {};
	ExceptionInfo fault {};
	fault.type = ExceptionType::AccessViolation;
	fault.access_violation_type = AccessViolationType::Write;
	fault.native_code = SIGSEGV;
	fault.native_context = &context;
	auto attempt = [&](uint64_t pc) {
		fault.exception_address = pc;
		context.uc_mcontext.gregs[REG_RIP] = static_cast<greg_t>(pc);
		return TryRecoverDemonsScreamAssert(&fault);
	};
	setenv("KYTY_BC250_TITLE_ID", "PPSA01341", 1);
	setenv("KYTY_DEMONS_SCREAM_ASSERT_RECOVERY", "0", 1);
	ConfigureFromEnvironment();
	Check(!attempt(sites[1]), "the real captured 1.007 fault is not swallowed when disabled");
	setenv("KYTY_DEMONS_SCREAM_ASSERT_RECOVERY", "1", 1);
	setenv("KYTY_BC250_TITLE_ID", "PPSA26344", 1);
	ConfigureFromEnvironment();
	Check(!attempt(sites[1]), "another title cannot enable the guard");
	setenv("KYTY_BC250_TITLE_ID", "PPSA01341", 1);
	ConfigureFromEnvironment();
	for (auto pc: sites) {
		Check(attempt(pc), "replay the confirmed 1.004/1.007 Scream assertion");
		Check(context.uc_mcontext.gregs[REG_RIP] == static_cast<greg_t>(pc + 2),
		      "resume at the existing guest cleanup jump");
	}
	Check(!attempt(sites[1] + 1), "adjacent and unknown instruction addresses remain faults");
	*reinterpret_cast<uint8_t*>(sites[1]) = 0x90;
	Check(!attempt(sites[1]), "mismatched instruction bytes remain faults");
	*reinterpret_cast<uint8_t*>(sites[1]) = 0xcd;
	fault.native_code = SIGILL;
	Check(!attempt(sites[1]), "unrelated signals remain faults");
	fault.native_code = SIGSEGV;
	fault.access_violation_type = AccessViolationType::Read;
	Check(!attempt(sites[1]), "read faults remain faults");
	fault.access_violation_type = AccessViolationType::Write;
	fault.access_violation_vaddr = 1;
	Check(!attempt(sites[1]), "non-null write faults remain faults");
	fault.access_violation_vaddr = 0;
	fault.native_context = nullptr;
	Check(!attempt(sites[1]), "missing context remains a fault");
	fault.native_context = &context;
	setenv("KYTY_DEMONS_SCREAM_ASSERT_RECOVERY", "true", 1);
	ConfigureFromEnvironment();
	Check(!attempt(sites[1]), "only explicit option value 1 enables recovery");
	for (auto pc: sites) munmap(reinterpret_cast<void*>(pc & ~0xfffull), 4096);
	std::puts("Demon's Souls Scream recovery CPU replay and negative cases passed.");
}

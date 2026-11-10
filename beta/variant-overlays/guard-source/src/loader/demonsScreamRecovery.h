#ifndef KYTY_LOADER_DEMONS_SCREAM_RECOVERY_H_
#define KYTY_LOADER_DEMONS_SCREAM_RECOVERY_H_

#include "common/hostException.h"
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX && defined(__x86_64__)
#include <ucontext.h>
#include <unistd.h>
#endif

namespace Loader::DemonsScreamRecovery {

// Narrow, opt-in recovery for known PPSA01341 v01.004/v01.007 Scream assertions.
// This is an audio compatibility experiment, not a cure for invalid audio state.
// Keep recovery lifetime per *process*, not per ELF/PRX module loaded.
#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX && defined(__x86_64__)
inline std::atomic_bool     g_demons_scream_assert_recovery {false};
inline std::atomic_bool     g_demons_scream_recovery_announced {false};
inline std::atomic_uint64_t g_demons_scream_assert_count {0};
inline std::atomic_int64_t  g_demons_scream_window_second {-1};
inline std::atomic_uint32_t g_demons_scream_window_count {0};

// Async-signal-safe output: write() only; decimal conversion uses stack data.
inline void LogScreamRecoveryCount(uint64_t count) noexcept {
    constexpr char prefix[] =
        "[Kyty] Demon's Souls Scream INT 0x41 recovery occurrences=";
    constexpr char suffix[] =
        " (same game assertion, continuing its cleanup branch)\n";
    char digits[24] {};
    size_t start = sizeof(digits);
    do {
        digits[--start] = static_cast<char>('0' + count % 10);
        count /= 10;
    } while (count != 0);
    (void)::write(STDERR_FILENO, prefix, sizeof(prefix) - 1);
    (void)::write(STDERR_FILENO, digits + start, sizeof(digits) - start);
    (void)::write(STDERR_FILENO, suffix, sizeof(suffix) - 1);
}

inline bool TryRecoverDemonsScreamAssert(const Common::HostException::ExceptionInfo* info) noexcept {
    // Two distinct, confirmed PPSA01341 guest binaries. Do not guess addresses:
    // 1.004 was previously observed here; 1.007 was observed on 2026-11-03
    // in BPE JobWorkerThread when the ATRAC9 decoder was created.
    constexpr uint64_t kPc1004 = 0x0000000901fa24d1ull;
    constexpr uint64_t kPc1007 = 0x0000000901e22491ull;
    const uint64_t kPc = info->exception_address;
    if (!g_demons_scream_assert_recovery.load(std::memory_order_acquire) ||
        info->type != Common::HostException::ExceptionType::AccessViolation ||
        (kPc != kPc1004 && kPc != kPc1007) ||
        info->access_violation_vaddr != 0 ||
        info->access_violation_type != Common::HostException::AccessViolationType::Write ||
        info->native_code != SIGSEGV || info->native_context == nullptr) {
        return false;
    }
    // Exact bytes in both PPSA01341 v01.004 and v01.007: INT 0x41; JMP +0x4D (guest cleanup).
    const auto* insn = reinterpret_cast<const uint8_t*>(kPc);
    if (insn[0] != 0xcd || insn[1] != 0x41 || insn[2] != 0xeb || insn[3] != 0x4d) {
        return false;
    }

    // No arbitrary lifetime cap. Recover infrequent assertions even after
    // hours of play. Detect only pathological tight loops (>1024 per second)
    // rather than hiding a broken state and wedging the machine.
    timespec now {};
    if (::clock_gettime(CLOCK_MONOTONIC, &now) == 0) {
        const auto second = static_cast<int64_t>(now.tv_sec);
        auto observed = g_demons_scream_window_second.load(std::memory_order_relaxed);
        if (observed != second &&
            g_demons_scream_window_second.compare_exchange_strong(observed, second,
                    std::memory_order_relaxed)) {
            g_demons_scream_window_count.store(0, std::memory_order_relaxed);
        }
        const auto hits = g_demons_scream_window_count.fetch_add(1, std::memory_order_relaxed) + 1;
        if (hits > 1024) {
            constexpr char warning[] =
                "[Kyty] Scream recovery stopped: >1024 assertions in one second, "
                "probable infinite loop. Emitting original guest fault.\n";
            (void)::write(STDERR_FILENO, warning, sizeof(warning) - 1);
            return false;
        }
    }

    const auto occurrence =
        g_demons_scream_assert_count.fetch_add(1, std::memory_order_relaxed) + 1;
    auto* context = static_cast<ucontext_t*>(info->native_context);
    context->uc_mcontext.gregs[REG_RIP] = static_cast<greg_t>(kPc + 2);
    // Powers of two: 1,2,4,8,16,32,... This bounds log spam even at
    // thousands of recoveries while proving counts above the old 16 cap.
    if ((occurrence & (occurrence - 1)) == 0)
        LogScreamRecoveryCount(occurrence);
    return true;
}
#endif

#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX && defined(__x86_64__)
inline void ConfigureFromEnvironment() {
    // Parse the title and user opt-in once BEFORE installing any signal handler.
    // Do not call getenv or allocate memory in the signal handler.
    const char* title = std::getenv("KYTY_BC250_TITLE_ID");
    const char* allow = std::getenv("KYTY_DEMONS_SCREAM_ASSERT_RECOVERY");
    const bool recovery = title != nullptr && std::strcmp(title, "PPSA01341") == 0 &&
                          allow != nullptr && allow[0] == '1' && allow[1] == '\0';
    g_demons_scream_assert_recovery.store(recovery, std::memory_order_release);
    if (recovery && !g_demons_scream_recovery_announced.exchange(true, std::memory_order_relaxed)) {
        // Prime libc's clock_gettime symbol outside the signal handler.
        timespec now {};
        (void)::clock_gettime(CLOCK_MONOTONIC, &now);
        std::fprintf(stderr, "[Kyty] Experimental Demon's Souls Scream assertion recovery armed "
                             "for exact PPSA01341 1.004/1.007 INT 0x41 sites; "
                             "1024-per-second runaway guard.\n");
    }
}
#endif

} // namespace Loader::DemonsScreamRecovery
#endif // KYTY_LOADER_DEMONS_SCREAM_RECOVERY_H_

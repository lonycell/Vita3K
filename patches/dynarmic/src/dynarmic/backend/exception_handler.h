/* This file is part of the dynarmic project.
 * Copyright (c) 2020 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include <functional>
#include <memory>
#include <optional>

#include <mcl/macro/architecture.hpp>
#include <mcl/stdint.hpp>

#if defined(MCL_ARCHITECTURE_X86_64)
namespace Dynarmic::Backend::X64 {
class BlockOfCode;
}  // namespace Dynarmic::Backend::X64
#elif defined(MCL_ARCHITECTURE_ARM64)
namespace oaknut {
class CodeBlock;
}  // namespace oaknut
#elif defined(MCL_ARCHITECTURE_RISCV)
namespace Dynarmic::Backend::RV64 {
class CodeBlock;
}  // namespace Dynarmic::Backend::RV64
#else
#    error "Invalid architecture"
#endif

namespace Dynarmic::Backend {

#if defined(MCL_ARCHITECTURE_X86_64)
struct FakeCall {
    u64 call_rip;
    u64 ret_rip;
};
#elif defined(MCL_ARCHITECTURE_ARM64)
struct FakeCall {
    u64 call_pc;
};
#elif defined(MCL_ARCHITECTURE_RISCV)
struct FakeCall {
};
#else
#    error "Invalid architecture"
#endif

class ExceptionHandler final {
public:
    ExceptionHandler();
    ~ExceptionHandler();

#if defined(MCL_ARCHITECTURE_X86_64)
    void Register(X64::BlockOfCode& code);
#elif defined(MCL_ARCHITECTURE_ARM64)
    void Register(oaknut::CodeBlock& mem, std::size_t mem_size);
#elif defined(MCL_ARCHITECTURE_RISCV)
    void Register(RV64::CodeBlock& mem, std::size_t mem_size);
#else
#    error "Invalid architecture"
#endif

    bool SupportsFastmem() const noexcept;
    void SetFastmemCallback(std::function<FakeCall(u64)> cb);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

// Register a process-wide fallback that is invoked when an EXC_BAD_ACCESS occurs at an address
// that is not inside any registered JIT code block (currently macOS only). The callback receives
// the faulting address and whether the access was a write, and should return true if it resolved
// the fault (the faulting instruction is then retried). This lets the host's own memory-protection
// fault handling (write-watch / page recovery) keep working even though dynarmic installs a
// task-level Mach exception port that otherwise intercepts every EXC_BAD_ACCESS. A no-op on
// platforms where the host signal handler still receives these faults directly.
void SetNonJitFaultFallback(std::function<bool(void* fault_address, bool is_write)> callback);

}  // namespace Dynarmic::Backend

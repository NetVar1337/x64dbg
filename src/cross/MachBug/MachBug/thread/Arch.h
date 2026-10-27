#pragma once

#include <mach/mach.h>
#include <cstddef>
#include <cstdint>

// The debugger is built for the architecture it debugs: an x86_64 build debugs Intel
// processes (natively or under Rosetta), an arm64 build debugs arm64 processes.
namespace MachBug
{
    using ptr = uint64_t;

#if defined(__x86_64__)
    using ThreadState = x86_thread_state64_t;
    constexpr thread_state_flavor_t kThreadStateFlavor = x86_THREAD_STATE64;
    constexpr mach_msg_type_number_t kThreadStateCount = x86_THREAD_STATE64_COUNT;
    constexpr uint8_t kBreakpointOpcode[] = {0xCC}; // int3
#elif defined(__arm64__)
    using ThreadState = arm_thread_state64_t;
    constexpr thread_state_flavor_t kThreadStateFlavor = ARM_THREAD_STATE64;
    constexpr mach_msg_type_number_t kThreadStateCount = ARM_THREAD_STATE64_COUNT;
    constexpr uint8_t kBreakpointOpcode[] = {0x00, 0x00, 0x20, 0xD4}; // brk #0
#else
#error MachBug supports x86_64 and arm64
#endif

    constexpr size_t kBreakpointSize = sizeof(kBreakpointOpcode);

    bool ReadThreadState(thread_act_t thread, ThreadState & state);
    bool WriteThreadState(thread_act_t thread, const ThreadState & state);

    ptr GetPc(const ThreadState & state);
    void SetPc(ThreadState & state, ptr pc);
    ptr GetSp(const ThreadState & state);

    bool SetSingleStep(thread_act_t thread, bool enable);

    // Address of the trap instruction for an EXC_BREAKPOINT at pc: x86 has already
    // executed the int3, arm64 leaves pc on the brk.
    ptr TrapAddress(ptr pc);

    bool IsSingleStepTrap(const int64_t* code, mach_msg_type_number_t codeCount);

    // True if the bytes are a trap instruction that raises EXC_BREAKPOINT.
    bool IsTrapInstruction(const uint8_t* bytes, size_t size);

    // Accepts the architecture's register names plus cip/csp.
    bool GetRegister(const ThreadState & state, const char* name, uint64_t & value);
    bool SetRegister(ThreadState & state, const char* name, uint64_t value);
}

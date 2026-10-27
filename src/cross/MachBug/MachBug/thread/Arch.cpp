#include <MachBug/thread/Arch.h>
#include <cstdlib>
#include <cstring>

namespace MachBug
{
    bool ReadThreadState(const thread_act_t thread, ThreadState & state)
    {
        mach_msg_type_number_t count = kThreadStateCount;
        return thread_get_state(thread, kThreadStateFlavor, reinterpret_cast<thread_state_t>(&state), &count) == KERN_SUCCESS;
    }

    bool WriteThreadState(const thread_act_t thread, const ThreadState & state)
    {
        return thread_set_state(thread, kThreadStateFlavor,
                                reinterpret_cast<thread_state_t>(const_cast<ThreadState*>(&state)), kThreadStateCount) == KERN_SUCCESS;
    }

#if defined(__x86_64__)
    namespace
    {
        struct NamedRegister
        {
            const char* name;
            uint64_t ThreadState::* field;
        };

        constexpr NamedRegister kRegisters[] =
        {
            {"rax", &ThreadState::__rax}, {"rbx", &ThreadState::__rbx}, {"rcx", &ThreadState::__rcx},
            {"rdx", &ThreadState::__rdx}, {"rdi", &ThreadState::__rdi}, {"rsi", &ThreadState::__rsi},
            {"rbp", &ThreadState::__rbp}, {"rsp", &ThreadState::__rsp}, {"csp", &ThreadState::__rsp},
            {"r8", &ThreadState::__r8}, {"r9", &ThreadState::__r9}, {"r10", &ThreadState::__r10},
            {"r11", &ThreadState::__r11}, {"r12", &ThreadState::__r12}, {"r13", &ThreadState::__r13},
            {"r14", &ThreadState::__r14}, {"r15", &ThreadState::__r15},
            {"rip", &ThreadState::__rip}, {"cip", &ThreadState::__rip},
            {"rflags", &ThreadState::__rflags}, {"eflags", &ThreadState::__rflags},
        };

        const NamedRegister* findRegister(const char* name)
        {
            for(const auto & entry : kRegisters)
            {
                if(strcmp(entry.name, name) == 0)
                    return &entry;
            }
            return nullptr;
        }

        constexpr uint64_t kTrapFlag = 1 << 8;
    }

    ptr GetPc(const ThreadState & state)
    {
        return state.__rip;
    }

    void SetPc(ThreadState & state, const ptr pc)
    {
        state.__rip = pc;
    }

    ptr GetSp(const ThreadState & state)
    {
        return state.__rsp;
    }

    bool SetSingleStep(const thread_act_t thread, const bool enable)
    {
        ThreadState state = {};
        if(!ReadThreadState(thread, state))
            return false;
        state.__rflags = enable ? state.__rflags | kTrapFlag : state.__rflags & ~kTrapFlag;
        return WriteThreadState(thread, state);
    }

    ptr TrapAddress(const ptr pc)
    {
        return pc - kBreakpointSize;
    }

    bool IsSingleStepTrap(const int64_t* code, const mach_msg_type_number_t codeCount)
    {
        return codeCount > 0 && code[0] == EXC_I386_SGL;
    }

    bool IsTrapInstruction(const uint8_t* bytes, const size_t size)
    {
        return size >= 1 && bytes[0] == 0xCC;
    }

    bool GetRegister(const ThreadState & state, const char* name, uint64_t & value)
    {
        const auto* entry = findRegister(name);
        if(!entry)
            return false;
        value = state.*entry->field;
        return true;
    }

    bool SetRegister(ThreadState & state, const char* name, const uint64_t value)
    {
        const auto* entry = findRegister(name);
        if(!entry)
            return false;
        state.*entry->field = value;
        return true;
    }
#elif defined(__arm64__)
    namespace
    {
        constexpr uint64_t kSoftwareStep = 1; // MDSCR_EL1.SS
        constexpr uint32_t kBrkMask = 0xFFE0001F;
        constexpr uint32_t kBrk = 0xD4200000;

        // x0-x28 index into __x; everything else gets its own accessor.
        int generalIndex(const char* name)
        {
            if(name[0] != 'x' || name[1] < '0' || name[1] > '9')
                return -1;
            char* end = nullptr;
            const long index = strtol(name + 1, &end, 10);
            return *end == '\0' && index >= 0 && index <= 30 ? static_cast<int>(index) : -1;
        }
    }

    ptr GetPc(const ThreadState & state)
    {
        return arm_thread_state64_get_pc(state);
    }

    void SetPc(ThreadState & state, const ptr pc)
    {
        arm_thread_state64_set_pc_fptr(state, reinterpret_cast<void*>(pc));
    }

    ptr GetSp(const ThreadState & state)
    {
        return arm_thread_state64_get_sp(state);
    }

    bool SetSingleStep(const thread_act_t thread, const bool enable)
    {
        arm_debug_state64_t debug = {};
        mach_msg_type_number_t count = ARM_DEBUG_STATE64_COUNT;
        if(thread_get_state(thread, ARM_DEBUG_STATE64, reinterpret_cast<thread_state_t>(&debug), &count) != KERN_SUCCESS)
            return false;
        debug.__mdscr_el1 = enable ? debug.__mdscr_el1 | kSoftwareStep : debug.__mdscr_el1 & ~kSoftwareStep;
        return thread_set_state(thread, ARM_DEBUG_STATE64, reinterpret_cast<thread_state_t>(&debug), count) == KERN_SUCCESS;
    }

    ptr TrapAddress(const ptr pc)
    {
        return pc;
    }

    // A brk reports its own address in code[1]; a completed step reports 0.
    bool IsSingleStepTrap(const int64_t* code, const mach_msg_type_number_t codeCount)
    {
        return codeCount > 1 && code[0] == EXC_ARM_BREAKPOINT && code[1] == 0;
    }

    bool IsTrapInstruction(const uint8_t* bytes, const size_t size)
    {
        if(size < 4)
            return false;
        uint32_t instruction = 0;
        memcpy(&instruction, bytes, sizeof(instruction));
        return (instruction & kBrkMask) == kBrk;
    }

    bool GetRegister(const ThreadState & state, const char* name, uint64_t & value)
    {
        const int index = generalIndex(name);
        if(index >= 0 && index <= 28)
            value = state.__x[index];
        else if(index == 29 || strcmp(name, "fp") == 0)
            value = arm_thread_state64_get_fp(state);
        else if(index == 30 || strcmp(name, "lr") == 0)
            value = arm_thread_state64_get_lr(state);
        else if(strcmp(name, "sp") == 0 || strcmp(name, "csp") == 0)
            value = arm_thread_state64_get_sp(state);
        else if(strcmp(name, "pc") == 0 || strcmp(name, "cip") == 0)
            value = arm_thread_state64_get_pc(state);
        else if(strcmp(name, "cpsr") == 0)
            value = state.__cpsr;
        else
            return false;
        return true;
    }

    bool SetRegister(ThreadState & state, const char* name, const uint64_t value)
    {
        const int index = generalIndex(name);
        if(index >= 0 && index <= 28)
            state.__x[index] = value;
        else if(index == 29 || strcmp(name, "fp") == 0)
            arm_thread_state64_set_fp(state, value);
        else if(index == 30 || strcmp(name, "lr") == 0)
            arm_thread_state64_set_lr_fptr(state, reinterpret_cast<void*>(value));
        else if(strcmp(name, "sp") == 0 || strcmp(name, "csp") == 0)
            arm_thread_state64_set_sp(state, value);
        else if(strcmp(name, "pc") == 0 || strcmp(name, "cip") == 0)
            SetPc(state, value);
        else if(strcmp(name, "cpsr") == 0)
            state.__cpsr = static_cast<uint32_t>(value);
        else
            return false;
        return true;
    }
#endif
}

#pragma once

#include <MachBug/thread/Arch.h>

namespace MachBug
{
    enum class StepKind
    {
        Plain,
        Call,      // stepped over by running to the next instruction
        Repeat,    // x86 rep string instruction, stepped over the same way
        PushFlags, // x86 pushf: a single step pushes the trap flag along with the rest
    };

    // Longest instruction the classifier needs to see.
    constexpr size_t kMaxInstructionSize = 16;

    // next is the address after the instruction, or 0 for an instruction that is not understood.
    StepKind ClassifyStep(const uint8_t* bytes, size_t size, ptr pc, ptr & next);
}

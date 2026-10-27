#include <MachBug/thread/Step.h>
#include <cstring>
#if defined(__x86_64__)
#include <zydis_wrapper.h>
#endif

namespace MachBug
{
#if defined(__x86_64__)
    StepKind ClassifyStep(const uint8_t* bytes, const size_t size, const ptr pc, ptr & next)
    {
        next = 0;
        Zydis zydis(true);
        if(!zydis.Disassemble(pc, bytes, size))
            return StepKind::Plain;
        next = pc + zydis.Size();
        if(zydis.IsCall())
            return StepKind::Call;
        if(zydis.IsRepeated())
            return StepKind::Repeat;
        switch(zydis.GetId())
        {
        case ZYDIS_MNEMONIC_PUSHF:
        case ZYDIS_MNEMONIC_PUSHFD:
        case ZYDIS_MNEMONIC_PUSHFQ:
            return StepKind::PushFlags;
        default:
            return StepKind::Plain;
        }
    }
#elif defined(__arm64__)
    StepKind ClassifyStep(const uint8_t* bytes, const size_t size, const ptr pc, ptr & next)
    {
        next = 0;
        if(size < 4)
            return StepKind::Plain;
        uint32_t instruction = 0;
        memcpy(&instruction, bytes, sizeof(instruction));
        next = pc + 4;
        const bool call = (instruction & 0xFC000000) == 0x94000000 || // bl
                          (instruction & 0xFFFFFC1F) == 0xD63F0000 || // blr
                          (instruction & 0xFFFFF81F) == 0xD63F081F || // blraaz, blrabz
                          (instruction & 0xFFFFF800) == 0xD73F0800;   // blraa, blrab
        return call ? StepKind::Call : StepKind::Plain;
    }
#endif
}

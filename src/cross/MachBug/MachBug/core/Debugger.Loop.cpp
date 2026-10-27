#include <MachBug/core/Debugger.h>
#include <MachBug/process/Image.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <string>

namespace MachBug
{
    namespace
    {
        constexpr mach_msg_timeout_t kPollIntervalMs = 10;

        // The signal the kernel turns an unhandled exception into.
        int signalForException(const exception_type_t exception, const int64_t* code)
        {
            switch(exception)
            {
            case EXC_BAD_ACCESS:
                return code[0] == KERN_INVALID_ADDRESS ? SIGSEGV : SIGBUS;
            case EXC_BAD_INSTRUCTION:
                return SIGILL;
            case EXC_ARITHMETIC:
                return SIGFPE;
            default:
                return SIGTRAP;
            }
        }

        bool isSoftSignal(const ExceptionMessage & message)
        {
            return message.exception == EXC_SOFTWARE && message.codeCount > 1 && message.code[0] == EXC_SOFT_SIGNAL;
        }
    }

    void Debugger::debugLoop()
    {
        for(;;)
        {
            bool reported = false;
            while(!reported)
            {
                std::optional<ExceptionMessage> message;
                if(!nextEvent(message))
                    return;
                reported = message ? handleException(*message) : reportPause();
            }

            const Command command = waitForCommand();
            if(command == Command::Detach)
            {
                detachProcess();
                return;
            }
            if(command == Command::Stop)
                killProcess();
            else
                resume(command);
        }
    }

    Debugger::Command Debugger::waitForCommand()
    {
        std::unique_lock lock(mCommandMutex);
        mCommandCv.wait(lock, [this]
        {
            return mCommand != Command::None || mStopRequested.load(std::memory_order_acquire) ||
                    mDetachRequested.load(std::memory_order_acquire);
        });
        if(mStopRequested.load(std::memory_order_acquire))
            mCommand = Command::Stop;
        else if(mDetachRequested.load(std::memory_order_acquire))
            mCommand = Command::Detach;
        // Cleared under the lock so a late Continue cannot queue a command for the next stop.
        mPaused.store(false, std::memory_order_release);
        return mCommand;
    }

    void Debugger::stopAt(const uint64_t tid)
    {
        // Any reported stop ends a step over, as in x64dbg.
        cancelStepOver();
        {
            std::lock_guard lock(mStateMutex);
            if(tid != 0)
                mCurrentTid = tid;
        }
        std::lock_guard lock(mCommandMutex);
        mCommand = Command::None;
        mPauseRequested.store(false, std::memory_order_release);
        mPaused.store(true, std::memory_order_release);
    }

    bool Debugger::nextEvent(std::optional<ExceptionMessage> & message)
    {
        // While a thread steps off, only its own step can come next.
        if(!mQueued.empty() && !mKilled && !mStepOff)
        {
            suspendTask();
            message = mQueued.front();
            mQueued.pop_front();
            return true;
        }

        for(;;)
        {
            if(reapExit())
                return false;
            if(mStopRequested.load(std::memory_order_acquire))
                killProcess();
            if(!mKilled && mDetachRequested.load(std::memory_order_acquire))
            {
                detachProcess();
                return false;
            }

            ExceptionMessage received;
            if(ReceiveException(mExceptionPort, kPollIntervalMs, received))
            {
                if(mKilled)
                {
                    ReplyException(received, KERN_SUCCESS);
                    continue;
                }
                suspendTask();
                message = received;
                return true;
            }

            // A pause waits for a step-off to finish: the other threads are held during it.
            if(!mKilled && !mStepOff && mPauseRequested.exchange(false, std::memory_order_acq_rel))
            {
                suspendTask();
                return true;
            }

            // A thread that exits while stepping off never reports the step; release the others.
            if(mStepOff && !threadAlive(mStepOff->tid))
            {
                // Code is only patched while nothing runs.
                suspendTask();
                finishStepOff();
                resumeTask();
            }
        }
    }

    bool Debugger::reapExit()
    {
        int status = 0;
        const pid_t pid = Pid();
        const pid_t result = waitpid(pid, &status, WNOHANG);
        if(result == 0 || (result == -1 && errno == EINTR) || (result == pid && WIFSTOPPED(status)))
            return false;

        mExited = true;
        int exitCode = 0;
        if(result == pid)
            exitCode = WIFSIGNALED(status) ? -WTERMSIG(status) : WEXITSTATUS(status);
        else
            cbInternalError("waitpid failed: " + std::string(strerror(errno)));
        mPaused.store(false, std::memory_order_release);
        cbExitProcess(exitCode);
        return true;
    }

    bool Debugger::reportPause()
    {
        refreshThreads(true);
        stopAt(0);
        cbPaused();
        return true;
    }

    bool Debugger::handleException(ExceptionMessage & message)
    {
        // The threads present at the system breakpoint are part of the new process.
        refreshThreads(!mAwaitingAttachStop);
        uint64_t tid = 0;
        {
            std::lock_guard lock(mStateMutex);
            if(const Thread* thread = findThreadByPort(message.thread))
                tid = thread->tid;
        }

        if(mStepOff && mStepOff->tid != tid)
        {
            // Raised before the other threads were held; report it once the step-off is done.
            mQueued.push_back(message);
            resumeTask();
            return false;
        }

        if(mPushFlagsTid == tid)
        {
            if(message.exception == EXC_BREAKPOINT && IsSingleStepTrap(message.code, message.codeCount))
                maskPushedTrapFlag(message.thread);
            mPushFlagsTid = 0;
        }

        if(mStepOff)
        {
            finishStepOff();
            if(message.exception == EXC_BREAKPOINT && IsSingleStepTrap(message.code, message.codeCount))
            {
                // Stepping off the breakpoint was the step.
                if(mStepTid == tid)
                {
                    mStepTid = 0;
                    mPending = PendingReply{message};
                    stopAt(tid);
                    cbStep();
                    return true;
                }
                resumeSilently(message);
                return false;
            }
            // The original instruction raised something itself: report that.
        }

        if(isSoftSignal(message))
        {
            const int signal = static_cast<int>(message.code[1]);
            if(mAwaitingAttachStop && signal == SIGSTOP)
            {
                mAwaitingAttachStop = false;
                mEntryPoint = FindEntryPoint(mTask);
                cbCreateProcess(Pid(), mEntryPoint);
                // Discarded on resume: the SIGSTOP is the debugger's, not the program's.
                mPending = PendingReply{message, KERN_SUCCESS, 0};
                stopAt(tid);
                if(mLaunched)
                    cbSystemBreakpoint();
                else
                    cbAttachBreakpoint();
                return true;
            }
            const auto forwarded = mForwardedSignals.find(tid);
            if(forwarded != mForwardedSignals.end() && forwarded->second == signal)
            {
                mForwardedSignals.erase(forwarded);
                resumeSilently(message, signal);
                return false;
            }
            mPending = PendingReply{message, KERN_SUCCESS, signal};
            stopAt(tid);
            cbException(signal, 0);
            return true;
        }

        if(message.exception == EXC_BREAKPOINT)
            return handleBreakpoint(tid, message);

        // Passing the fault on makes the kernel raise the matching signal.
        const int signal = signalForException(message.exception, message.code);
        const ptr address = message.exception == EXC_BAD_ACCESS ? static_cast<ptr>(message.code[1]) : 0;
        reportException(tid, message, signal, address, KERN_FAILURE);
        return true;
    }

    bool Debugger::handleBreakpoint(const uint64_t tid, ExceptionMessage & message)
    {
        const bool singleStep = IsSingleStepTrap(message.code, message.codeCount);
        if(singleStep && mStepTid == tid)
        {
            mStepTid = 0;
            SetSingleStep(message.thread, false);
            mPending = PendingReply{message};
            stopAt(tid);
            cbStep();
            return true;
        }

        ThreadState state = {};
        if(!singleStep && ReadThreadState(message.thread, state))
        {
            const ptr address = TrapAddress(GetPc(state));
            bool known = false;
            bool user = false;
            {
                std::lock_guard lock(mStateMutex);
                const auto it = mBreakpoints.find(address);
                known = it != mBreakpoints.end();
                user = known && it->second.user;
            }
            if(known)
            {
                SetPc(state, address);
                WriteThreadState(message.thread, state);
                mPending = PendingReply{message};
                if(mStepOver && mStepOver->address == address && mStepOver->tid == tid && GetSp(state) >= mStepOver->stackFloor)
                {
                    stopAt(tid);
                    cbStep();
                    return true;
                }
                if(user)
                {
                    stopAt(tid);
                    cbBreakpoint(address);
                    return true;
                }
                // Another thread or a deeper frame reached the step-over target: let it through.
                runThread(tid, false);
                return false;
            }

            // The breakpoint was deleted while this request waited: run the original code.
            uint8_t bytes[kBreakpointSize] = {};
            if(ReadMemory(mTask, address, bytes, sizeof(bytes)) && !IsTrapInstruction(bytes, sizeof(bytes)))
            {
                SetPc(state, address);
                WriteThreadState(message.thread, state);
                resumeSilently(message);
                return false;
            }
        }

        // The program's own trap.
        reportException(tid, message, SIGTRAP, 0, KERN_FAILURE);
        return true;
    }

    void Debugger::reportException(const uint64_t tid, ExceptionMessage & message, const int signal, const ptr address,
                                   const kern_return_t result)
    {
        if(result == KERN_FAILURE)
            mForwardedSignals[tid] = signal;
        mPending = PendingReply{message, result};
        stopAt(tid);
        cbException(signal, address);
    }

    void Debugger::resumeSilently(ExceptionMessage & message, const int signal)
    {
        if(isSoftSignal(message))
            ptrace(PT_THUPDATE, Pid(), reinterpret_cast<caddr_t>(static_cast<uintptr_t>(message.thread)), signal);
        ReplyException(message, KERN_SUCCESS);
        resumeTask();
    }

    void Debugger::resume(const Command command)
    {
        uint64_t tid = 0;
        ThreadState state = {};
        {
            std::lock_guard lock(mStateMutex);
            const Thread* thread = findThread(mCurrentTid);
            if(thread && ReadThreadState(thread->port, state))
                tid = thread->tid;
        }

        bool step = command == Command::StepInto;
        ptr next = 0;
        if(tid != 0 && command == Command::StepOver)
        {
            const StepKind kind = classifyAt(GetPc(state), next);
            step = true;
            if(kind == StepKind::Call || kind == StepKind::Repeat)
            {
                std::lock_guard lock(mStateMutex);
                bool planted = mBreakpoints.count(next) != 0;
                if(!planted)
                {
                    Breakpoint bp;
                    bp.user = false;
                    planted = ReadMemory(mTask, next, bp.original.data(), kBreakpointSize) &&
                              WriteMemory(mTask, next, kBreakpointOpcode, kBreakpointSize);
                    if(planted)
                        mBreakpoints.emplace(next, bp);
                }
                if(planted)
                {
                    mStepOver = StepOverRequest{tid, next, GetSp(state)};
                    step = false;
                }
            }
        }
        mStepTid = step ? tid : 0;
        runThread(tid, step);
    }

    // Lets the task run. tid first steps off a breakpoint under its pc, and is single-stepped if step.
    void Debugger::runThread(const uint64_t tid, const bool step)
    {
        thread_act_t port = MACH_PORT_NULL;
        ptr pc = 0;
        std::optional<ptr> breakpoint;
        {
            std::lock_guard lock(mStateMutex);
            if(const Thread* thread = findThread(tid))
            {
                port = thread->port;
                ThreadState state = {};
                if(ReadThreadState(port, state))
                    pc = GetPc(state);
                if(pc != 0 && mBreakpoints.count(pc))
                    breakpoint = pc;
            }

            if(breakpoint)
            {
                // Put the original code back and run only this thread over it. The thread list is
                // read again: a thread created by a syscall still in flight at the stop is not known yet.
                StepOff stepOff{tid, *breakpoint, {}};
                WriteMemory(mTask, *breakpoint, mBreakpoints[*breakpoint].original.data(), kBreakpointSize);
                thread_act_array_t list = nullptr;
                mach_msg_type_number_t count = 0;
                if(task_threads(mTask, &list, &count) == KERN_SUCCESS)
                {
                    for(mach_msg_type_number_t i = 0; i < count; ++i)
                    {
                        if(list[i] != port && thread_suspend(list[i]) == KERN_SUCCESS)
                            stepOff.held.push_back(list[i]); // keeps the reference task_threads added
                        else
                            mach_port_deallocate(mach_task_self(), list[i]);
                    }
                    vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(list), count * sizeof(thread_act_t));
                }
                mStepOff = std::move(stepOff);
            }
        }

        if(port != MACH_PORT_NULL && (breakpoint || step))
        {
            SetSingleStep(port, true);
            ptr next = 0;
            if(classifyAt(pc, next) == StepKind::PushFlags)
                mPushFlagsTid = tid;
        }
        replyPending();
        resumeTask();
    }

    StepKind Debugger::classifyAt(const ptr pc, ptr & next) const
    {
        uint8_t bytes[kMaxInstructionSize] = {};
        size_t size = sizeof(bytes);
        // An instruction at the end of a mapping is followed by fewer readable bytes.
        if(!MemRead(pc, bytes, size))
        {
            size = std::min<size_t>(size, vm_page_size - (pc & (vm_page_size - 1)));
            if(!MemRead(pc, bytes, size))
            {
                next = 0;
                return StepKind::Plain;
            }
        }
        return ClassifyStep(bytes, size, pc, next);
    }

    void Debugger::cancelStepOver()
    {
        if(!mStepOver)
            return;
        std::lock_guard lock(mStateMutex);
        const auto it = mBreakpoints.find(mStepOver->address);
        if(it != mBreakpoints.end() && !it->second.user)
        {
            // A thread stepping off it already has the original code mapped.
            if(!mStepOff || mStepOff->address != it->first)
                WriteMemory(mTask, it->first, it->second.original.data(), kBreakpointSize);
            mBreakpoints.erase(it);
        }
        mStepOver.reset();
    }

    // A single-stepped pushf pushes the trap flag too; a later popf would trap on it.
    void Debugger::maskPushedTrapFlag(const thread_act_t thread)
    {
#if defined(__x86_64__)
        ThreadState state = {};
        uint8_t flagsHigh = 0; // the trap flag is bit 8: bit 0 of the second pushed byte
        if(!ReadThreadState(thread, state) || !ReadMemory(mTask, GetSp(state) + 1, &flagsHigh, 1))
            return;
        flagsHigh &= ~1;
        WriteMemory(mTask, GetSp(state) + 1, &flagsHigh, 1);
#else
        (void)thread;
#endif
    }

    void Debugger::finishStepOff()
    {
        if(!mStepOff)
            return;
        std::lock_guard lock(mStateMutex);
        const auto bp = mBreakpoints.find(mStepOff->address);
        if(bp != mBreakpoints.end())
            WriteMemory(mTask, bp->first, kBreakpointOpcode, kBreakpointSize);
        if(const Thread* thread = findThread(mStepOff->tid))
            SetSingleStep(thread->port, false);
        for(const auto port : mStepOff->held)
        {
            thread_resume(port);
            mach_port_deallocate(mach_task_self(), port);
        }
        mStepOff.reset();
    }

    void Debugger::replyPending()
    {
        if(!mPending)
            return;
        if(isSoftSignal(mPending->message))
            ptrace(PT_THUPDATE, Pid(), reinterpret_cast<caddr_t>(static_cast<uintptr_t>(mPending->message.thread)),
                   mPending->signal);
        ReplyException(mPending->message, mPending->result);
        mPending.reset();
    }

    void Debugger::detachProcess()
    {
        mDetachRequested.store(false, std::memory_order_release);
        suspendTask();
        finishStepOff();
        {
            // Nothing may trap once the process runs on its own.
            std::lock_guard lock(mStateMutex);
            for(const auto & [address, bp] : mBreakpoints)
                WriteMemory(mTask, address, bp.original.data(), kBreakpointSize);
            mBreakpoints.clear();
            if(const Thread* thread = findThread(mStepTid))
                SetSingleStep(thread->port, false);
        }
        mStepTid = 0;
        mStepOver.reset();
        mPushFlagsTid = 0;
        restoreExceptionPorts();

        // Requests that reached our port before the handlers were restored still wait for us.
        ExceptionMessage message;
        while(ReceiveException(mExceptionPort, 0, message))
            mQueued.push_back(message);
        replyPending();
        for(auto & queued : mQueued)
            answerUnreported(queued);
        mQueued.clear();

        if(ptrace(PT_DETACH, Pid(), reinterpret_cast<caddr_t>(1), 0) != 0)
            cbInternalError("PT_DETACH failed: " + std::string(strerror(errno)));
        resumeTask();
        if(mLaunched)
            mDetachedChildren.push_back(Pid());
        mPaused.store(false, std::memory_order_release);
        cbDetach();
    }

    // Lets a request go as if no debugger had been attached when it was raised.
    void Debugger::answerUnreported(ExceptionMessage & message)
    {
        if(isSoftSignal(message))
        {
            const int signal = static_cast<int>(message.code[1]);
            ptrace(PT_THUPDATE, Pid(), reinterpret_cast<caddr_t>(static_cast<uintptr_t>(message.thread)),
                   mAwaitingAttachStop && signal == SIGSTOP ? 0 : signal);
            ReplyException(message, KERN_SUCCESS);
            return;
        }
        if(message.exception != EXC_BREAKPOINT)
        {
            ReplyException(message, KERN_FAILURE);
            return;
        }
        // x86 reports a removed int3 with the pc past it.
        ThreadState state = {};
        uint8_t bytes[kBreakpointSize] = {};
        if(!IsSingleStepTrap(message.code, message.codeCount) && ReadThreadState(message.thread, state))
        {
            const ptr address = TrapAddress(GetPc(state));
            if(ReadMemory(mTask, address, bytes, sizeof(bytes)) && !IsTrapInstruction(bytes, sizeof(bytes)))
            {
                SetPc(state, address);
                WriteThreadState(message.thread, state);
            }
        }
        ReplyException(message, KERN_SUCCESS);
    }

    void Debugger::killProcess()
    {
        if(mKilled || Pid() <= 0)
            return;
        mKilled = true;
        kill(Pid(), SIGKILL);
        // The process can only die once its threads may run again.
        finishStepOff();
        if(mPending)
        {
            ReplyException(mPending->message, KERN_SUCCESS);
            mPending.reset();
        }
        for(auto & message : mQueued)
            ReplyException(message, KERN_SUCCESS);
        mQueued.clear();
        resumeTask();
        mPaused.store(false, std::memory_order_release);
    }

    void Debugger::suspendTask()
    {
        if(!mTaskSuspended && task_suspend(mTask) == KERN_SUCCESS)
            mTaskSuspended = true;
    }

    void Debugger::resumeTask()
    {
        if(mTaskSuspended && task_resume(mTask) == KERN_SUCCESS)
            mTaskSuspended = false;
    }
}

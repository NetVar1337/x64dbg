#include <MachBug/core/Debugger.h>
#include <MachBug/process/Image.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
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
            if(command == Command::Stop)
                killProcess();
            else
                resume(command == Command::StepInto);
        }
    }

    Debugger::Command Debugger::waitForCommand()
    {
        std::unique_lock lock(mCommandMutex);
        mCommandCv.wait(lock, [this]
        {
            return mCommand != Command::None || mStopRequested.load(std::memory_order_acquire);
        });
        if(mStopRequested.load(std::memory_order_acquire))
            mCommand = Command::Stop;
        // Cleared under the lock so a late Continue cannot queue a command for the next stop.
        mPaused.store(false, std::memory_order_release);
        return mCommand;
    }

    void Debugger::stopAt(const uint64_t tid)
    {
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

            if(!mKilled && mPauseRequested.exchange(false, std::memory_order_acq_rel))
            {
                suspendTask();
                return true;
            }

            // A thread that exits while stepping off never reports the step; release the others.
            if(mStepOff && !threadAlive(mStepOff->tid))
                finishStepOff();
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
                cbSystemBreakpoint();
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
            if(HasBreakpoint(address))
            {
                SetPc(state, address);
                WriteThreadState(message.thread, state);
                mPending = PendingReply{message};
                stopAt(tid);
                cbBreakpoint(address);
                return true;
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

    void Debugger::resume(const bool step)
    {
        uint64_t tid = 0;
        thread_act_t port = MACH_PORT_NULL;
        std::optional<ptr> breakpoint;
        {
            std::lock_guard lock(mStateMutex);
            if(const Thread* thread = findThread(mCurrentTid))
            {
                tid = thread->tid;
                port = thread->port;
                ThreadState state = {};
                if(ReadThreadState(port, state) && mBreakpoints.count(GetPc(state)))
                    breakpoint = GetPc(state);
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
            SetSingleStep(port, true);
        mStepTid = step ? tid : 0;
        replyPending();
        resumeTask();
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

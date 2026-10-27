#include <MachBug/core/Debugger.h>
#include <MachBug/process/Image.h>
#include <mach/mach_vm.h>
#include <spawn.h>
#include <sys/ptrace.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstring>

extern char** environ;

namespace MachBug
{
    namespace
    {
        constexpr exception_mask_t kExceptionMask = EXC_MASK_BREAKPOINT | EXC_MASK_BAD_ACCESS |
            EXC_MASK_BAD_INSTRUCTION | EXC_MASK_ARITHMETIC | EXC_MASK_SOFTWARE;

        constexpr const char* kTaskAccessHint =
            "run as root, or sign the debugger with the com.apple.security.cs.debugger entitlement; "
            "Apple's own binaries and hardened apps without get-task-allow cannot be debugged";

        uint64_t threadId(const thread_act_t port)
        {
            thread_identifier_info_data_t info = {};
            mach_msg_type_number_t count = THREAD_IDENTIFIER_INFO_COUNT;
            if(thread_info(port, THREAD_IDENTIFIER_INFO, reinterpret_cast<thread_info_t>(&info), &count) != KERN_SUCCESS)
                return 0;
            return info.thread_id;
        }
    }

    Debugger::Debugger() = default;

    Debugger::~Debugger()
    {
        const pid_t pid = mPid.load(std::memory_order_acquire);
        if(pid > 0 && !mExited && mLaunched)
        {
            killProcess();
            waitpid(pid, nullptr, 0);
        }
        endSession();
        reapDetachedChildren();
    }

    bool Debugger::Init(const char* path, const char* const* argv, const char* workingDirectory)
    {
        if(mIsRunning.load(std::memory_order_acquire))
        {
            cbInternalError("cannot launch '" + std::string(path ? path : "") + "': the debug loop is still running");
            return false;
        }

        mHasLaunchArgs = false;
        mAttachPid = 0;
        if(!path)
        {
            cbInternalError("cannot launch: no path");
            return false;
        }

        const std::string filePath(path);
        struct stat info = {};
        if(stat(path, &info) != 0)
        {
            cbInternalError("cannot execute '" + filePath + "': " + strerror(errno));
            return false;
        }
        if(!S_ISREG(info.st_mode))
        {
            cbInternalError("cannot execute '" + filePath + "': not a regular file");
            return false;
        }
        if(access(path, X_OK) != 0)
        {
            cbInternalError("cannot execute '" + filePath + "': " + strerror(errno));
            return false;
        }

        mPath = filePath;
        mCwd = workingDirectory ? workingDirectory : "";
        mArgv.clear();
        if(argv)
        {
            for(const char* const* p = argv; *p; ++p)
                mArgv.emplace_back(*p);
        }
        if(mArgv.empty())
            mArgv.push_back(mPath);
        mHasLaunchArgs = true;
        return true;
    }

    bool Debugger::Attach(const pid_t processId)
    {
        const std::string target = "cannot attach to pid " + std::to_string(processId) + ": ";
        if(mIsRunning.load(std::memory_order_acquire))
        {
            cbInternalError(target + "the debug loop is still running");
            return false;
        }

        mHasLaunchArgs = false;
        mAttachPid = 0;
        if(processId <= 0 || processId == getpid())
        {
            cbInternalError(target + "not a debuggable process");
            return false;
        }
        if(kill(processId, 0) != 0 && errno == ESRCH)
        {
            cbInternalError(target + "no such process");
            return false;
        }
        mAttachPid = processId;
        return true;
    }

    void Debugger::Start()
    {
        if(!mHasLaunchArgs && mAttachPid == 0)
        {
            cbInternalError("Start called without Init or Attach");
            return;
        }
        if(mIsRunning.exchange(true, std::memory_order_acq_rel))
        {
            cbInternalError("Start called while the debug loop is running");
            return;
        }
        const bool attaching = mAttachPid != 0;
        mHasLaunchArgs = false;

        beginSession();
        if(attaching ? attach() : launch())
            debugLoop();
        endSession();
        mIsRunning.store(false, std::memory_order_release);
    }

    void Debugger::beginSession()
    {
        reapDetachedChildren();
        mExited = false;
        mKilled = false;
        mStopRequested.store(false, std::memory_order_release);
        mPauseRequested.store(false, std::memory_order_release);
        mDetachRequested.store(false, std::memory_order_release);
        std::lock_guard lock(mCommandMutex);
        mCommand = Command::None;
    }

    bool Debugger::launch()
    {
        posix_spawnattr_t attr;
        posix_spawnattr_init(&attr);
        // Suspended before dyld runs, and in its own process group like a shell job.
        posix_spawnattr_setflags(&attr, POSIX_SPAWN_START_SUSPENDED | POSIX_SPAWN_SETPGROUP);
        posix_spawnattr_setpgroup(&attr, 0);
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        if(!mCwd.empty())
            posix_spawn_file_actions_addchdir_np(&actions, mCwd.c_str());

        std::vector<char*> argv;
        for(auto & arg : mArgv)
            argv.push_back(arg.data());
        argv.push_back(nullptr);

        pid_t pid = 0;
        const int spawnError = posix_spawn(&pid, mPath.c_str(), &actions, &attr, argv.data(), environ);
        posix_spawn_file_actions_destroy(&actions);
        posix_spawnattr_destroy(&attr);
        if(spawnError != 0)
        {
            cbInternalError("cannot launch '" + mPath + "': " + strerror(spawnError));
            return false;
        }
        mPid.store(pid, std::memory_order_release);
        mLaunched = true;

        const auto fail = [&](const std::string & error)
        {
            cbInternalError(error);
            kill(pid, SIGKILL);
            waitpid(pid, nullptr, 0);
            mExited = true;
            return false;
        };

        const kern_return_t kr = task_for_pid(mach_task_self(), pid, &mTask);
        if(kr != KERN_SUCCESS)
        {
            mTask = MACH_PORT_NULL;
            return fail(std::string("task_for_pid failed: ") + mach_error_string(kr) + " (" + kTaskAccessHint + ")");
        }

        if(!installExceptionPort())
            return fail("cannot set the exception port of pid " + std::to_string(pid));

        // Signals now arrive as EXC_SOFT_SIGNAL, and the process may run patched code pages.
        // Attaching also lifts the spawn suspension; the SIGSTOP it queues becomes the
        // system breakpoint, raised before the program runs an instruction of its own.
        if(ptrace(PT_ATTACHEXC, pid, nullptr, 0) != 0)
            return fail(std::string("PT_ATTACHEXC failed: ") + strerror(errno));
        mAwaitingAttachStop = true;
        return true;
    }

    bool Debugger::attach()
    {
        const pid_t pid = mAttachPid;
        mAttachPid = 0;
        const std::string target = "cannot attach to pid " + std::to_string(pid) + ": ";

        const kern_return_t kr = task_for_pid(mach_task_self(), pid, &mTask);
        if(kr != KERN_SUCCESS)
        {
            mTask = MACH_PORT_NULL;
            cbInternalError(target + "task_for_pid failed: " + mach_error_string(kr) + " (" + kTaskAccessHint + ")");
            return false;
        }
        if(!installExceptionPort())
        {
            cbInternalError(target + "cannot set its exception port");
            return false;
        }
        // Signals now arrive as EXC_SOFT_SIGNAL; the SIGSTOP it queues becomes the attach stop.
        if(ptrace(PT_ATTACHEXC, pid, nullptr, 0) != 0)
        {
            const int error = errno;
            restoreExceptionPorts();
            cbInternalError(target + (error == EBUSY ? std::string("it is already being debugged")
                                      : std::string("PT_ATTACHEXC failed: ") + strerror(error)));
            return false;
        }
        mPid.store(pid, std::memory_order_release);
        mLaunched = false;
        mAwaitingAttachStop = true;
        return true;
    }

    bool Debugger::installExceptionPort()
    {
        mOriginalPorts.count = EXC_TYPES_COUNT;
        if(task_get_exception_ports(mTask, kExceptionMask, mOriginalPorts.masks, &mOriginalPorts.count, mOriginalPorts.ports,
                                    mOriginalPorts.behaviors, mOriginalPorts.flavors) != KERN_SUCCESS)
            mOriginalPorts.count = 0;
        return mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &mExceptionPort) == KERN_SUCCESS &&
               mach_port_insert_right(mach_task_self(), mExceptionPort, mExceptionPort, MACH_MSG_TYPE_MAKE_SEND) == KERN_SUCCESS &&
               task_set_exception_ports(mTask, kExceptionMask, mExceptionPort, EXCEPTION_DEFAULT | MACH_EXCEPTION_CODES,
                                        THREAD_STATE_NONE) == KERN_SUCCESS;
    }

    void Debugger::restoreExceptionPorts()
    {
        // Masks without a handler stay pointed at our port otherwise.
        task_set_exception_ports(mTask, kExceptionMask, MACH_PORT_NULL, EXCEPTION_DEFAULT, THREAD_STATE_NONE);
        for(mach_msg_type_number_t i = 0; i < mOriginalPorts.count; ++i)
            task_set_exception_ports(mTask, mOriginalPorts.masks[i], mOriginalPorts.ports[i], mOriginalPorts.behaviors[i],
                                     mOriginalPorts.flavors[i]);
    }

    void Debugger::reapDetachedChildren()
    {
        std::erase_if(mDetachedChildren, [](const pid_t pid)
        {
            const pid_t reaped = waitpid(pid, nullptr, WNOHANG);
            return reaped == pid || (reaped == -1 && errno == ECHILD);
        });
    }

    void Debugger::endSession()
    {
        std::lock_guard lock(mStateMutex);
        if(mStepOff)
        {
            for(const auto port : mStepOff->held)
                mach_port_deallocate(mach_task_self(), port);
            mStepOff.reset();
        }
        if(mPending)
        {
            ReplyException(mPending->message, KERN_SUCCESS);
            mPending.reset();
        }
        for(auto & message : mQueued)
            ReplyException(message, KERN_SUCCESS);
        mQueued.clear();
        for(const auto & thread : mThreads)
            mach_port_deallocate(mach_task_self(), thread.port);
        mThreads.clear();
        mForwardedSignals.clear();
        mBreakpoints.clear();
        mCurrentTid = 0;
        mStepTid = 0;
        mStepOver.reset();
        mPushFlagsTid = 0;
        mModules.clear();
        mStopCount = 0;
        mModulesStop = 0;
        mNextThreadNumber = 0;
        mAwaitingAttachStop = false;
        mLaunched = false;
        mDetachRequested.store(false, std::memory_order_release);
        for(mach_msg_type_number_t i = 0; i < mOriginalPorts.count; ++i)
        {
            if(MACH_PORT_VALID(mOriginalPorts.ports[i]))
                mach_port_deallocate(mach_task_self(), mOriginalPorts.ports[i]);
        }
        mOriginalPorts.count = 0;
        if(mExceptionPort != MACH_PORT_NULL)
        {
            mach_port_deallocate(mach_task_self(), mExceptionPort);
            mach_port_mod_refs(mach_task_self(), mExceptionPort, MACH_PORT_RIGHT_RECEIVE, -1);
            mExceptionPort = MACH_PORT_NULL;
        }
        if(mTask != MACH_PORT_NULL)
        {
            mach_port_deallocate(mach_task_self(), mTask);
            mTask = MACH_PORT_NULL;
        }
        mTaskSuspended = false;
        mPid.store(0, std::memory_order_release);
        mPaused.store(false, std::memory_order_release);
    }

    void Debugger::Continue()
    {
        std::lock_guard lock(mCommandMutex);
        if(!mPaused.load(std::memory_order_acquire))
            return;
        mCommand = Command::Continue;
        mCommandCv.notify_all();
    }

    void Debugger::StepInto()
    {
        std::lock_guard lock(mCommandMutex);
        if(!mPaused.load(std::memory_order_acquire))
            return;
        mCommand = Command::StepInto;
        mCommandCv.notify_all();
    }

    void Debugger::StepOver()
    {
        std::lock_guard lock(mCommandMutex);
        if(!mPaused.load(std::memory_order_acquire))
            return;
        mCommand = Command::StepOver;
        mCommandCv.notify_all();
    }

    void Debugger::Pause()
    {
        if(mIsRunning.load(std::memory_order_acquire) && !mPaused.load(std::memory_order_acquire))
            mPauseRequested.store(true, std::memory_order_release);
    }

    bool Debugger::Stop()
    {
        if(!mIsRunning.load(std::memory_order_acquire) || Pid() <= 0)
            return false;
        std::lock_guard lock(mCommandMutex);
        mStopRequested.store(true, std::memory_order_release);
        mCommandCv.notify_all();
        return true;
    }

    bool Debugger::Detach()
    {
        if(!mIsRunning.load(std::memory_order_acquire) || Pid() <= 0)
            return false;
        std::lock_guard lock(mCommandMutex);
        if(mDetachRequested.exchange(true, std::memory_order_acq_rel))
            return false;
        mCommandCv.notify_all();
        return true;
    }

    std::vector<uint64_t> Debugger::ThreadIds() const
    {
        std::lock_guard lock(mStateMutex);
        std::vector<uint64_t> tids;
        for(const auto & thread : mThreads)
            tids.push_back(thread.tid);
        return tids;
    }

    uint64_t Debugger::CurrentThreadId() const
    {
        if(!IsPaused())
            return 0;
        std::lock_guard lock(mStateMutex);
        return mCurrentTid;
    }

    bool Debugger::SwitchThread(const uint64_t tid)
    {
        if(!IsPaused())
            return false;
        std::lock_guard lock(mStateMutex);
        if(!findThread(tid))
            return false;
        mCurrentTid = tid;
        return true;
    }

    bool Debugger::GetRegisters(ThreadState & state) const
    {
        if(!IsPaused())
            return false;
        std::lock_guard lock(mStateMutex);
        const Thread* thread = findThread(mCurrentTid);
        return thread && ReadThreadState(thread->port, state);
    }

    bool Debugger::SetRegister(const char* name, const uint64_t value)
    {
        if(!IsPaused() || !name)
            return false;
        std::lock_guard lock(mStateMutex);
        const Thread* thread = findThread(mCurrentTid);
        ThreadState state = {};
        return thread && ReadThreadState(thread->port, state) && MachBug::SetRegister(state, name, value) &&
               WriteThreadState(thread->port, state);
    }

    bool Debugger::MemRead(const ptr address, void* dest, const size_t size) const
    {
        std::lock_guard lock(mStateMutex);
        if(mTask == MACH_PORT_NULL || !ReadMemory(mTask, address, dest, size))
            return false;
        auto* bytes = static_cast<uint8_t*>(dest);
        for(const auto & [bpAddress, bp] : mBreakpoints)
        {
            for(size_t i = 0; i < kBreakpointSize; ++i)
            {
                const ptr at = bpAddress + i;
                if(at >= address && at - address < size)
                    bytes[at - address] = bp.original[i];
            }
        }
        return true;
    }

    bool Debugger::MemWrite(const ptr address, const void* src, const size_t size)
    {
        std::lock_guard lock(mStateMutex);
        if(mTask == MACH_PORT_NULL)
            return false;

        // New bytes under a breakpoint become its original code; the trap stays in memory.
        std::vector<uint8_t> bytes(static_cast<const uint8_t*>(src), static_cast<const uint8_t*>(src) + size);
        auto breakpoints = mBreakpoints;
        for(auto & [bpAddress, bp] : breakpoints)
        {
            for(size_t i = 0; i < kBreakpointSize; ++i)
            {
                const ptr at = bpAddress + i;
                if(at < address || at - address >= size)
                    continue;
                bp.original[i] = bytes[at - address];
                // While a thread steps off this breakpoint its original code is what is mapped.
                if(!mStepOff || mStepOff->address != bpAddress)
                    bytes[at - address] = kBreakpointOpcode[i];
            }
        }
        if(!WriteMemory(mTask, address, bytes.data(), bytes.size()))
            return false;
        mBreakpoints = std::move(breakpoints);
        return true;
    }

    std::vector<MemoryRegion> Debugger::MemoryMap() const
    {
        std::lock_guard lock(mStateMutex);
        if(mTask == MACH_PORT_NULL)
            return {};
        return ReadMemoryMap(mTask);
    }

    std::vector<Module> Debugger::Modules() const
    {
        std::lock_guard lock(mStateMutex);
        if(mTask != MACH_PORT_NULL && mModulesStop != mStopCount)
        {
            mModules = ReadModules(mTask, Pid(), mModules);
            mModulesStop = mStopCount;
        }
        return mModules;
    }

    bool Debugger::ModuleFromAddress(const ptr address, Module & module) const
    {
        for(auto & candidate : Modules())
        {
            if(candidate.Contains(address))
            {
                module = std::move(candidate);
                return true;
            }
        }
        return false;
    }

    bool Debugger::SetBreakpoint(const ptr address)
    {
        std::lock_guard lock(mStateMutex);
        if(mTask == MACH_PORT_NULL || address % kBreakpointSize != 0)
            return false;
        const auto existing = mBreakpoints.find(address);
        if(existing != mBreakpoints.end())
        {
            existing->second.user = true;
            return true;
        }

        Breakpoint bp;
        if(!ReadMemory(mTask, address, bp.original.data(), kBreakpointSize) ||
                !WriteMemory(mTask, address, kBreakpointOpcode, kBreakpointSize))
            return false;
        mBreakpoints.emplace(address, bp);
        return true;
    }

    bool Debugger::DeleteBreakpoint(const ptr address)
    {
        std::lock_guard lock(mStateMutex);
        const auto it = mBreakpoints.find(address);
        if(it == mBreakpoints.end() || !it->second.user)
            return false;
        // A pending step over still needs the trap there.
        if(mStepOver && mStepOver->address == address)
        {
            it->second.user = false;
            return true;
        }
        if(mTask != MACH_PORT_NULL && !WriteMemory(mTask, address, it->second.original.data(), kBreakpointSize))
            return false;
        mBreakpoints.erase(it);
        return true;
    }

    bool Debugger::HasBreakpoint(const ptr address) const
    {
        std::lock_guard lock(mStateMutex);
        const auto it = mBreakpoints.find(address);
        return it != mBreakpoints.end() && it->second.user;
    }

    // Mach has no thread-creation event, so threads are discovered at each stop.
    void Debugger::refreshThreads(const bool report)
    {
        thread_act_array_t list = nullptr;
        mach_msg_type_number_t count = 0;
        if(task_threads(mTask, &list, &count) != KERN_SUCCESS)
            return;

        std::vector<uint64_t> created;
        std::vector<uint64_t> exited;
        {
            std::lock_guard lock(mStateMutex);
            std::vector<Thread> current;
            for(mach_msg_type_number_t i = 0; i < count; ++i)
            {
                const auto known = std::find_if(mThreads.begin(), mThreads.end(), [&](const Thread & thread)
                {
                    return thread.port == list[i];
                });
                if(known != mThreads.end())
                {
                    current.push_back(*known);
                    // task_threads added a reference to a port already held.
                    mach_port_deallocate(mach_task_self(), list[i]);
                    continue;
                }
                current.push_back({threadId(list[i]), list[i], mNextThreadNumber++});
                if(report)
                    created.push_back(current.back().tid);
            }
            for(const auto & thread : mThreads)
            {
                const bool alive = std::any_of(current.begin(), current.end(), [&](const Thread & other)
                {
                    return other.port == thread.port;
                });
                if(alive)
                    continue;
                exited.push_back(thread.tid);
                mach_port_deallocate(mach_task_self(), thread.port);
            }
            std::sort(current.begin(), current.end(), [](const Thread & a, const Thread & b)
            {
                return a.number < b.number;
            });
            mThreads = std::move(current);
            if(!findThread(mCurrentTid))
                mCurrentTid = mThreads.empty() ? 0 : mThreads.front().tid;
        }
        vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(list), count * sizeof(thread_act_t));

        for(const uint64_t tid : created)
            cbCreateThread(tid);
        for(const uint64_t tid : exited)
        {
            mForwardedSignals.erase(tid);
            cbExitThread(tid);
        }
    }

    Debugger::Thread* Debugger::findThread(const uint64_t tid)
    {
        const auto it = std::find_if(mThreads.begin(), mThreads.end(), [tid](const Thread & thread)
        {
            return thread.tid == tid;
        });
        return it == mThreads.end() ? nullptr : &*it;
    }

    const Debugger::Thread* Debugger::findThread(const uint64_t tid) const
    {
        return const_cast<Debugger*>(this)->findThread(tid);
    }

    bool Debugger::threadAlive(const uint64_t tid)
    {
        std::lock_guard lock(mStateMutex);
        const Thread* thread = findThread(tid);
        return thread && threadId(thread->port) == tid;
    }

    Debugger::Thread* Debugger::findThreadByPort(const thread_act_t port)
    {
        const auto it = std::find_if(mThreads.begin(), mThreads.end(), [port](const Thread & thread)
        {
            return thread.port == port;
        });
        return it == mThreads.end() ? nullptr : &*it;
    }

    void Debugger::cbCreateProcess(pid_t, ptr) {}
    void Debugger::cbExitProcess(int) {}
    void Debugger::cbCreateThread(uint64_t) {}
    void Debugger::cbExitThread(uint64_t) {}
    void Debugger::cbSystemBreakpoint() {}
    void Debugger::cbAttachBreakpoint() {}
    void Debugger::cbDetach() {}
    void Debugger::cbBreakpoint(ptr) {}
    void Debugger::cbStep() {}
    void Debugger::cbPaused() {}
    void Debugger::cbException(int, ptr) {}
    void Debugger::cbInternalError(const std::string &) {}
}

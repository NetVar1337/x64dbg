#pragma once

#include <MachBug/core/ExceptionMessage.h>
#include <MachBug/process/Memory.h>
#include <MachBug/thread/Arch.h>
#include <mach/mach.h>
#include <sys/types.h>
#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace MachBug
{
    // Debugs one launched or attached process through its Mach exception port. Every stop
    // suspends the whole task, so a reported stop freezes all threads.
    class Debugger
    {
    public:
        Debugger();
        virtual ~Debugger();

        Debugger(const Debugger &) = delete;
        Debugger & operator=(const Debugger &) = delete;

        bool Init(const char* path, const char* const* argv = nullptr, const char* workingDirectory = nullptr);
        bool Attach(pid_t processId);
        void Start();    // Blocks - runs the debug loop until the process is gone or detached
        void Continue(); // Thread-safe
        void StepInto(); // Thread-safe
        void Pause();    // Thread-safe
        bool Stop();     // Thread-safe
        bool Detach();   // Thread-safe

        [[nodiscard]] bool IsPaused() const { return mPaused.load(std::memory_order_acquire); }
        [[nodiscard]] pid_t Pid() const { return mPid.load(std::memory_order_acquire); }
        [[nodiscard]] ptr EntryPoint() const { return mEntryPoint; }

        // System thread ids (as in pthread_threadid_np), in creation order.
        [[nodiscard]] std::vector<uint64_t> ThreadIds() const;
        [[nodiscard]] uint64_t CurrentThreadId() const;
        bool SwitchThread(uint64_t tid);

        // Registers of the current thread.
        bool GetRegisters(ThreadState & state) const;
        bool SetRegister(const char* name, uint64_t value);

        // Breakpoint bytes read back as the code they replaced.
        bool MemRead(ptr address, void* dest, size_t size) const;
        bool MemWrite(ptr address, const void* src, size_t size);
        [[nodiscard]] std::vector<MemoryRegion> MemoryMap() const;

        bool SetBreakpoint(ptr address);
        bool DeleteBreakpoint(ptr address);
        [[nodiscard]] bool HasBreakpoint(ptr address) const;

    protected:
        virtual void cbCreateProcess(pid_t pid, ptr entryPoint);
        virtual void cbExitProcess(int exitCode); // negative: killed by that signal
        virtual void cbCreateThread(uint64_t tid);
        virtual void cbExitThread(uint64_t tid);
        virtual void cbSystemBreakpoint();
        virtual void cbAttachBreakpoint();
        virtual void cbDetach();
        virtual void cbBreakpoint(ptr address);
        virtual void cbStep();
        virtual void cbPaused();
        // signal is what the process receives on Continue; address is the fault address, if any.
        virtual void cbException(int signal, ptr address);
        virtual void cbInternalError(const std::string & error);

    private:
        struct Thread
        {
            uint64_t tid = 0;
            thread_act_t port = MACH_PORT_NULL;
            uint32_t number = 0;
        };

        struct Breakpoint
        {
            std::array<uint8_t, kBreakpointSize> original = {};
        };

        enum class Command
        {
            None,
            Continue,
            StepInto,
            Stop,
            Detach,
        };

        // What to do with the exception that caused the current stop when the thread resumes.
        struct PendingReply
        {
            ExceptionMessage message;
            kern_return_t result = KERN_SUCCESS;
            int signal = 0; // for EXC_SOFT_SIGNAL: the signal to deliver, 0 to discard
        };

        struct StepOff
        {
            uint64_t tid = 0;
            ptr address = 0;
            std::vector<thread_act_t> held;
        };

        void beginSession();
        bool launch();
        bool attach();
        bool installExceptionPort();
        void restoreExceptionPorts();
        void detachProcess();
        void answerUnreported(ExceptionMessage & message);
        void reapDetachedChildren();
        void debugLoop();
        void endSession();
        bool nextEvent(std::optional<ExceptionMessage> & message);
        bool handleException(ExceptionMessage & message);
        bool handleBreakpoint(uint64_t tid, ExceptionMessage & message);
        bool reportPause();
        void reportException(uint64_t tid, ExceptionMessage & message, int signal, ptr address, kern_return_t result);
        void stopAt(uint64_t tid);
        void resumeSilently(ExceptionMessage & message, int signal = 0);
        bool threadAlive(uint64_t tid);
        Command waitForCommand();
        void resume(bool step);
        void finishStepOff();
        void replyPending();
        void killProcess();
        bool reapExit();
        void suspendTask();
        void resumeTask();
        void refreshThreads(bool report);
        Thread* findThreadByPort(thread_act_t port);
        Thread* findThread(uint64_t tid);
        const Thread* findThread(uint64_t tid) const;
        bool writeBreakpointBytes(ptr address, const uint8_t* bytes);

        std::string mPath;
        std::vector<std::string> mArgv;
        std::string mCwd;
        bool mHasLaunchArgs = false;
        pid_t mAttachPid = 0;
        bool mLaunched = false;
        // Detached children are still ours to reap once they exit.
        std::vector<pid_t> mDetachedChildren;

        std::atomic<bool> mIsRunning{false};
        std::atomic<bool> mPaused{false};
        std::atomic<bool> mPauseRequested{false};
        std::atomic<bool> mStopRequested{false};
        std::atomic<bool> mDetachRequested{false};
        std::atomic<pid_t> mPid{0};
        bool mExited = false;
        bool mKilled = false;
        ptr mEntryPoint = 0;

        task_t mTask = MACH_PORT_NULL;
        mach_port_t mExceptionPort = MACH_PORT_NULL;
        // The handlers the process had, restored on detach.
        struct
        {
            exception_mask_t masks[EXC_TYPES_COUNT] = {};
            mach_port_t ports[EXC_TYPES_COUNT] = {};
            exception_behavior_t behaviors[EXC_TYPES_COUNT] = {};
            thread_state_flavor_t flavors[EXC_TYPES_COUNT] = {};
            mach_msg_type_number_t count = 0;
        } mOriginalPorts;
        bool mTaskSuspended = false;
        // PT_ATTACHEXC queues a SIGSTOP; it is not the program's.
        bool mAwaitingAttachStop = false;
        uint32_t mNextThreadNumber = 0;

        // Guards the state the API reads from other threads.
        mutable std::mutex mStateMutex;
        std::vector<Thread> mThreads;
        uint64_t mCurrentTid = 0;
        std::map<ptr, Breakpoint> mBreakpoints;

        std::optional<PendingReply> mPending;
        // Requests from other threads that arrived while one thread was stepping off a breakpoint.
        std::deque<ExceptionMessage> mQueued;
        std::optional<StepOff> mStepOff;
        uint64_t mStepTid = 0;
        // A fault passed on to the process comes back as this signal; it is delivered unreported.
        std::map<uint64_t, int> mForwardedSignals;

        std::mutex mCommandMutex;
        std::condition_variable mCommandCv;
        Command mCommand = Command::None;
    };
}

#pragma once

#include <mach/mach.h>
#include <cstdint>

namespace MachBug
{
    // A mach_exception_raise request (EXCEPTION_DEFAULT | MACH_EXCEPTION_CODES). The thread
    // that raised it stays blocked in the kernel until the request is replied to.
    struct ExceptionMessage
    {
        thread_act_t thread = MACH_PORT_NULL; // holds a send right until the reply
        exception_type_t exception = 0;
        int64_t code[2] = {};
        mach_msg_type_number_t codeCount = 0;
        mach_port_t replyPort = MACH_PORT_NULL; // send-once right, consumed by the reply
        mach_msg_id_t id = 0;
    };

    // False on timeout. Messages that are not exception requests are dropped.
    bool ReceiveException(mach_port_t port, mach_msg_timeout_t timeoutMs, ExceptionMessage & message);

    // KERN_SUCCESS resumes the thread; KERN_FAILURE passes the exception on, which ends in a signal.
    void ReplyException(ExceptionMessage & message, kern_return_t result);
}

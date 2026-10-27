#include <MachBug/core/ExceptionMessage.h>

namespace MachBug
{
    namespace
    {
        constexpr mach_msg_id_t kMachExceptionRaise = 2405;
        constexpr mach_msg_id_t kReplyOffset = 100;

        // Layouts from mach_exc.defs, as MIG generates them.
#pragma pack(push, 4)
        struct Request
        {
            mach_msg_header_t header;
            mach_msg_body_t body;
            mach_msg_port_descriptor_t thread;
            mach_msg_port_descriptor_t task;
            NDR_record_t ndr;
            exception_type_t exception;
            mach_msg_type_number_t codeCount;
            int64_t code[2];
        };

        struct Reply
        {
            mach_msg_header_t header;
            NDR_record_t ndr;
            kern_return_t result;
        };
#pragma pack(pop)

        union ReceiveBuffer
        {
            Request request;
            uint8_t raw[sizeof(Request) + MAX_TRAILER_SIZE];
        };
    }

    bool ReceiveException(const mach_port_t port, const mach_msg_timeout_t timeoutMs, ExceptionMessage & message)
    {
        for(;;)
        {
            ReceiveBuffer buffer = {};
            const kern_return_t kr = mach_msg(&buffer.request.header, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0,
                                              sizeof(buffer), port, timeoutMs, MACH_PORT_NULL);
            if(kr != MACH_MSG_SUCCESS)
                return false;

            const Request & request = buffer.request;
            if(request.header.msgh_id != kMachExceptionRaise || !(request.header.msgh_bits & MACH_MSGH_BITS_COMPLEX) ||
                    request.body.msgh_descriptor_count != 2 || request.codeCount > 2)
            {
                mach_msg_destroy(&buffer.request.header);
                continue;
            }

            // The task right is not needed: the debugger keeps its own.
            mach_port_deallocate(mach_task_self(), request.task.name);

            message.thread = request.thread.name;
            message.exception = request.exception;
            message.codeCount = request.codeCount;
            message.code[0] = request.codeCount > 0 ? request.code[0] : 0;
            message.code[1] = request.codeCount > 1 ? request.code[1] : 0;
            message.replyPort = request.header.msgh_remote_port;
            message.id = request.header.msgh_id;
            return true;
        }
    }

    void ReplyException(ExceptionMessage & message, const kern_return_t result)
    {
        if(message.replyPort != MACH_PORT_NULL)
        {
            Reply reply = {};
            reply.header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND_ONCE, 0);
            reply.header.msgh_size = sizeof(reply);
            reply.header.msgh_remote_port = message.replyPort;
            reply.header.msgh_local_port = MACH_PORT_NULL;
            reply.header.msgh_id = message.id + kReplyOffset;
            reply.ndr = NDR_record;
            reply.result = result;
            // A dead reply port only means the thread is already gone.
            if(mach_msg(&reply.header, MACH_SEND_MSG, sizeof(reply), 0, MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE,
                        MACH_PORT_NULL) != MACH_MSG_SUCCESS)
                mach_port_deallocate(mach_task_self(), message.replyPort);
            message.replyPort = MACH_PORT_NULL;
        }
        if(message.thread != MACH_PORT_NULL)
        {
            mach_port_deallocate(mach_task_self(), message.thread);
            message.thread = MACH_PORT_NULL;
        }
    }
}

#include "net/ctrlimpl/socket_ctrl_common.h"
#include "net/socket.h"
#include "base/strtool.h"
#include "base/task_queue_impl.h"

#include <string.h>
#include <assert.h>
#include <stdlib.h>
#include <iostream>
#include <sstream>
using namespace std;

using namespace ff;

SocketCtrlCommon::SocketCtrlCommon(msg_handler_ptr_t msg_handler_):
    m_msg_handler(msg_handler_),
    m_have_recv_size(0)
{
}

SocketCtrlCommon::~SocketCtrlCommon()
{
}

//! when socket broken(whenever), this function will be called
//! this func just callback logic layer to process this event
//! each socket broken event only can happen once
//! logic layer has responsibily to deconstruct the socket object
int SocketCtrlCommon::handleError(SocketI* sp_)
{
    if (m_msg_handler->getTqPtr()){
        m_msg_handler->getTqPtr()->produce(TaskBinder::gen(&MsgHandlerI::handleBroken, m_msg_handler, sp_));
    }
    else{
        m_msg_handler->handleBroken(sp_);
    }
    return 0;
}

int SocketCtrlCommon::handleRead(SocketI* sp_, const char* buff, size_t len)
{
    size_t left_len = len;
    size_t tmp      = 0;
    
    while (left_len > 0)
    {
        if (false == m_message.have_recv_head(m_have_recv_size))
        {
            tmp = m_message.append_head(m_have_recv_size, buff, left_len);

            m_have_recv_size += tmp;
            left_len         -= tmp;
            buff             += tmp;
        }
        
        tmp = m_message.append_msg(buff, left_len);
        m_have_recv_size += tmp;
        left_len         -= tmp;
        buff             += tmp;
        
        if (m_message.get_body().size() == m_message.size())
        {
            this->post_msg(sp_);
            m_have_recv_size = 0;
            m_message.clear();
        }
    }

    return 0;
}

void SocketCtrlCommon::post_msg(SocketI* sp_)
{
    if (m_msg_handler->getTqPtr()){
        m_msg_handler->getTqPtr()->produce(TaskBinder::gen(&MsgHandlerI::handleMsg,
                                             m_msg_handler, m_message, sp_));
    }
    else{
        m_msg_handler->handleMsg(m_message, sp_);
    }
    m_message.clear();
}

//! 当数据全部发送完毕后，此函数会被回调
int SocketCtrlCommon::handleWriteCompleted(SocketI* sp_)
{
    return 0;
}

int SocketCtrlCommon::checkPreSend(SocketI* sp_, const string& buff, int flag)
{
    if (sp_->socket() < 0)
    {
        return -1;
    }
    return 0;
}

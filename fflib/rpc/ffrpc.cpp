#include "rpc/ffrpc.h"
#include "rpc/ffrpc_ops.h"
#include "base/log.h"
#include "net/net_factory.h"
#include "rpc/ffbroker.h"
using namespace std;
using namespace ff;

#define FFRPC                   "FFRPC"

FFRpc::FFRpc(string service_name_):
    m_runing(false),
    m_service_name(service_name_),
    m_node_id(0),
    m_callback_id(0),
    m_timer(&m_tq),
    m_master_broker_sock(NULL),
    m_bind_broker_id(0)
{
}

FFRpc::~FFRpc()
{
    this->close();
}
int FFRpc::open(ArgHelper& arg_helper)
{
    string arg = arg_helper.getOptionValue("-master_broker");
    if (arg.empty())
    {
        arg = arg_helper.getOptionValue("-broker");
    }
    return open(arg);
}
int FFRpc::open(const string& broker_addr)
{
    NetFactory::start(1);
    m_host = broker_addr;

    m_thread.create_thread(TaskBinder::gen(&TaskQueue::run, &m_tq), 1);
            
    //!新版本
    m_ffslot.bind(REGISTER_TO_BROKER_RET, FFRpcOps::gen_callback(&FFRpc::handle_broker_reg_response, this))
            .bind(BROKER_TO_CLIENT_MSG, FFRpcOps::gen_callback(&FFRpc::handle_rpc_call_msg, this));

    m_master_broker_sock = connect_to_broker(m_host, BROKER_MASTER_NODE_ID);

    if (NULL == m_master_broker_sock)
    {
        LOGERROR((FFRPC, "FFRpc::open failed, can't connect to remote broker<%s>", m_host.c_str()));
        return -1;
    }

    while(m_node_id == 0)
    {
        usleep(1);
        if (m_master_broker_sock == NULL)
        {
            LOGERROR((FFRPC, "FFRpc::open failed"));
            return -1;
        }
    }
    m_runing = true;
    LOGTRACE((FFRPC, "FFRpc::open end ok m_node_id[%u]", m_node_id));
    return 0;
}
int FFRpc::close()
{
    if (false == m_runing)
    {
        return 0;
    }
    m_runing = false;
    m_timer.stop(true);
    if (m_master_broker_sock)
    {
        m_master_broker_sock->close();
    }
    map<uint64_t, socket_ptr_t>::iterator it = m_broker_sockets.begin();
    for (; it != m_broker_sockets.end(); ++it)
    {
        it->second->close();
    }
    m_tq.close();
    m_thread.join();
    //usleep(100);
    return 0;
}

//! 连接到broker master
socket_ptr_t FFRpc::connect_to_broker(const string& host_, uint32_t node_id_)
{
    LOGINFO((FFRPC, "FFRpc::connect_to_broker begin...host_<%s>,node_id_[%u]", host_.c_str(), node_id_));
    socket_ptr_t sock = NetFactory::connect(host_, this);
    if (NULL == sock)
    {
        LOGERROR((FFRPC, "FFRpc::register_to_broker_master failed, can't connect to remote broker<%s>", host_.c_str()));
        return sock;
    }
    session_data_t* psession = new session_data_t(node_id_);
    sock->set_data(psession);

    //!新版发送注册消息
    RegisterToBroker::in_t reg_msg;
    reg_msg.node_type = RPC_NODE;
    reg_msg.service_name = m_service_name;
    reg_msg.node_id = m_node_id;
    reg_msg.bind_broker_id = Singleton<FFRpcMemoryRoute>::instance().get_broker_in_mem();
    msg_sender_t::send(sock, REGISTER_TO_BROKER_REQ, ffthrift_t::EncodeAsString(reg_msg));
    
    LOGINFO((FFRPC, "FFRpc::connect_to_broker end bind_broker_id=%d", reg_msg.bind_broker_id));
    return sock;
}
//! 投递到ffrpc 特定的线程
static void route_call_reconnect(FFRpc* ffrpc_)
{
    if (ffrpc_->m_runing == false)
        return;
    ffrpc_->get_tq().produce(TaskBinder::gen(&FFRpc::timer_reconnect_broker, ffrpc_));
}
//! 定时重连 broker master
void FFRpc::timer_reconnect_broker()
{
    LOGINFO((FFRPC, "FFRpc::timer_reconnect_broker begin..."));
    if (m_runing == false)
        return;

    if (NULL == m_master_broker_sock)
    {
        m_master_broker_sock = connect_to_broker(m_host, BROKER_MASTER_NODE_ID);
        if (NULL == m_master_broker_sock)
        {
            LOGERROR((FFRPC, "FFRpc::timer_reconnect_broker failed, can't connect to remote broker<%s>", m_host.c_str()));
            //! 设置定时器重连
            m_timer.onceTimer(RECONNECT_TO_BROKER_TIMEOUT, TaskBinder::gen(&route_call_reconnect, this));
        }
        else
        {
            LOGWARN((FFRPC, "FFRpc::timer_reconnect_broker failed, connect to master remote broker<%s> ok", m_host.c_str()));
        }
    }
    //!检测是否需要连接slave broker
    map<string/*host*/, int64_t>::iterator it = m_broker_data.slave_broker_data.begin();
    for (; it != m_broker_data.slave_broker_data.end(); ++it)
    {
        uint64_t node_id = it->second;
        string host = it->first;
        if (m_broker_sockets.find(node_id) == m_broker_sockets.end())//!重连
        {
            socket_ptr_t sock = connect_to_broker(host, node_id);
            if (sock == NULL)
            {
                LOGERROR((FFRPC, "FFRpc::timer_reconnect_broker failed, can't connect to remote broker<%s>", host.c_str()));
            }
            else
            {
                m_broker_sockets[node_id] = sock;
                LOGWARN((FFRPC, "FFRpc::timer_reconnect_broker failed, connect to slave remote broker<%s> ok", host.c_str()));
            }
        }
    }
    LOGINFO((FFRPC, "FFRpc::timer_reconnect_broker  end ok"));
}

//! 获取任务队列对象
TaskQueue& FFRpc::get_tq()
{
    return m_tq;
}
TaskQueueI* FFRpc::getTqPtr()
{
    return &m_tq;
}
/*
int FFRpc::handleBroken(socket_ptr_t sock_)
{
    m_tq.produce(TaskBinder::gen(&FFRpc::handleBroken_impl, this, sock_));
    return 0;
}
int FFRpc::handleMsg(const Message& msg_, socket_ptr_t sock_)
{
    m_tq.produce(TaskBinder::gen(&FFRpc::handleMsg_impl, this, msg_, sock_));
    return 0;
}
*/
int FFRpc::handleBroken(socket_ptr_t sock_)
{
    //! 设置定时器重练
    if (m_master_broker_sock == sock_)
    {
        m_master_broker_sock = NULL;
    }
    else
    {
        map<uint64_t, socket_ptr_t>::iterator it = m_broker_sockets.begin();
        for (; it != m_broker_sockets.end(); ++it)
        {
            if (it->second == sock_)
            {
                m_broker_sockets.erase(it);
                break;
            }
        }
    }
    sock_->safeDelete();

    if (true == m_runing)
    {
        m_timer.onceTimer(RECONNECT_TO_BROKER_TIMEOUT, TaskBinder::gen(&route_call_reconnect, this));
    }
    return 0;
}

int FFRpc::handleMsg(const Message& msg_, socket_ptr_t sock_)
{
    uint16_t cmd = msg_.get_cmd();
    LOGTRACE((FFRPC, "FFRpc::handleMsg_impl cmd[%u] begin", cmd));

    FFSlot::FFCallBack* cb = m_ffslot.get_callback(cmd);
    if (cb)
    {
        try
        {
            ffslot_msg_arg arg(msg_.get_body(), sock_);
            cb->exe(&arg);
            LOGTRACE((FFRPC, "FFRpc::handleMsg_impl cmd[%u] call end ok", cmd));
            return 0;
        }
        catch(exception& e_)
        {
            LOGTRACE((BROKER, "FFRpc::handleMsg_impl exception<%s> body_size=%u", e_.what(), msg_.get_body().size()));
            return -1;
        }
    }
    LOGWARN((FFRPC, "FFRpc::handleMsg_impl cmd[%u] end ok", cmd));
    return -1;
}


//! 新版 调用消息对应的回调函数
int FFRpc::handle_rpc_call_msg(BrokerRouteMsg::in_t& msg_, socket_ptr_t sock_)
{
    LOGTRACE((FFRPC, "FFRpc::handle_rpc_call_msg msg begin service_name=%s dest_msg_name=%s callback_id=%u, body_size=%d",
                     msg_.dest_service_name, msg_.dest_msg_name, msg_.callback_id, msg_.body.size()));
    
    if (false == msg_.err_info.empty())
    {
        LOGERROR((FFRPC, "FFRpc::handle_rpc_call_msg error=%s", msg_.err_info));
        return 0;
    }
    if (msg_.dest_service_name.empty() == false)
    {
        FFSlot::FFCallBack* cb = m_ffslot_interface.get_callback(msg_.dest_msg_name);
        if (NULL == cb)
        {
            vector<string> args;
            StrTool::split(msg_.dest_msg_name, args, "::");
            if (args.empty() == false)
            {
                const string& tmp_str_name = args[args.size()-1];
                map<string, FFSlot::FFCallBack*>& all_cmd = m_ffslot_interface.get_str_cmd();
                for (map<string, FFSlot::FFCallBack*>::iterator it = all_cmd.begin(); it != all_cmd.end(); ++it)
                {
                    string::size_type pos = it->first.find(tmp_str_name);
                    if (pos != string::npos && pos + tmp_str_name.size() == it->first.size())
                    {
                        cb = it->second;
                        break;
                    }
                }
            }
        }
        if (cb)
        {
            ffslot_req_arg arg(msg_.body, msg_.from_node_id, msg_.callback_id, msg_.from_namespace, msg_.err_info, this);
            cb->exe(&arg);
            return 0;
        }
        else
        {
            LOGERROR((FFRPC, "FFRpc::handle_rpc_call_msg service=%s and msg_name=%s not found", msg_.dest_service_name, msg_.dest_msg_name));
            msg_.err_info = "interface named " + msg_.dest_msg_name + " not found in rpc";
            msg_.dest_node_id = msg_.from_node_id;
            msg_.dest_service_name.clear();
            this->response(msg_.from_namespace, "", msg_.from_node_id, 0, ffthrift_t::EncodeAsString(msg_), msg_.err_info);
        }
    }
    else
    {
        FFSlot::FFCallBack* cb = m_ffslot_callback.get_callback(msg_.callback_id);
        if (cb)
        {
            ffslot_req_arg arg(msg_.body, msg_.from_node_id, 0, msg_.from_namespace, msg_.err_info, this);
            cb->exe(&arg);
            m_ffslot_callback.del(msg_.callback_id);
            return 0;
        }
        else
        {
            LOGERROR((FFRPC, "FFRpc::handle_rpc_call_msg callback_id[%u] or dest_msg=%s not found", msg_.callback_id, msg_.dest_msg_name));
        }
    }
    LOGTRACE((FFRPC, "FFRpc::handle_rpc_call_msg msg end ok"));
    return 0;
}
//!获取某一类型的service
vector<string> FFRpc::getServicesLike(const string& token)
{
    vector<string> ret;
    map<string, int64_t>::iterator it = m_broker_data.service2node_id.begin();
    for (; it != m_broker_data.service2node_id.end(); ++it){
        if (it->first.find(token) != string::npos){
            ret.push_back(it->first);
        }
    }
    return ret;
}
string FFRpc::getServicesById(uint64_t dest_node_id_)
{
    map<string, int64_t>::iterator it = m_broker_data.service2node_id.begin();
    for (; it != m_broker_data.service2node_id.end(); ++it){
        if (it->second == (int64_t)dest_node_id_){
            return it->first;
        }
    }
    return "";
}
int FFRpc::call_impl(const string& service_name_, const string& msg_name_, const string& body_, FFSlot::FFCallBack* callback_)
{
    //!调用远程消息
    LOGTRACE((FFRPC, "FFRpc::call_impl begin service_name_<%s>, msg_name_<%s> body_size=%u", service_name_.c_str(), msg_name_.c_str(), body_.size()));
    
    map<string, int64_t>::iterator it = m_broker_data.service2node_id.find(service_name_);
    if (it == m_broker_data.service2node_id.end())
    {
        LOGWARN((FFRPC, "FFRpc::call_impl end service not exist=%s", service_name_));
        if (callback_){
            try{
                ffslot_req_arg arg("", 0, 0, "", "service " + service_name_ + " not exist in ffrpc", this);
                callback_->exe(&arg);
            }
            catch(exception& e_)
            {
                LOGWARN((FFRPC, "FFRpc::call_impl end exception=%s", e_.what()));
            }
        }
        return -1;
    }
    int64_t callback_id  = int64_t(callback_);
    if (callback_)
    {
        m_ffslot_callback.bind(callback_id, callback_);
    }
    
    LOGTRACE((FFRPC, "FFRpc::call_impl end dest_id=%u callback_id=%u", it->second, callback_id));

    static string null_str;
    send_to_dest_node(null_str, service_name_, msg_name_, it->second, callback_id, body_);

    return 0;
}

//! 通过node id 发送消息给broker
void FFRpc::send_to_dest_node(const string& dest_namespace_, const string& service_name_, const string& msg_name_,
                                uint64_t dest_node_id_, int64_t callback_id_, const string& body_, string error_info)
{
    LOGTRACE((FFRPC, "FFRpc::send_to_broker_by_nodeid begin dest_node_id[%u]", dest_node_id_));
    BrokerRouteMsg::in_t dest_msg;
    dest_msg.dest_namespace = dest_namespace_;
    dest_msg.dest_service_name = service_name_;
    dest_msg.dest_msg_name = msg_name_;
    dest_msg.dest_node_id = dest_node_id_;
    dest_msg.callback_id = callback_id_;
    dest_msg.body = body_;
    dest_msg.err_info = error_info;

    dest_msg.from_node_id = m_node_id;
    
    uint64_t dest_broker_id = m_bind_broker_id;
    //!如果赋值了namespace, 那么需要转发给master BROKER
    if (false == dest_namespace_.empty())
    {
        dest_broker_id = BROKER_MASTER_NODE_ID;
    }
    FFBroker* pbroker = Singleton<FFRpcMemoryRoute>::instance().get_broker(dest_broker_id);
    if (pbroker)//!如果broker和本身都在同一个进程中,那么直接内存间投递即可
    {
        LOGTRACE((FFRPC, "FFRpc::send_to_broker_by_nodeid begin dest_node_id[%u], m_bind_broker_id=%u memory post", dest_node_id_, dest_broker_id));
        pbroker->get_tq().produce(TaskBinder::gen(&FFBroker::send_to_rpc_node, pbroker, dest_msg));
    }
    else if (dest_broker_id == 0)
    {
        msg_sender_t::send(m_master_broker_sock, BROKER_ROUTE_MSG, ffthrift_t::EncodeAsString(dest_msg));
    }
    else
    {
        msg_sender_t::send(m_broker_sockets[m_bind_broker_id], BROKER_ROUTE_MSG, ffthrift_t::EncodeAsString(dest_msg));
    }
    return;
}
//! 判断某个service是否存在
bool FFRpc::isExist(const string& service_name_)
{
    map<string, uint32_t>::iterator it = m_broker_client_name2nodeid.find(service_name_);
    if (it == m_broker_client_name2nodeid.end())
    {
        return false;
    }
    return true;
}
//! 通过bridge broker调用远程的service TODO
int FFRpc::bridge_call_impl(const string& broker_group_, const string& service_name_, const string& msg_name_,
                              const string& body_, FFSlot::FFCallBack* callback_)
{
    int64_t callback_id  = int64_t(callback_);
    m_ffslot_callback.bind(callback_id, callback_);
    send_to_dest_node(broker_group_, service_name_, msg_name_, 0, callback_id, body_);
    LOGINFO((FFRPC, "FFRpc::bridge_call_impl group<%s> service[%s] end ok", broker_group_, service_name_));
    return 0;
}


//! 调用接口后，需要回调消息给请求者
void FFRpc::response(const string& dest_namespace_, const string& msg_name_,  uint64_t dest_node_id_, int64_t callback_id_, const string& body_, string err_info)
{
    static string null_str;
    m_tq.produce(TaskBinder::gen(&FFRpc::send_to_dest_node, this, dest_namespace_, null_str, msg_name_, dest_node_id_, callback_id_, body_, err_info));
}

//! 处理注册, 
int FFRpc::handle_broker_reg_response(RegisterToBroker::out_t& msg_, socket_ptr_t sock_)
{
    LOGTRACE((FFRPC, "FFBroker::handle_broker_reg_response begin node_id=%d", msg_.node_id));
    if (msg_.register_flag < 0)
    {
        LOGERROR((FFRPC, "FFBroker::handle_broker_reg_response register failed, service exist"));
        return -1;
    }
    if (msg_.register_flag == 1)
    {
        m_node_id = msg_.node_id;//! -1表示注册失败，0表示同步消息，1表示注册成功
        Singleton<FFRpcMemoryRoute>::instance().add_node(m_node_id, this);
        LOGWARN((FFRPC, "FFBroker::handle_broker_reg_response alloc node_id=%d", m_node_id));
    }
    m_bind_broker_id = msg_.rpc_bind_broker_info[m_node_id];
    m_broker_data = msg_;

    if (m_master_broker_sock)
        timer_reconnect_broker();
    LOGTRACE((FFRPC, "FFBroker::handle_broker_reg_response end service num=%d, m_bind_broker_id=%d", m_broker_data.service2node_id.size(), m_bind_broker_id));
    return 0;
}

//!获取broker socket
socket_ptr_t FFRpc::get_broker_socket()
{
    if (m_bind_broker_id == 0)
    {
        return m_master_broker_sock;
    }
    return m_broker_sockets[m_bind_broker_id];     
}

//! 注册接口【支持动态的增加接口】
void FFRpc::reg(const string& name_, FFSlot::FFCallBack* func_)
{
	m_ffslot_interface.bind(name_, func_);
}

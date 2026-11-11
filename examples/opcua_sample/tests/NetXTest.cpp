#include "NetX.h"
#include "Memory.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <deque>
#include <vector>
#include <thread>
#include <chrono>

namespace {
struct Socket { std::deque<NX_PACKET*> received; };
std::vector<NX_TCP_SOCKET*> sockets;
NX_TCP_SOCKET* listener{};
bool blocked{};
bool finishDuringAccept{};
bool portBound{};
std::vector<unsigned char> transmitted;
unsigned packets{};
Socket& data(NX_TCP_SOCKET* socket) { return *static_cast<Socket*>(socket->test); }
}

extern "C" {
UINT tx_event_flags_create(TX_EVENT_FLAGS_GROUP* f,char*) { f->flags=0;return 0; }
UINT tx_event_flags_delete(TX_EVENT_FLAGS_GROUP*) { return 0; }
UINT tx_event_flags_set(TX_EVENT_FLAGS_GROUP* f,ULONG bits,UINT) { f->flags|=bits;return 0; }
UINT tx_event_flags_get(TX_EVENT_FLAGS_GROUP* f,ULONG bits,UINT,ULONG* value,ULONG) {
    *value=f->flags&bits;f->flags&=~bits;return 0;
}
UINT tx_interrupt_control(UINT) { return 0; }
UINT nx_tcp_socket_create(NX_IP*,NX_TCP_SOCKET* s,char*,ULONG,ULONG,UINT,ULONG,
                          void(*)(NX_TCP_SOCKET*),void(*)(NX_TCP_SOCKET*)) {
    s->test=new Socket;s->nx_tcp_socket_state=NX_TCP_CLOSED;sockets.push_back(s);return 0;
}
UINT nx_tcp_socket_delete(NX_TCP_SOCKET* s) {
    for(auto* p:data(s).received) nx_packet_release(p);
    delete static_cast<Socket*>(s->test);s->test=nullptr;
    std::erase(sockets,s);if(listener==s)listener=nullptr;return 0;
}
UINT nx_tcp_socket_receive_notify(NX_TCP_SOCKET*,void(*)(NX_TCP_SOCKET*)) { return 0; }
UINT nx_tcp_socket_establish_notify(NX_TCP_SOCKET*,void(*)(NX_TCP_SOCKET*)) { return 0; }
UINT nx_tcp_socket_window_update_notify_set(NX_TCP_SOCKET*,void(*)(NX_TCP_SOCKET*)) { return 0; }
UINT nx_tcp_server_socket_relisten(NX_IP*,UINT,NX_TCP_SOCKET* s) {
    listener=s;s->nx_tcp_socket_state=NX_TCP_LISTEN_STATE;return 0;
}
UINT nx_tcp_server_socket_listen(NX_IP* ip,UINT port,NX_TCP_SOCKET* s,UINT,void(*)(NX_TCP_SOCKET*,UINT)) {
    if(portBound)return 99;
    portBound=true;
    return nx_tcp_server_socket_relisten(ip,port,s);
}
UINT nx_tcp_server_socket_unlisten(NX_IP*,UINT) {
    if(listener && listener->nx_tcp_socket_state!=NX_TCP_LISTEN_STATE)return 99;
    listener=nullptr;portBound=false;return 0;
}
UINT nx_tcp_server_socket_accept(NX_TCP_SOCKET* s,ULONG) {
    if(finishDuringAccept) {finishDuringAccept=false;s->nx_tcp_socket_state=NX_TCP_ESTABLISHED;return 99;}
    if(s->nx_tcp_socket_state==NX_TCP_ESTABLISHED)return 0;
    if(s->nx_tcp_socket_state==NX_TCP_LISTEN_STATE)s->nx_tcp_socket_state=NX_TCP_SYN_RECEIVED;
    return 99;
}
UINT nx_tcp_server_socket_unaccept(NX_TCP_SOCKET* s) {
    if(listener==s)listener=nullptr;s->nx_tcp_socket_state=NX_TCP_CLOSED;return 0;
}
UINT nx_tcp_socket_disconnect(NX_TCP_SOCKET* s,ULONG) { s->nx_tcp_socket_state=NX_TCP_CLOSED;return 0; }
UINT nx_tcp_socket_receive(NX_TCP_SOCKET* s,NX_PACKET** p,ULONG) {
    if(data(s).received.empty())return 99;
    *p=data(s).received.front();data(s).received.pop_front();--s->nx_tcp_socket_receive_queue_count;return 0;
}
UINT nx_tcp_socket_send(NX_TCP_SOCKET*,NX_PACKET* p,ULONG) {
    if(blocked)return 99;
    auto& bytes=*static_cast<std::vector<unsigned char>*>(p->payload);
    transmitted.insert(transmitted.end(),bytes.begin(),bytes.end());nx_packet_release(p);return 0;
}
UINT nx_packet_allocate(NX_PACKET_POOL*,NX_PACKET** p,ULONG,ULONG) {
    *p=new NX_PACKET{0,new std::vector<unsigned char>};++packets;return 0;
}
UINT nx_packet_data_append(NX_PACKET* p,const void* source,ULONG count,NX_PACKET_POOL*,ULONG) {
    auto* b=static_cast<const unsigned char*>(source);
    auto& bytes=*static_cast<std::vector<unsigned char>*>(p->payload);
    bytes.insert(bytes.end(),b,b+count);p->nx_packet_length=bytes.size();return 0;
}
UINT nx_packet_data_extract_offset(NX_PACKET* p,ULONG offset,void* out,ULONG count,ULONG* copied) {
    auto& bytes=*static_cast<std::vector<unsigned char>*>(p->payload);
    if(offset+count>bytes.size())return NX_SIZE_ERROR;
    std::memcpy(out,bytes.data()+offset,count);*copied=count;return 0;
}
UINT nx_packet_release(NX_PACKET* p) {
    delete static_cast<std::vector<unsigned char>*>(p->payload);delete p;--packets;return 0;
}
}

class NetXAdapter : public testing::Test {
protected:
    alignas(std::max_align_t) std::array<std::byte,128*1024> arena{};
    NX_IP ip{}; NX_PACKET_POOL pool{}; UA_ServerConfig config{};
    UA_EventLoop* loop{}; UA_ConnectionManager* manager{};
    std::vector<uintptr_t> accepted, closed;
    std::vector<unsigned char> received;
    static void callback(UA_ConnectionManager*,uintptr_t id,void* app,void**,UA_ConnectionState state,
                         const UA_KeyValueMap*,UA_ByteString bytes) {
        auto& self=*static_cast<NetXAdapter*>(app);
        if(state==UA_CONNECTIONSTATE_CLOSING)self.closed.push_back(id);
        else if(bytes.length) self.received.insert(self.received.end(),bytes.data,bytes.data+bytes.length);
        else if(id!=1)self.accepted.push_back(id);
    }
    void SetUp() override {
        ASSERT_TRUE(sockets.empty());ASSERT_FALSE(portBound);
        blocked=false;finishDuringAccept=false;transmitted.clear();
        ASSERT_TRUE(opcua_memory_install(arena.data(),arena.size()));
        ASSERT_EQ(opcua_netx_configure(&config,&ip,&pool),UA_STATUSCODE_GOOD);
        loop=config.eventLoop;manager=reinterpret_cast<UA_ConnectionManager*>(loop->eventSources);
        ASSERT_EQ(loop->start(loop),UA_STATUSCODE_GOOD);
        UA_KeyValueMap parameters=UA_KEYVALUEMAP_NULL;
        UA_Boolean yes=true;UA_UInt16 port=4840;
        char listenName[]{"listen"}, portName[]{"port"};
        UA_KeyValueMap_setScalar(&parameters,UA_QUALIFIEDNAME(0,listenName),&yes,&UA_TYPES[UA_TYPES_BOOLEAN]);
        UA_KeyValueMap_setScalar(&parameters,UA_QUALIFIEDNAME(0,portName),&port,&UA_TYPES[UA_TYPES_UINT16]);
        ASSERT_EQ(manager->openConnection(manager,&parameters,this,nullptr,callback),UA_STATUSCODE_GOOD);
        UA_KeyValueMap_clear(&parameters);
    }
    void TearDown() override {
        blocked=false;
        loop->stop(loop);
        for(unsigned i=0;i<10 && loop->state!=UA_EVENTLOOPSTATE_STOPPED;++i)loop->run(loop,0);
        ASSERT_EQ(loop->state,UA_EVENTLOOPSTATE_STOPPED);
        ASSERT_EQ(loop->free(loop),UA_STATUSCODE_GOOD);
        EXPECT_TRUE(sockets.empty());EXPECT_EQ(packets,0);
        EXPECT_FALSE(portBound);
        EXPECT_EQ(opcua_memory_stats().used,0);
    }
    NX_TCP_SOCKET* accept() {
        auto* s=listener;s->nx_tcp_socket_state=NX_TCP_ESTABLISHED;loop->run(loop,0);return s;
    }
};

TEST_F(NetXAdapter, DisconnectBeforeAcceptDoesNotConsumeListener) {
    for(unsigned i=0;i<20;++i) {
        ASSERT_NE(listener,nullptr);
        listener->nx_tcp_socket_state=NX_TCP_CLOSE_WAIT;
        loop->run(loop,0);
        EXPECT_TRUE(accepted.empty());EXPECT_EQ(sockets.size(),1);
    }
    accept();EXPECT_EQ(accepted.size(),1);
}
TEST_F(NetXAdapter, HandshakeFinishingBetweenAcceptAndStateCheckIsNotAborted) {
    finishDuringAccept=true;
    loop->run(loop,0);
    EXPECT_TRUE(accepted.empty());ASSERT_NE(listener,nullptr);
    EXPECT_EQ(listener->nx_tcp_socket_state,NX_TCP_ESTABLISHED);
    loop->run(loop,0);EXPECT_EQ(accepted.size(),1);
}
TEST_F(NetXAdapter, ShutdownUnacceptsIdleSocketBeforeRemovingListenPort) {
    loop->run(loop,0);
    ASSERT_NE(listener,nullptr);
    EXPECT_EQ(listener->nx_tcp_socket_state,NX_TCP_SYN_RECEIVED);
    loop->stop(loop);loop->run(loop,0);
    EXPECT_FALSE(portBound);
}
TEST_F(NetXAdapter, PendingSendsRetainOwnershipAndRejectOverflow) {
    accept();blocked=true;
    for(unsigned i=0;i<3;++i) {
        UA_ByteString b=UA_BYTESTRING_NULL;
        ASSERT_EQ(manager->allocNetworkBuffer(manager,accepted[0],&b,4096),UA_STATUSCODE_GOOD);
        std::memset(b.data,0x41+i,b.length);
        auto status=manager->sendWithConnection(manager,accepted[0],&UA_KEYVALUEMAP_NULL,&b);
        EXPECT_EQ(status,i<2?UA_STATUSCODE_GOOD:UA_STATUSCODE_BADRESOURCEUNAVAILABLE);
        EXPECT_EQ(b.data,nullptr);loop->run(loop,0);
    }
    EXPECT_TRUE(transmitted.empty());blocked=false;loop->run(loop,0);
    ASSERT_EQ(transmitted.size(),8192);
    EXPECT_TRUE(std::all_of(transmitted.begin(),transmitted.begin()+4096,[](auto b){return b==0x41;}));
    EXPECT_TRUE(std::all_of(transmitted.begin()+4096,transmitted.end(),[](auto b){return b==0x42;}));
    ASSERT_EQ(closed.size(),1);
    EXPECT_EQ(closed[0],accepted[0]);
}
TEST_F(NetXAdapter, OverflowClosesEvenWhenPeerNeverConsumesPendingOutput) {
    accept();blocked=true;
    for(unsigned i=0;i<3;++i) {
        UA_ByteString b=UA_BYTESTRING_NULL;
        ASSERT_EQ(manager->allocNetworkBuffer(manager,accepted[0],&b,64),UA_STATUSCODE_GOOD);
        EXPECT_EQ(manager->sendWithConnection(manager,accepted[0],&UA_KEYVALUEMAP_NULL,&b),
                  i<2?UA_STATUSCODE_GOOD:UA_STATUSCODE_BADRESOURCEUNAVAILABLE);
    }
    loop->run(loop,0);
    EXPECT_TRUE(closed.empty());
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    loop->run(loop,0);
    ASSERT_EQ(closed.size(),1);
    EXPECT_EQ(closed[0],accepted[0]);
    EXPECT_TRUE(transmitted.empty());
    EXPECT_EQ(packets,0);
}
TEST_F(NetXAdapter, HalfClosedPeerDeliversFinalDataThenClosesExactlyOnce) {
    auto* s=accept();NX_PACKET* p{};nx_packet_allocate(&pool,&p,0,0);
    nx_packet_data_append(p,"test",4,&pool,0);data(s).received.push_back(p);
    s->nx_tcp_socket_receive_queue_count=1;s->nx_tcp_socket_state=NX_TCP_CLOSE_WAIT;
    loop->run(loop,0);
    EXPECT_EQ(received,(std::vector<unsigned char>{'t','e','s','t'}));
    EXPECT_EQ(closed.size(),1);loop->run(loop,0);EXPECT_EQ(closed.size(),1);
}
TEST_F(NetXAdapter, DeferredCleanupAddedAfterStopIsFreed) {
    loop->stop(loop);loop->run(loop,0);
    ASSERT_EQ(loop->state,UA_EVENTLOOPSTATE_STOPPED);
    auto* cleanup=static_cast<UA_DelayedCallback*>(UA_calloc(1,sizeof(UA_DelayedCallback)));
    ASSERT_NE(cleanup,nullptr);
    cleanup->context=cleanup;cleanup->callback=[](void*,void* context){UA_free(context);};
    loop->addDelayedCallback(loop,cleanup);
    // TearDown re-enters stop; free must also work directly in the stopped state.
    ASSERT_EQ(loop->free(loop),UA_STATUSCODE_GOOD);
    EXPECT_EQ(opcua_memory_stats().used,0);
    config={};
    ASSERT_EQ(opcua_netx_configure(&config,&ip,&pool),UA_STATUSCODE_GOOD);
    loop=config.eventLoop;manager=reinterpret_cast<UA_ConnectionManager*>(loop->eventSources);
    loop->start(loop);
}

TEST_F(NetXAdapter, DeferredCallbackCanCancelLaterCallbackInSameBatch) {
    struct State {
        UA_EventLoop* loop;
        UA_DelayedCallback first{}, second{};
        unsigned calls{};
    } state{loop};
    state.first.context=&state;
    state.first.callback=[](void*,void* context) {
        auto& s=*static_cast<State*>(context);
        ++s.calls;s.loop->removeDelayedCallback(s.loop,&s.second);
    };
    state.second.context=&state;
    state.second.callback=[](void*,void* context){++static_cast<State*>(context)->calls;};
    loop->addDelayedCallback(loop,&state.first);loop->addDelayedCallback(loop,&state.second);
    loop->run(loop,0);EXPECT_EQ(state.calls,1);
    loop->run(loop,0);EXPECT_EQ(state.calls,1);
}

TEST(NetXConstruction, AllocationFailureLeavesNoLoopOrResources) {
    alignas(std::max_align_t) std::array<std::byte,512> arena{};
    ASSERT_TRUE(opcua_memory_install(arena.data(),arena.size()));
    UA_ServerConfig config{};NX_IP ip{};NX_PACKET_POOL pool{};
    EXPECT_EQ(opcua_netx_configure(&config,&ip,&pool),UA_STATUSCODE_BADOUTOFMEMORY);
    EXPECT_EQ(config.eventLoop,nullptr);
    EXPECT_EQ(opcua_memory_stats().used,0);
    EXPECT_TRUE(sockets.empty());EXPECT_FALSE(portBound);
}

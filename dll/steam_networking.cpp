/* Copyright (C) 2019 Mr Goldberg
   This file is part of the Goldberg Emulator

   The Goldberg Emulator is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 3 of the License, or (at your option) any later version.

   The Goldberg Emulator is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with the Goldberg Emulator; if not, see
   <http://www.gnu.org/licenses/>.  */

#include "dll/steam_networking.h"
#include "dll/dll.h"



#define OLD_CHANNEL_NUMBER 1


SNetSocket_t Steam_Networking::create_connection_socket(CSteamID target, int nVirtualPort, uint32 nIP, uint16 nPort, SNetListenSocket_t id, enum steam_socket_connection_status status, SNetSocket_t other_id)
{
    static SNetSocket_t socket_number = 0;
    bool found = false;
    do {
        found = false;
        ++socket_number;
        for (auto & c: connection_sockets) {
            if (c.id == socket_number || socket_number == 0) {
                found = true;
                break;
            }
        }
    } while (found);

    struct steam_connection_socket socket{};
    socket.id = socket_number;
    socket.listen_id = id;
    socket.status = status;
    socket.target = target;
    socket.nVirtualPort = nVirtualPort;
    socket.nIP = nIP;
    socket.nPort = nPort;
    socket.other_id = other_id;
    connection_sockets.push_back(socket);

    Common_Message msg{};
    msg.set_source_id(settings->get_local_steam_id().ConvertToUint64());
    msg.set_dest_id(target.ConvertToUint64());
    msg.set_allocated_network_old(new Network_Old);
    if (nPort) {
        msg.mutable_network_old()->set_type(Network_Old::CONNECTION_REQUEST_IP);
        msg.mutable_network_old()->set_port(nPort);
    } else {
        msg.mutable_network_old()->set_type(Network_Old::CONNECTION_REQUEST_STEAMID);
        msg.mutable_network_old()->set_port(nVirtualPort);
    }

    if (socket.status == SOCKET_CONNECTED) {
        msg.mutable_network_old()->set_type(Network_Old::CONNECTION_ACCEPTED);
    }

    msg.mutable_network_old()->set_connection_id(socket.other_id);
    msg.mutable_network_old()->set_connection_id_from(socket.id);

    if (target.IsValid()) {
        network->sendTo(&msg, true);
    } else if (nIP) {
        network->sendToIPPort(&msg, nIP, nPort, true);
    }

    return socket.id;
}

struct steam_connection_socket* Steam_Networking::get_connection_socket(SNetSocket_t id)
{
    auto conn = std::find_if(connection_sockets.begin(), connection_sockets.end(), [&id](struct steam_connection_socket const& conn) {
        return conn.id == id;
    });
    if (connection_sockets.end() == conn) return NULL;

    return &(*conn);
}

void Steam_Networking::remove_killed_connection_sockets()
{
    auto socket = std::begin(connection_sockets);
    while (socket != std::end(connection_sockets)) {
        if (socket->status == SOCKET_KILLED || socket->status == SOCKET_DISCONNECTED) {
            socket = connection_sockets.erase(socket);
        } else {
            ++socket;
        }
    }
}

void Steam_Networking::steam_networking_callback(void *object, Common_Message *msg)
{
    // PRINT_DEBUG_ENTRY();

    Steam_Networking *steam_networking = (Steam_Networking *)object;
    steam_networking->Callback(msg);
}

void Steam_Networking::steam_run_every_runcb(void *object)
{
    // PRINT_DEBUG_ENTRY();

    Steam_Networking *steam_networking = (Steam_Networking *)object;
    steam_networking->RunCallbacks();
}

Steam_Networking::Steam_Networking(class Settings *settings, class Networking *network, class P2p_Manager *p2p_manager, class SteamCallBacks *callbacks, class RunEveryRunCB *run_every_runcb)
{
    this->settings = settings;
    this->network = network;
    this->p2p_manager = p2p_manager;
    this->callbacks = callbacks;
    this->run_every_runcb = run_every_runcb;

    this->network->setCallback(CALLBACK_ID_NETWORKING, settings->get_local_steam_id(), &Steam_Networking::steam_networking_callback, this);
    this->network->setCallback(CALLBACK_ID_USER_STATUS, settings->get_local_steam_id(), &Steam_Networking::steam_networking_callback, this);
    this->run_every_runcb->add(&Steam_Networking::steam_run_every_runcb, this);

    PRINT_DEBUG("user id %llu", settings->get_local_steam_id().ConvertToUint64());
}

Steam_Networking::~Steam_Networking()
{
    this->network->rmCallback(CALLBACK_ID_NETWORKING, settings->get_local_steam_id(), &Steam_Networking::steam_networking_callback, this);
    this->network->rmCallback(CALLBACK_ID_USER_STATUS, settings->get_local_steam_id(), &Steam_Networking::steam_networking_callback, this);
    this->run_every_runcb->remove(&Steam_Networking::steam_run_every_runcb, this);
}

////////////////////////////////////////////////////////////////////////////////////////////
// Session-less connection functions
//    automatically establishes NAT-traversing or Relay server connections

// Sends a P2P packet to the specified user
// UDP-like, unreliable and a max packet size of 1200 bytes
// the first packet send may be delayed as the NAT-traversal code runs
// if we can't get through to the user, an error will be posted via the callback P2PSessionConnectFail_t
// see EP2PSend enum above for the descriptions of the different ways of sending packets
//
// nChannel is a routing number you can use to help route message to different systems 	- you'll have to call ReadP2PPacket() 
// with the same channel number in order to retrieve the data on the other end
// using different channels to talk to the same user will still use the same underlying p2p connection, saving on resources
bool Steam_Networking::SendP2PPacket( CSteamID steamIDRemote, const void *pubData, uint32 cubData, EP2PSend eP2PSendType, int nChannel)
{
    PRINT_DEBUG(
        "size=[%u] sendtype: <%u> channel: [%u] from=[%llu] to=[%llu]",
        cubData, eP2PSendType, nChannel, settings->get_local_steam_id().ConvertToUint64(), steamIDRemote.ConvertToUint64()
    );

    return p2p_manager->send_packet(
        settings->get_local_steam_id(), steamIDRemote,
        pubData, cubData, eP2PSendType, nChannel
    );
}

bool Steam_Networking::SendP2PPacket( CSteamID steamIDRemote, const void *pubData, uint32 cubData, EP2PSend eP2PSendType )
{
    PRINT_DEBUG("old");
    return SendP2PPacket(steamIDRemote, pubData, cubData, eP2PSendType, OLD_CHANNEL_NUMBER);
}

// returns true if any data is available for read, and the amount of data that will need to be read
bool Steam_Networking::IsP2PPacketAvailable( uint32 *pcubMsgSize, int nChannel)
{
    PRINT_DEBUG(
        "channel=[%i], my steam id=%llu (is server=%u)",
        nChannel, settings->get_local_steam_id().ConvertToUint64(), settings->get_local_steam_id().BGameServerAccount()
    );
    
    //Not sure if this should be here because it slightly screws up games that don't like such low "pings"
    //Commenting it out for now because it looks like it causes a bug where 20xx gets stuck in an infinite receive packet loop
    //this->network->Run();
    //RunCallbacks();

    return p2p_manager->is_packet_available(
        settings->get_local_steam_id(),
        pcubMsgSize, nChannel
    );
}

bool Steam_Networking::IsP2PPacketAvailable( uint32 *pcubMsgSize)
{
    PRINT_DEBUG("old");
    return IsP2PPacketAvailable(pcubMsgSize, OLD_CHANNEL_NUMBER);
}

// reads in a packet that has been sent from another user via SendP2PPacket()
// returns the size of the message and the steamID of the user who sent it in the last two parameters
// if the buffer passed in is too small, the message will be truncated
// this call is not blocking, and will return false if no data is available
bool Steam_Networking::ReadP2PPacket( void *pubDest, uint32 cubDest, uint32 *pcubMsgSize, CSteamID *psteamIDRemote, int nChannel)
{
    PRINT_DEBUG("%u %i %p", cubDest, nChannel, pubDest);

    //Not sure if this should be here because it slightly screws up games that don't like such low "pings"
    //Commenting it out for now because it looks like it causes a bug where 20xx gets stuck in an infinite receive packet loop
    //this->network->Run();
    //RunCallbacks();

    return p2p_manager->read_packet(
        settings->get_local_steam_id(),
        pubDest, cubDest, pcubMsgSize, psteamIDRemote, nChannel
    );
}

bool Steam_Networking::ReadP2PPacket( void *pubDest, uint32 cubDest, uint32 *pcubMsgSize, CSteamID *psteamIDRemote)
{
    PRINT_DEBUG("old");
    return ReadP2PPacket(pubDest, cubDest, pcubMsgSize, psteamIDRemote, OLD_CHANNEL_NUMBER);
}

// AcceptP2PSessionWithUser() should only be called in response to a P2PSessionRequest_t callback
// P2PSessionRequest_t will be posted if another user tries to send you a packet that you haven't talked to yet
// if you don't want to talk to the user, just ignore the request
// if the user continues to send you packets, another P2PSessionRequest_t will be posted periodically
// this may be called multiple times for a single user
// (if you've called SendP2PPacket() on the other user, this implicitly accepts the session request)
bool Steam_Networking::AcceptP2PSessionWithUser( CSteamID steamIDRemote )
{
    PRINT_DEBUG("from [%llu], I am=[%llu]", steamIDRemote.ConvertToUint64(), settings->get_local_steam_id().ConvertToUint64());

    if (p2p_manager->accept_session(settings->get_local_steam_id().ConvertToUint64(), steamIDRemote)) {
        PRINT_DEBUG("  connection accepted");
        return true;
    }

    PRINT_DEBUG("  [X] connection NOT accepted");
    return false;
}


// call CloseP2PSessionWithUser() when you're done talking to a user, will free up resources under-the-hood
// if the remote user tries to send data to you again, another P2PSessionRequest_t callback will be posted
bool Steam_Networking::CloseP2PSessionWithUser( CSteamID steamIDRemote )
{
    PRINT_DEBUG("%llu", steamIDRemote.ConvertToUint64());

    return p2p_manager->close_session(settings->get_local_steam_id().ConvertToUint64(), steamIDRemote);
}


// call CloseP2PChannelWithUser() when you're done talking to a user on a specific channel. Once all channels
// open channels to a user have been closed, the open session to the user will be closed and new data from this
// user will trigger a P2PSessionRequest_t callback
bool Steam_Networking::CloseP2PChannelWithUser( CSteamID steamIDRemote, int nChannel )
{
    PRINT_DEBUG("%llu", steamIDRemote.ConvertToUint64());

    return p2p_manager->close_channel(settings->get_local_steam_id().ConvertToUint64(), steamIDRemote, nChannel);
}


// fills out P2PSessionState_t structure with details about the underlying connection to the user
// should only needed for debugging purposes
// returns false if no connection exists to the specified user
bool Steam_Networking::GetP2PSessionState( CSteamID steamIDRemote, P2PSessionState_t *pConnectionState )
{
    PRINT_DEBUG("%llu", steamIDRemote.ConvertToUint64());
    
    return p2p_manager->get_session_state(settings->get_local_steam_id().ConvertToUint64(), steamIDRemote, pConnectionState);
}


// Allow P2P connections to fall back to being relayed through the Steam servers if a direct connection
// or NAT-traversal cannot be established. Only applies to connections created after setting this value,
// or to existing connections that need to automatically reconnect after this value is set.
//
// P2P packet relay is allowed by default
bool Steam_Networking::AllowP2PPacketRelay( bool bAllow )
{
    PRINT_DEBUG("%u", bAllow);
    return true;
}



////////////////////////////////////////////////////////////////////////////////////////////
// LISTEN / CONNECT style interface functions
//
// This is an older set of functions designed around the Berkeley TCP sockets model
// it's preferential that you use the above P2P functions, they're more robust
// and these older functions will be removed eventually
//
////////////////////////////////////////////////////////////////////////////////////////////

SNetListenSocket_t socket_number = 0;
// creates a socket and listens others to connect
// will trigger a SocketStatusCallback_t callback on another client connecting
// nVirtualP2PPort is the unique ID that the client will connect to, in case you have multiple ports
//		this can usually just be 0 unless you want multiple sets of connections
// unIP is the local IP address to bind to
//		pass in 0 if you just want the default local IP
// unPort is the port to use
//		pass in 0 if you don't want users to be able to connect via IP/Port, but expect to be always peer-to-peer connections only
SNetListenSocket_t Steam_Networking::CreateListenSocket( int nVirtualP2PPort, uint32 nIP, uint16 nPort, bool bAllowUseOfPacketRelay )
{
    PRINT_DEBUG("old %i %u %hu %u", nVirtualP2PPort, nIP, nPort, bAllowUseOfPacketRelay);
    std::lock_guard<std::recursive_mutex> lock(global_mutex);
    for (auto & c : listen_sockets) {
        if (c.nVirtualP2PPort == nVirtualP2PPort || c.nPort == nPort)
            return 0;
    }

    ++socket_number;
    if (!socket_number) ++socket_number;

    struct steam_listen_socket socket;
    socket.id = socket_number;
    socket.nVirtualP2PPort = nVirtualP2PPort;
    socket.nIP = nIP;
    socket.nPort = nPort;
    listen_sockets.push_back(socket);
    return socket.id;
}

SNetListenSocket_t Steam_Networking::CreateListenSocket( int nVirtualP2PPort, SteamIPAddress_t nIP, uint16 nPort, bool bAllowUseOfPacketRelay )
{
    PRINT_DEBUG("%i %i %u %hu %u", nVirtualP2PPort, nIP.m_eType, nIP.m_unIPv4, nPort, bAllowUseOfPacketRelay);
    //TODO: ipv6
    return CreateListenSocket(nVirtualP2PPort, nIP.m_unIPv4, nPort, bAllowUseOfPacketRelay);
}

SNetListenSocket_t Steam_Networking::CreateListenSocket( int nVirtualP2PPort, uint32 nIP, uint16 nPort )
{
    PRINT_DEBUG("old");
    return CreateListenSocket(nVirtualP2PPort, nIP, nPort, true);
}

// creates a socket and begin connection to a remote destination
// can connect via a known steamID (client or game server), or directly to an IP
// on success will trigger a SocketStatusCallback_t callback
// on failure or timeout will trigger a SocketStatusCallback_t callback with a failure code in m_eSNetSocketState
SNetSocket_t Steam_Networking::CreateP2PConnectionSocket( CSteamID steamIDTarget, int nVirtualPort, int nTimeoutSec, bool bAllowUseOfPacketRelay )
{
    PRINT_DEBUG("%llu %i %i %u", steamIDTarget.ConvertToUint64(), nVirtualPort, nTimeoutSec, bAllowUseOfPacketRelay);
    std::lock_guard<std::recursive_mutex> lock(global_mutex);
    //TODO: nTimeoutSec
    return create_connection_socket(steamIDTarget, nVirtualPort, 0, 0);
}

SNetSocket_t Steam_Networking::CreateP2PConnectionSocket( CSteamID steamIDTarget, int nVirtualPort, int nTimeoutSec )
{
    PRINT_DEBUG("old");
    return CreateP2PConnectionSocket(steamIDTarget, nVirtualPort, nTimeoutSec, true);
}

SNetSocket_t Steam_Networking::CreateConnectionSocket( uint32 nIP, uint16 nPort, int nTimeoutSec )
{
    PRINT_DEBUG("%u %hu %i", nIP, nPort, nTimeoutSec);
    std::lock_guard<std::recursive_mutex> lock(global_mutex);
    //TODO: nTimeoutSec
    return create_connection_socket((uint64)0, 0, nIP, nPort);
}

SNetSocket_t Steam_Networking::CreateConnectionSocket( SteamIPAddress_t nIP, uint16 nPort, int nTimeoutSec )
{
    PRINT_DEBUG("%i %u %hu %i", nIP.m_eType, nIP.m_unIPv4, nPort, nTimeoutSec);
    //TODO: ipv6
    return CreateConnectionSocket(nIP.m_unIPv4, nPort, nTimeoutSec);
}

// disconnects the connection to the socket, if any, and invalidates the handle
// any unread data on the socket will be thrown away
// if bNotifyRemoteEnd is set, socket will not be completely destroyed until the remote end acknowledges the disconnect
bool Steam_Networking::DestroySocket( SNetSocket_t hSocket, bool bNotifyRemoteEnd )
{
    PRINT_DEBUG_ENTRY();
    std::lock_guard<std::recursive_mutex> lock(global_mutex);
    struct steam_connection_socket *socket = get_connection_socket(hSocket);
    if (!socket || socket->status == SOCKET_KILLED) return false;
    socket->status = SOCKET_KILLED;
    return true;
}

// destroying a listen socket will automatically kill all the regular sockets generated from it
bool Steam_Networking::DestroyListenSocket( SNetListenSocket_t hSocket, bool bNotifyRemoteEnd )
{
    PRINT_DEBUG_ENTRY();
    std::lock_guard<std::recursive_mutex> lock(global_mutex);
    auto c = std::begin(listen_sockets);
    while (c != std::end(listen_sockets)) {
        if (c->id == hSocket) {
            c = listen_sockets.erase(c);
            for (auto & socket : connection_sockets) {
                if (socket.listen_id == hSocket) {
                    socket.status = SOCKET_KILLED;
                }
            }
            return true;
        } else {
            ++c;
        }
    }

    return false;
}


// sending data
// must be a handle to a connected socket
// data is all sent via UDP, and thus send sizes are limited to 1200 bytes; after this, many routers will start dropping packets
// use the reliable flag with caution; although the resend rate is pretty aggressive,
// it can still cause stalls in receiving data (like TCP)
bool Steam_Networking::SendDataOnSocket( SNetSocket_t hSocket, void *pubData, uint32 cubData, bool bReliable )
{
    PRINT_DEBUG_ENTRY();
    std::lock_guard<std::recursive_mutex> lock(global_mutex);
    struct steam_connection_socket *socket = get_connection_socket(hSocket);
    if (!socket || socket->status != SOCKET_CONNECTED) return false;

    Common_Message msg;
    msg.set_source_id(settings->get_local_steam_id().ConvertToUint64());
    msg.set_dest_id(socket->target.ConvertToUint64());
    msg.set_allocated_network_old(new Network_Old);
    msg.mutable_network_old()->set_type(Network_Old::DATA);
    msg.mutable_network_old()->set_connection_id(socket->other_id);
    msg.mutable_network_old()->set_data(pubData, cubData);
    return network->sendTo(&msg, bReliable);
}


// receiving data
// returns false if there is no data remaining
// fills out *pcubMsgSize with the size of the next message, in bytes
bool Steam_Networking::IsDataAvailableOnSocket( SNetSocket_t hSocket, uint32 *pcubMsgSize )
{
    PRINT_DEBUG_ENTRY();
    std::lock_guard<std::recursive_mutex> lock(global_mutex);
    struct steam_connection_socket *socket = get_connection_socket(hSocket);
    if (!socket) {
        if (pcubMsgSize) *pcubMsgSize = 0;
        return false;
    }

    if (socket->data_packets.size() == 0) return false;
    if (pcubMsgSize) *pcubMsgSize = static_cast<uint32>(socket->data_packets[0].data().size());
    return true;
}


// fills in pubDest with the contents of the message
// messages are always complete, of the same size as was sent (i.e. packetized, not streaming)
// if *pcubMsgSize < cubDest, only partial data is written
// returns false if no data is available
bool Steam_Networking::RetrieveDataFromSocket( SNetSocket_t hSocket, void *pubDest, uint32 cubDest, uint32 *pcubMsgSize )
{
    PRINT_DEBUG_ENTRY();
    std::lock_guard<std::recursive_mutex> lock(global_mutex);
    struct steam_connection_socket *socket = get_connection_socket(hSocket);
    if (!socket || socket->data_packets.size() == 0) return false;

    auto msg = std::begin(socket->data_packets);
    if (msg != std::end(socket->data_packets)) {
        uint32 msg_size = static_cast<uint32>(msg->data().size());
        if (msg_size > cubDest) msg_size = cubDest;
        if (pcubMsgSize) *pcubMsgSize = msg_size;
        memcpy(pubDest, msg->data().data(), msg_size);
        msg = socket->data_packets.erase(msg);
        return true;
    }

    return false;
}


// checks for data from any socket that has been connected off this listen socket
// returns false if there is no data remaining
// fills out *pcubMsgSize with the size of the next message, in bytes
// fills out *phSocket with the socket that data is available on
bool Steam_Networking::IsDataAvailable( SNetListenSocket_t hListenSocket, uint32 *pcubMsgSize, SNetSocket_t *phSocket )
{
    PRINT_DEBUG_ENTRY();
    std::lock_guard<std::recursive_mutex> lock(global_mutex);
    if (!hListenSocket) return false;

    for (auto & socket : connection_sockets) {
        if (socket.listen_id == hListenSocket && socket.data_packets.size()) {
            if (pcubMsgSize) *pcubMsgSize = static_cast<uint32>(socket.data_packets[0].data().size());
            if (phSocket) *phSocket = socket.id;
            return true;
        }
    }

    return false;
}


// retrieves data from any socket that has been connected off this listen socket
// fills in pubDest with the contents of the message
// messages are always complete, of the same size as was sent (i.e. packetized, not streaming)
// if *pcubMsgSize < cubDest, only partial data is written
// returns false if no data is available
// fills out *phSocket with the socket that data is available on
bool Steam_Networking::RetrieveData( SNetListenSocket_t hListenSocket, void *pubDest, uint32 cubDest, uint32 *pcubMsgSize, SNetSocket_t *phSocket )
{
    PRINT_DEBUG_ENTRY();
    std::lock_guard<std::recursive_mutex> lock(global_mutex);
    if (!hListenSocket) return false;

    for (auto & socket : connection_sockets) {
        if (socket.listen_id == hListenSocket && socket.data_packets.size()) {
            auto msg = std::begin(socket.data_packets);
            if (msg != std::end(socket.data_packets)) {
                uint32 msg_size = static_cast<uint32>(msg->data().size());
                if (msg_size > cubDest) msg_size = cubDest;
                if (pcubMsgSize) *pcubMsgSize = msg_size;
                if (phSocket) *phSocket = socket.id;
                memcpy(pubDest, msg->data().data(), msg_size);
                msg = socket.data_packets.erase(msg);
                return true;
            }
        }
    }

    return false;
}


// returns information about the specified socket, filling out the contents of the pointers
bool Steam_Networking::GetSocketInfo( SNetSocket_t hSocket, CSteamID *pSteamIDRemote, int *peSocketStatus, uint32 *punIPRemote, uint16 *punPortRemote )
{
    PRINT_DEBUG_ENTRY();
    std::lock_guard<std::recursive_mutex> lock(global_mutex);
    struct steam_connection_socket *socket = get_connection_socket(hSocket);
    if (!socket) return false;
    if (pSteamIDRemote) *pSteamIDRemote = socket->target;
    if (peSocketStatus) {
        //TODO: I'm not sure what peSocketStatus is supposed to be but I'm guessing it's ESNetSocketState
        if (socket->status == SOCKET_CONNECTED) {
            *peSocketStatus = k_ESNetSocketStateConnected;
        } else if (socket->status == SOCKET_CONNECTING) {
            *peSocketStatus = k_ESNetSocketStateInitiated;
        } else if (socket->status == SOCKET_DISCONNECTED) {
            *peSocketStatus = k_ESNetSocketStateDisconnecting;
        } else if (socket->status == SOCKET_KILLED) {
            *peSocketStatus = k_ESNetSocketStateConnectionBroken;
        } else {
            *peSocketStatus = k_ESNetSocketStateInvalid;
        }
    }

    if (punIPRemote) *punIPRemote = socket->nIP;
    if (punPortRemote) *punPortRemote = socket->nPort;
    return true;
}

bool Steam_Networking::GetSocketInfo( SNetSocket_t hSocket, CSteamID *pSteamIDRemote, int *peSocketStatus, SteamIPAddress_t *punIPRemote, uint16 *punPortRemote )
{
    PRINT_DEBUG_ENTRY();
    //TODO: ipv6
    uint32 *ip_remote = NULL;
    if (punIPRemote) {
        ip_remote = &(punIPRemote->m_unIPv4);
    }

    bool ret = GetSocketInfo(hSocket, pSteamIDRemote, peSocketStatus, ip_remote, punPortRemote );
    if (punIPRemote && ret) {
        punIPRemote->m_eType = k_ESteamIPTypeIPv4;
    }

    return ret;
}

// returns which local port the listen socket is bound to
// *pnIP and *pnPort will be 0 if the socket is set to listen for P2P connections only
bool Steam_Networking::GetListenSocketInfo( SNetListenSocket_t hListenSocket, uint32 *pnIP, uint16 *pnPort )
{
    PRINT_DEBUG_ENTRY();
    std::lock_guard<std::recursive_mutex> lock(global_mutex);
    auto conn = std::find_if(listen_sockets.begin(), listen_sockets.end(), [&hListenSocket](struct steam_listen_socket const& conn) { return conn.id == hListenSocket;});
    if (conn == listen_sockets.end()) return false;
    if (pnIP) *pnIP = conn->nIP;
    if (pnPort) *pnPort = conn->nPort;
    return true;
}

bool Steam_Networking::GetListenSocketInfo( SNetListenSocket_t hListenSocket, SteamIPAddress_t *pnIP, uint16 *pnPort )
{
    PRINT_DEBUG_ENTRY();
    //TODO: ipv6
    uint32 *ip = NULL;
    if (pnIP) {
        ip = &(pnIP->m_unIPv4);
    }

    bool ret = GetListenSocketInfo(hListenSocket, ip, pnPort );
    if (pnIP && ret) {
        pnIP->m_eType = k_ESteamIPTypeIPv4;
    }

    return ret;
}

// returns true to describe how the socket ended up connecting
ESNetSocketConnectionType Steam_Networking::GetSocketConnectionType( SNetSocket_t hSocket )
{
    PRINT_DEBUG_ENTRY();
    std::lock_guard<std::recursive_mutex> lock(global_mutex);
    struct steam_connection_socket *socket = get_connection_socket(hSocket);
    if (!socket || socket->status != SOCKET_CONNECTED) return k_ESNetSocketConnectionTypeNotConnected;
    else return k_ESNetSocketConnectionTypeUDP;
}


// max packet size, in bytes
int Steam_Networking::GetMaxPacketSize( SNetSocket_t hSocket )
{
    PRINT_DEBUG_ENTRY();
    return 1500;
}



void Steam_Networking::RunCallbacks()
{
    //TODO: not sure if sockets should be wiped right away
    remove_killed_connection_sockets();
}

void Steam_Networking::Callback(Common_Message *msg)
{
    if (msg->has_network_old()) {
        PRINT_DEBUG("got old network socket msg %u", msg->network_old().type());
        if (msg->network_old().type() == Network_Old::CONNECTION_REQUEST_IP) {
            for (auto & listen : listen_sockets) {
                if (listen.nPort == msg->network_old().port()) {
                    SNetSocket_t new_sock = create_connection_socket((uint64)msg->source_id(), 0, 0, msg->network_old().port(), listen.id, SOCKET_CONNECTED, static_cast<SNetSocket_t>(msg->network_old().connection_id_from()));
                    if (new_sock) {
                        struct SocketStatusCallback_t data;
                        data.m_hSocket = new_sock;
                        data.m_hListenSocket = listen.id;
                        data.m_steamIDRemote = (uint64)msg->source_id();
                        data.m_eSNetSocketState = k_ESNetSocketStateConnected; //TODO is this the right state?
                        callbacks->addCBResult(data.k_iCallback, &data, sizeof(data));
                    }
                }
            }
        } else if (msg->network_old().type() == Network_Old::CONNECTION_REQUEST_STEAMID) {
            for (auto & listen : listen_sockets) {
                if (listen.nVirtualP2PPort == msg->network_old().port()) {
                    SNetSocket_t new_sock = create_connection_socket((uint64)msg->source_id(), msg->network_old().port(), 0, 0, listen.id, SOCKET_CONNECTED, static_cast<SNetSocket_t>(msg->network_old().connection_id_from()));
                    if (new_sock) {
                        struct SocketStatusCallback_t data;
                        data.m_hSocket = new_sock;
                        data.m_hListenSocket = listen.id;
                        data.m_steamIDRemote = (uint64)msg->source_id();
                        data.m_eSNetSocketState = k_ESNetSocketStateConnected; //TODO is this the right state?
                        callbacks->addCBResult(data.k_iCallback, &data, sizeof(data));
                    }
                }
            }

        } else if (msg->network_old().type() == Network_Old::CONNECTION_ACCEPTED) {
            struct steam_connection_socket *socket = get_connection_socket(static_cast<SNetSocket_t>(msg->network_old().connection_id()));
            if (socket && socket->nPort && socket->status == SOCKET_CONNECTING && !socket->target.IsValid()) {
                socket->target = (uint64)msg->source_id();
            }

            if (socket && socket->status == SOCKET_CONNECTING && msg->source_id() == socket->target.ConvertToUint64()) {
                socket->status = SOCKET_CONNECTED;
                socket->other_id = static_cast<SNetSocket_t>(msg->network_old().connection_id_from());
                struct SocketStatusCallback_t data;
                data.m_hSocket = socket->id;
                data.m_hListenSocket = socket->listen_id;
                data.m_steamIDRemote = socket->target;
                data.m_eSNetSocketState = k_ESNetSocketStateConnected; //TODO is this the right state?
                callbacks->addCBResult(data.k_iCallback, &data, sizeof(data));
            }
        } else if (msg->network_old().type() == Network_Old::CONNECTION_END) {
            struct steam_connection_socket *socket = get_connection_socket(static_cast<SNetSocket_t>(msg->network_old().connection_id()));
            if (socket && socket->status == SOCKET_CONNECTED && msg->source_id() == socket->target.ConvertToUint64()) {
                struct SocketStatusCallback_t data;
                socket->status = SOCKET_DISCONNECTED;
                data.m_hSocket = socket->id;
                data.m_hListenSocket = socket->listen_id;
                data.m_steamIDRemote = socket->target;
                data.m_eSNetSocketState = k_ESNetSocketStateRemoteEndDisconnected; //TODO is this the right state?
                callbacks->addCBResult(data.k_iCallback, &data, sizeof(data));
            }
        } else if (msg->network_old().type() == Network_Old::DATA) {
            struct steam_connection_socket *socket = get_connection_socket(static_cast<SNetSocket_t>(msg->network_old().connection_id()));
            if (socket && socket->status == SOCKET_CONNECTED && msg->source_id() == socket->target.ConvertToUint64()) {
                socket->data_packets.push_back(msg->network_old());
            }
        }
    }

    if (msg->has_low_level()) {
        if (msg->low_level().type() == Low_Level::DISCONNECT) {
            for (auto & socket : connection_sockets) {
                if (socket.target.ConvertToUint64() == msg->source_id()) {
                    struct SocketStatusCallback_t data{};
                    socket.status = SOCKET_DISCONNECTED;
                    data.m_hSocket = socket.id;
                    data.m_hListenSocket = socket.listen_id;
                    data.m_steamIDRemote = socket.target;
                    data.m_eSNetSocketState = k_ESNetSocketStateConnectionBroken; //TODO is this the right state?
                    callbacks->addCBResult(data.k_iCallback, &data, sizeof(data));
                }
            }
        } else if (msg->low_level().type() == Low_Level::CONNECT) {
            
        }
    }
}

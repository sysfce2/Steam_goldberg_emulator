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

#pragma once

#include "base.h"


class P2p_Manager
{
private:
    struct Peer_Src_t
    {
        CSteamID remote_id{};
        CSteamID my_dest_id{};
    };

    struct Packet_t
    {
        bool is_processed = false;
        std::vector<char> data{};
    };

    struct Channel_t
    {
        std::list<Packet_t> packets{};
    };

    struct Connection_t
    {
        Peer_Src_t peer_conn{};
        std::chrono::high_resolution_clock::time_point time_added = std::chrono::high_resolution_clock::now();
        bool is_accepted = false;
        std::unordered_map<int, Channel_t> channels{};
    };

    std::recursive_mutex p2p_mtx{};
    std::list<Connection_t> connections{};

    class Settings *settings_client{};
    class Settings *settings_server{};
    class SteamCallBacks *callbacks_client{};
    class SteamCallBacks *callbacks_server{};
    class Networking *network{};
    class RunEveryRunCB *run_every_runcb{};

    SteamCallBacks* get_my_callbacks(const CSteamID &my_id) const;
    bool is_same_peer(const CSteamID &id, const CSteamID &peer_id) const;
    void send_peer_session_failure(const Peer_Src_t &peer_conn);
    void trigger_session_request(const CSteamID &remote_id, const CSteamID &my_id);

    bool remove_connection(const CSteamID &remote_id, const CSteamID &my_id);
    Connection_t* create_connection(const CSteamID &remote_id, const CSteamID &my_id);
    Connection_t* get_connection(const CSteamID &remote_id, const CSteamID &my_id);

    // true if the connection was already accepted,
    // false otherwise.
    bool store_packet(CSteamID my_id, CSteamID steamIDRemote, const void *pubData, uint32 cubData, int nChannel);

    void periodic_handle_connections(const std::chrono::high_resolution_clock::time_point &now);
    void periodic_handle_channels(const std::chrono::high_resolution_clock::time_point &now);
    void periodic_handle_packets(const std::chrono::high_resolution_clock::time_point &now);
    void periodic_callback();

    void network_data_packets(Common_Message *msg);
    void network_low_level(Common_Message *msg);
    void network_callback(Common_Message *msg);

    static void steam_networking_callback(void *object, Common_Message *msg);
    static void steam_run_every_runcb(void *object);


public:
    P2p_Manager(
        class Settings *settings_client, class Settings *settings_server,
        class SteamCallBacks *callbacks_client, class SteamCallBacks *callbacks_server,
        class Networking *network, class RunEveryRunCB *run_every_runcb
    );
    ~P2p_Manager();

    bool send_packet(CSteamID my_id, CSteamID steamIDRemote, const void *pubData, uint32 cubData, EP2PSend eP2PSendType, int nChannel);
    bool is_packet_available(CSteamID my_id, uint32 *pcubMsgSize, int nChannel);
    bool read_packet(CSteamID my_id, void *pubDest, uint32 cubDest, uint32 *pcubMsgSize, CSteamID *psteamIDRemote, int nChannel);
    bool close_channel(CSteamID my_id, CSteamID steamIDRemote, int nChannel);
    bool close_session(CSteamID my_id, CSteamID steamIDRemote);
    bool get_session_state(CSteamID my_id, CSteamID steamIDRemote, P2PSessionState_t *pConnectionState);
    bool accept_session(CSteamID my_id, CSteamID steamIDRemote);

};

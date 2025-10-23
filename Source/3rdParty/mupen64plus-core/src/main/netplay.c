/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - netplay.c                                               *
 *   Mupen64Plus homepage: https://mupen64plus.org/                        *
 *   Copyright (C) 2020 loganmc10                                          *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.          *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#define SETTINGS_SIZE 24

#define M64P_CORE_PROTOTYPES 1
#include "api/callbacks.h"
#include "main.h"
#include "util.h"
#include "plugin/plugin.h"
#include "backends/plugins_compat/plugins_compat.h"
#include "netplay.h"

#ifdef USE_SDL3NET
#include <SDL3_net/SDL_net.h>
#else
#include <SDL_net.h>
#endif
#if !defined(WIN32)
#include <netinet/ip.h>
#endif

static int l_canFF;
static int l_netplay_controller;
static int l_netplay_control[4];
#ifdef USE_SDL3NET
static NET_DatagramSocket* l_udpSocket;
static NET_StreamSocket* l_tcpSocket;
#else
static UDPsocket l_udpSocket;
static TCPsocket l_tcpSocket;
#endif
static int l_udpChannel;
static int l_spectator;
static int l_netplay_is_init = 0;
static uint32_t l_vi_counter;
static uint8_t l_status;
static uint32_t l_reg_id;
static struct controller_input_compat *l_cin_compats;
static uint8_t l_plugin[4];
static uint8_t l_buffer_target;
static uint8_t l_player_lag[4];

// Fair Input Delay variables
static int l_fair_input_delay = 0;
static int l_input_delay_frames = 3;

// Frame Synchronization variables
static int l_frame_sync_enabled = 0;
static uint32_t l_lead_frame = 0;
static uint32_t l_local_frame = 0;
static uint32_t l_frame_sync_threshold = 2; // Allow 2 frame difference before slowing down

//UDP packets
#ifdef USE_SDL3NET
// SDL3_net uses dynamic packet allocation
static NET_Datagram* l_process_packet;
static NET_Address* l_server_address;
#else
static UDPpacket *l_request_input_packet;
static UDPpacket *l_send_input_packet;
static UDPpacket *l_process_packet;
static UDPpacket *l_check_sync_packet;
#endif
static const int32_t l_check_sync_packet_size = (CP0_REGS_COUNT * 4) + 5;

//UDP packet formats
#define UDP_SEND_KEY_INFO 0
#define UDP_RECEIVE_KEY_INFO 1
#define UDP_REQUEST_KEY_INFO 2
#define UDP_RECEIVE_KEY_INFO_GRATUITOUS 3
#define UDP_SYNC_DATA 4

//TCP packet formats
#define TCP_SEND_SAVE 1
#define TCP_RECEIVE_SAVE 2
#define TCP_SEND_SETTINGS 3
#define TCP_RECEIVE_SETTINGS 4
#define TCP_REGISTER_PLAYER 5
#define TCP_GET_REGISTRATION 6
#define TCP_DISCONNECT_NOTICE 7

struct __UDPSocket {
    int ready;
    int channel;
};

#define CS4 32

m64p_error netplay_start(const char* host, int port)
{
#ifdef USE_SDL3NET
    // SDL3_net implementation
    l_udpSocket = NET_CreateDatagramSocket(NULL, 0);
    if (l_udpSocket == NULL)
    {
        DebugMessage(M64MSG_ERROR, "Netplay: UDP socket creation failed");
        return M64ERR_SYSTEM_FAIL;
    }

    l_server_address = NET_ResolveHostname(host);
    if (l_server_address == NULL)
    {
        DebugMessage(M64MSG_ERROR, "Netplay: Could not resolve server address");
        NET_DestroyDatagramSocket(l_udpSocket);
        l_udpSocket = NULL;
        return M64ERR_SYSTEM_FAIL;
    }

    l_tcpSocket = NET_CreateClient(l_server_address, port);
    if (l_tcpSocket == NULL)
    {
        DebugMessage(M64MSG_ERROR, "Netplay: TCP socket creation failed");
        NET_DestroyDatagramSocket(l_udpSocket);
        NET_UnrefAddress(l_server_address);
        l_udpSocket = NULL;
        l_server_address = NULL;
        return M64ERR_SYSTEM_FAIL;
    }
#else
    // SDL_net implementation
    if (SDLNet_Init() < 0)
    {
        DebugMessage(M64MSG_ERROR, "Netplay: Could not initialize SDL Net library");
        return M64ERR_SYSTEM_FAIL;
    }

    l_udpSocket = SDLNet_UDP_Open(0);
    if (l_udpSocket == NULL)
    {
        DebugMessage(M64MSG_ERROR, "Netplay: UDP socket creation failed");
        return M64ERR_SYSTEM_FAIL;
    }

#if !defined(WIN32)
    const char tos_local = CS4 << 2;
    struct __UDPSocket* socket = (struct __UDPSocket*) l_udpSocket;
    setsockopt(socket->channel, IPPROTO_IP, IP_TOS, &tos_local, sizeof(tos_local));
#endif

    IPaddress dest;
    SDLNet_ResolveHost(&dest, host, port);

    l_udpChannel = SDLNet_UDP_Bind(l_udpSocket, -1, &dest);
    if (l_udpChannel < 0)
    {
        DebugMessage(M64MSG_ERROR, "Netplay: could not bind to UDP socket");
        SDLNet_UDP_Close(l_udpSocket);
        l_udpSocket = NULL;
        return M64ERR_SYSTEM_FAIL;
    }

    l_tcpSocket = SDLNet_TCP_Open(&dest);
    if (l_tcpSocket == NULL)
    {
        DebugMessage(M64MSG_ERROR, "Netplay: could not bind to TCP socket");
        SDLNet_UDP_Close(l_udpSocket);
        l_udpSocket = NULL;
        return M64ERR_SYSTEM_FAIL;
    }

    l_request_input_packet = SDLNet_AllocPacket(12);
    l_send_input_packet = SDLNet_AllocPacket(11);
    l_process_packet = SDLNet_AllocPacket(512);
    l_check_sync_packet = SDLNet_AllocPacket(l_check_sync_packet_size);
    if (l_request_input_packet == NULL ||
        l_send_input_packet == NULL ||
        l_process_packet == NULL ||
        l_check_sync_packet == NULL)
    {
        DebugMessage(M64MSG_ERROR, "Netplay: could not allocate UDP packets");
        SDLNet_UDP_Close(l_udpSocket);
        l_udpSocket = NULL;
        SDLNet_TCP_Close(l_tcpSocket);
        l_tcpSocket = NULL;
        SDLNet_FreePacket(l_request_input_packet);
        l_request_input_packet = NULL;
        SDLNet_FreePacket(l_send_input_packet);
        l_send_input_packet = NULL;
        SDLNet_FreePacket(l_process_packet);
        l_process_packet = NULL;
        SDLNet_FreePacket(l_check_sync_packet);
        l_check_sync_packet = NULL;
        return M64ERR_NO_MEMORY;
    }
#endif

    for (int i = 0; i < 4; ++i)
    {
        l_netplay_control[i] = -1;
        l_plugin[i] = 0;
        l_player_lag[i] = 0;
    }

    l_canFF = 0;
    l_netplay_controller = 0;
    l_netplay_is_init = 1;
    l_spectator = 1;
    l_vi_counter = 0;
    l_status = 0;
    l_reg_id = 0;

    return M64ERR_SUCCESS;
}

m64p_error netplay_stop()
{
    if (l_udpSocket == NULL)
        return M64ERR_INVALID_STATE;
    else
    {
        if (l_cin_compats != NULL)
        {
            for (int i = 0; i < 4; ++i)
            {
                struct netplay_event* current = l_cin_compats[i].event_first;
                struct netplay_event* next;
                while (current != NULL)
                {
                    next = current->next;
                    free(current);
                    current = next;
                }
            }
        }

#ifdef USE_SDL3NET
        // SDL3_net implementation
        char output_data[5];
        output_data[0] = TCP_DISCONNECT_NOTICE;
        // Write registration ID (assuming we have a helper function)
        uint32_t reg_id_le = SDL_Swap32LE(l_reg_id);
        memcpy(&output_data[1], &reg_id_le, 4);
        
        NET_WriteToStreamSocket(l_tcpSocket, output_data, 5);

        NET_DestroyDatagramSocket(l_udpSocket);
        NET_DestroyStreamSocket(l_tcpSocket);
        NET_UnrefAddress(l_server_address);
        l_tcpSocket = NULL;
        l_udpSocket = NULL;
        l_server_address = NULL;
#else
        // SDL_net implementation
        char output_data[5];
        output_data[0] = TCP_DISCONNECT_NOTICE;
        SDLNet_Write32(l_reg_id, &output_data[1]);
        SDLNet_TCP_Send(l_tcpSocket, &output_data[0], 5);

        SDLNet_UDP_Unbind(l_udpSocket, l_udpChannel);
        SDLNet_UDP_Close(l_udpSocket);
        SDLNet_TCP_Close(l_tcpSocket);
        l_tcpSocket = NULL;
        l_udpSocket = NULL;
        l_udpChannel = -1;

        SDLNet_FreePacket(l_request_input_packet);
        SDLNet_FreePacket(l_send_input_packet);
        SDLNet_FreePacket(l_process_packet);
        SDLNet_FreePacket(l_check_sync_packet);
        l_request_input_packet = NULL;
        l_send_input_packet = NULL;
        l_process_packet = NULL;
        l_check_sync_packet = NULL;
    
        SDLNet_Quit();
#endif
    
        l_netplay_is_init = 0;
        return M64ERR_SUCCESS;
    }
}

int netplay_is_init()
{
    return l_netplay_is_init;
}

static uint8_t buffer_size(uint8_t control_id)
{
    //This function returns the size of the local input buffer
    uint8_t counter = 0;
    struct netplay_event* current = l_cin_compats[control_id].event_first;
    while (current != NULL)
    {
        current = current->next;
        ++counter;
    }
    return counter;
}

static void netplay_request_input(uint8_t control_id)
{
#ifdef USE_SDL3NET
    // SDL3_net implementation
    uint8_t packet_data[12];
    packet_data[0] = UDP_REQUEST_KEY_INFO;
    packet_data[1] = control_id; //The player we need input for
    uint32_t reg_id_le = SDL_Swap32LE(l_reg_id);
    memcpy(&packet_data[2], &reg_id_le, 4); //our registration ID
    uint32_t count_le = SDL_Swap32LE(l_cin_compats[control_id].netplay_count);
    memcpy(&packet_data[6], &count_le, 4); //the current event count
    packet_data[10] = l_spectator; //whether we are a spectator
    packet_data[11] = buffer_size(control_id); //our local buffer size
    
    NET_SendDatagram(l_udpSocket, l_server_address, 0, packet_data, 12);
#else
    // SDL_net implementation
    l_request_input_packet->data[0] = UDP_REQUEST_KEY_INFO;
    l_request_input_packet->data[1] = control_id; //The player we need input for
    SDLNet_Write32(l_reg_id, &l_request_input_packet->data[2]); //our registration ID
    SDLNet_Write32(l_cin_compats[control_id].netplay_count, &l_request_input_packet->data[6]); //the current event count
    l_request_input_packet->data[10] = l_spectator; //whether we are a spectator
    l_request_input_packet->data[11] = buffer_size(control_id); //our local buffer size
    l_request_input_packet->len = 12;
    SDLNet_UDP_Send(l_udpSocket, l_udpChannel, l_request_input_packet);
#endif
}

static int check_valid(uint8_t control_id, uint32_t count)
{
    //Check if we already have this event recorded locally, returns 1 if we do
    struct netplay_event* current = l_cin_compats[control_id].event_first;
    while (current != NULL)
    {
        if (current->count == count) //event already recorded
            return 1;
        current = current->next;
    }
    return 0;
}

static int netplay_require_response(void* opaque)
{
    //This function runs inside a thread.
    //It runs if our local buffer size is 0 (we need to execute a key event, but we don't have the data we need).
    //We basically beg the server for input data.
    //After 10 seconds a timeout occurs, we assume we have lost connection to the server.
    uint8_t control_id = *(uint8_t*)opaque;
    uint32_t timeout = SDL_GetTicks() + 10000;
    while (!check_valid(control_id, l_cin_compats[control_id].netplay_count))
    {
        if (SDL_GetTicks() > timeout)
        {
            l_udpChannel = -1;
            return 0;
        }
        netplay_request_input(control_id);
        SDL_Delay(5);
    }
    return 1;
}

static void netplay_process()
{
    //In this function we process data we have received from the server
    uint32_t curr, count, keys;
    uint8_t plugin, player, current_status;
    
#ifdef USE_SDL3NET
    // SDL3_net implementation
    while (NET_ReceiveDatagram(l_udpSocket, &l_process_packet))
    {
        if (l_process_packet == NULL) break;
        
        switch (l_process_packet->buf[0])
        {
            case UDP_RECEIVE_KEY_INFO:
            case UDP_RECEIVE_KEY_INFO_GRATUITOUS:
                player = l_process_packet->buf[1];
                current_status = l_process_packet->buf[2];
                if (l_process_packet->buf[0] == UDP_RECEIVE_KEY_INFO)
                    l_player_lag[player] = l_process_packet->buf[3];
                if (current_status != l_status)
                {
                    if (((current_status & 0x1) ^ (l_status & 0x1)) != 0)
                        DebugMessage(M64MSG_ERROR, "Netplay: players have de-synced at VI %u", l_vi_counter);
                    for (int dis = 1; dis < 5; ++dis)
                    {
                        if (((current_status & (0x1 << dis)) ^ (l_status & (0x1 << dis))) != 0)
                            DebugMessage(M64MSG_ERROR, "Netplay: player %u has disconnected", dis);
                    }
                    l_status = current_status;
                }
                curr = 5;
                for (uint8_t i = 0; i < l_process_packet->buf[4]; ++i)
                {
                    count = SDL_Swap32LE(*(uint32_t*)&l_process_packet->buf[curr]);
                    curr += 4;

                    if (((count - l_cin_compats[player].netplay_count) > (UINT32_MAX / 2)) || (check_valid(player, count)))
                    {
                        curr += 5;
                        continue;
                    }

                    keys = SDL_Swap32LE(*(uint32_t*)&l_process_packet->buf[curr]);
                    curr += 4;
                    plugin = l_process_packet->buf[curr];
                    curr += 1;

                    struct netplay_event* new_event = (struct netplay_event*)malloc(sizeof(struct netplay_event));
                    new_event->count = count;
                    new_event->buttons = keys;
                    new_event->plugin = plugin;
                    new_event->next = l_cin_compats[player].event_first;
                    l_cin_compats[player].event_first = new_event;
                }
                break;
            default:
                DebugMessage(M64MSG_ERROR, "Netplay: received unknown message from server");
                break;
        }
        
        NET_DestroyDatagram(l_process_packet);
    }
#else
    // SDL_net implementation
    while (SDLNet_UDP_Recv(l_udpSocket, l_process_packet) == 1)
    {
        switch (l_process_packet->data[0])
        {
            case UDP_RECEIVE_KEY_INFO:
            case UDP_RECEIVE_KEY_INFO_GRATUITOUS:
                player = l_process_packet->data[1];
                //current_status is a status update from the server
                //it will let us know if another player has disconnected, or the games have desynced
                current_status = l_process_packet->data[2];
                if (l_process_packet->data[0] == UDP_RECEIVE_KEY_INFO)
                    l_player_lag[player] = l_process_packet->data[3];
                if (current_status != l_status)
                {
                    if (((current_status & 0x1) ^ (l_status & 0x1)) != 0)
                        DebugMessage(M64MSG_ERROR, "Netplay: players have de-synced at VI %u", l_vi_counter);
                    for (int dis = 1; dis < 5; ++dis)
                    {
                        if (((current_status & (0x1 << dis)) ^ (l_status & (0x1 << dis))) != 0)
                            DebugMessage(M64MSG_ERROR, "Netplay: player %u has disconnected", dis);
                    }
                    l_status = current_status;
                }
                curr = 5;
                //this loop processes input data from the server, inserting new events into the linked list for each player
                //it skips events that we have already recorded, or if we receive data for an event that has already happened
                for (uint8_t i = 0; i < l_process_packet->data[4]; ++i)
                {
                    count = SDLNet_Read32(&l_process_packet->data[curr]);
                    curr += 4;

                    if (((count - l_cin_compats[player].netplay_count) > (UINT32_MAX / 2)) || (check_valid(player, count))) //event doesn't need to be recorded
                    {
                        curr += 5;
                        continue;
                    }

                    keys = SDLNet_Read32(&l_process_packet->data[curr]);
                    curr += 4;
                    plugin = l_process_packet->data[curr];
                    curr += 1;

                    //insert new event at beginning of linked list
                    struct netplay_event* new_event = (struct netplay_event*)malloc(sizeof(struct netplay_event));
                    new_event->count = count;
                    new_event->buttons = keys;
                    new_event->plugin = plugin;
                    new_event->next = l_cin_compats[player].event_first;
                    l_cin_compats[player].event_first = new_event;
                }
                break;
            default:
                DebugMessage(M64MSG_ERROR, "Netplay: received unknown message from server");
                break;
        }
    }
#endif
}

static int netplay_ensure_valid(uint8_t control_id)
{
    //This function makes sure we have data for a certain event
    //If we don't have the data, it will create a new thread that will request the data
    if (check_valid(control_id, l_cin_compats[control_id].netplay_count))
        return 1;

    if (l_udpChannel == -1)
        return 0;

    SDL_Thread* thread = SDL_CreateThread(netplay_require_response, "Netplay key request", &control_id);

    while (!check_valid(control_id, l_cin_compats[control_id].netplay_count) && l_udpChannel != -1)
        netplay_process();
    int success;
    SDL_WaitThread(thread, &success);
    return success;
}

static void netplay_delete_event(struct netplay_event* current, uint8_t control_id)
{
    //This function deletes an event from the linked list
    struct netplay_event* find = l_cin_compats[control_id].event_first;
    while (find != NULL)
    {
        if (find->next == current)
        {
            find->next = current->next;
            break;
        }
        find = find->next;
    }
    if (current == l_cin_compats[control_id].event_first)
        l_cin_compats[control_id].event_first = l_cin_compats[control_id].event_first->next;
    free(current);
}

static uint32_t netplay_get_input(uint8_t control_id)
{
    uint32_t keys;
    netplay_process();
    netplay_request_input(control_id);

    //l_buffer_target is set by the server upon registration
    //l_player_lag is how far behind we are from the lead player
    //buffer_size is the local buffer size
    if (l_player_lag[control_id] > 0 && buffer_size(control_id) > l_buffer_target)
    {
        l_canFF = 1;
        main_core_state_set(M64CORE_SPEED_LIMITER, 0);
    }
    else
    {
        main_core_state_set(M64CORE_SPEED_LIMITER, 1);
        l_canFF = 0;
    }

    if (netplay_ensure_valid(control_id))
    {
        //We grab the event from the linked list, the delete it once it has been used
        //Finally we increment the event counter
        struct netplay_event* current = l_cin_compats[control_id].event_first;
        while (current->count != l_cin_compats[control_id].netplay_count)
            current = current->next;
        keys = current->buttons;
        Controls[control_id].Plugin = current->plugin;
        netplay_delete_event(current, control_id);
        ++l_cin_compats[control_id].netplay_count;
    }
    else
    {
        DebugMessage(M64MSG_ERROR, "Netplay: lost connection to server");
        main_core_state_set(M64CORE_EMU_STATE, M64EMU_STOPPED);
        keys = 0;
    }

    return keys;
}

static void netplay_send_input(uint8_t control_id, uint32_t keys)
{
#ifdef USE_SDL3NET
    // SDL3_net implementation
    uint8_t packet_data[11];
    packet_data[0] = UDP_SEND_KEY_INFO;
    packet_data[1] = control_id; //player number
    uint32_t count_le = SDL_Swap32LE(l_cin_compats[control_id].netplay_count);
    memcpy(&packet_data[2], &count_le, 4); // current event count
    uint32_t keys_le = SDL_Swap32LE(keys);
    memcpy(&packet_data[6], &keys_le, 4); //key data
    packet_data[10] = l_plugin[control_id]; //current plugin
    
    NET_SendDatagram(l_udpSocket, l_server_address, 0, packet_data, 11);
#else
    // SDL_net implementation
    l_send_input_packet->data[0] = UDP_SEND_KEY_INFO;
    l_send_input_packet->data[1] = control_id; //player number
    SDLNet_Write32(l_cin_compats[control_id].netplay_count, &l_send_input_packet->data[2]); // current event count
    SDLNet_Write32(keys, &l_send_input_packet->data[6]); //key data
    l_send_input_packet->data[10] = l_plugin[control_id]; //current plugin
    l_send_input_packet->len = 11;
    SDLNet_UDP_Send(l_udpSocket, l_udpChannel, l_send_input_packet);
#endif
}

uint8_t netplay_register_player(uint8_t player, uint8_t plugin, uint8_t rawdata, uint32_t reg_id)
{
    l_reg_id = reg_id;
    char output_data[8];
    output_data[0] = TCP_REGISTER_PLAYER;
    output_data[1] = player; //player number we'd like to register
    output_data[2] = plugin; //current plugin
    output_data[3] = rawdata; //whether we are using a RawData input plugin

#ifdef USE_SDL3NET
    // SDL3_net implementation
    uint32_t reg_id_le = SDL_Swap32LE(l_reg_id);
    memcpy(&output_data[4], &reg_id_le, 4);

    NET_WriteToStreamSocket(l_tcpSocket, output_data, 8);

    uint8_t response[2];
    size_t recv = 0;
    while (recv < 2)
        recv += NET_ReadFromStreamSocket(l_tcpSocket, &response[recv], 2 - recv);
#else
    // SDL_net implementation
    SDLNet_Write32(l_reg_id, &output_data[4]);

    SDLNet_TCP_Send(l_tcpSocket, &output_data[0], 8);

    uint8_t response[2];
    size_t recv = 0;
    while (recv < 2)
        recv += SDLNet_TCP_Recv(l_tcpSocket, &response[recv], 2 - recv);
#endif

    l_buffer_target = response[1]; //local buffer size target
    return response[0];
}

int netplay_lag()
{
    return l_canFF;
}

int netplay_next_controller()
{
    return l_netplay_controller;
}

void netplay_set_controller(uint8_t player)
{
    l_netplay_control[player] = l_netplay_controller++;
    l_spectator = 0;
}

int netplay_get_controller(uint8_t player)
{
    return l_netplay_control[player];
}

file_status_t netplay_read_storage(const char *filename, void *data, size_t size)
{
    //This function syncs save games.
    //If the client is controlling player 1, it sends its save game to the server
    //All other players receive save files from the server
    const char *file_extension = strrchr(filename, '.');
    file_extension += 1;

    uint32_t buffer_pos = 0;
    char *output_data = malloc(size + strlen(file_extension) + 6);

    file_status_t ret;
    uint8_t request;
    if (l_netplay_control[0] != -1)
    {
        request = TCP_SEND_SAVE;
        memcpy(&output_data[buffer_pos], &request, 1);
        ++buffer_pos;

         //send file extension
        memcpy(&output_data[buffer_pos], file_extension, strlen(file_extension) + 1);
        buffer_pos += strlen(file_extension) + 1;

        ret = read_from_file(filename, data, size);
        if (ret == file_open_error)
            memset(data, 0, size); //all zeros means there is no save file
#ifdef USE_SDL3NET
        // SDL3_net implementation
        uint32_t size_le = SDL_Swap32LE((int32_t)size);
        memcpy(&output_data[buffer_pos], &size_le, 4); //file data size
#else
        // SDL_net implementation
        SDLNet_Write32((int32_t)size, &output_data[buffer_pos]); //file data size
#endif
        buffer_pos += 4;
        memcpy(&output_data[buffer_pos], data, size); //file data
        buffer_pos += size;

#ifdef USE_SDL3NET
        // SDL3_net implementation
        NET_WriteToStreamSocket(l_tcpSocket, output_data, buffer_pos);
#else
        // SDL_net implementation
        SDLNet_TCP_Send(l_tcpSocket, &output_data[0], buffer_pos);
#endif
    }
    else
    {
        request = TCP_RECEIVE_SAVE;
        memcpy(&output_data[buffer_pos], &request, 1);
        ++buffer_pos;

        //extension of the file we are requesting
        memcpy(&output_data[buffer_pos], file_extension, strlen(file_extension) + 1);
        buffer_pos += strlen(file_extension) + 1;

#ifdef USE_SDL3NET
        // SDL3_net implementation
        NET_WriteToStreamSocket(l_tcpSocket, output_data, buffer_pos);
        size_t recv = 0;
        char *data_array = data;
        while (recv < size)
            recv += NET_ReadFromStreamSocket(l_tcpSocket, data_array + recv, size - recv);
#else
        // SDL_net implementation
        SDLNet_TCP_Send(l_tcpSocket, &output_data[0], buffer_pos);
        size_t recv = 0;
        char *data_array = data;
        while (recv < size)
            recv += SDLNet_TCP_Recv(l_tcpSocket, data_array + recv, size - recv);
#endif

        int sum = 0;
        for (int i = 0; i < size; ++i)
            sum |= data_array[i];

        if (sum == 0) //all zeros means there is no save file
            ret = file_open_error;
        else
            ret = file_ok;
    }
    free(output_data);
    return ret;
}

void netplay_sync_settings(uint32_t *count_per_op, uint32_t *count_per_op_denom_pot, uint32_t *disable_extra_mem, int32_t *si_dma_duration, uint32_t *emumode, int32_t *no_compiled_jump)
{
    if (!netplay_is_init())
        return;

    char output_data[SETTINGS_SIZE + 1];
    uint8_t request;
    if (l_netplay_control[0] != -1) //player 1 is the source of truth for settings
    {
        request = TCP_SEND_SETTINGS;
        memcpy(&output_data[0], &request, 1);
#ifdef USE_SDL3NET
        // SDL3_net implementation
        uint32_t count_per_op_le = SDL_Swap32LE(*count_per_op);
        memcpy(&output_data[1], &count_per_op_le, 4);
        uint32_t count_per_op_denom_pot_le = SDL_Swap32LE(*count_per_op_denom_pot);
        memcpy(&output_data[5], &count_per_op_denom_pot_le, 4);
        uint32_t disable_extra_mem_le = SDL_Swap32LE(*disable_extra_mem);
        memcpy(&output_data[9], &disable_extra_mem_le, 4);
        uint32_t si_dma_duration_le = SDL_Swap32LE(*si_dma_duration);
        memcpy(&output_data[13], &si_dma_duration_le, 4);
        uint32_t emumode_le = SDL_Swap32LE(*emumode);
        memcpy(&output_data[17], &emumode_le, 4);
        uint32_t no_compiled_jump_le = SDL_Swap32LE(*no_compiled_jump);
        memcpy(&output_data[21], &no_compiled_jump_le, 4);
        
        NET_WriteToStreamSocket(l_tcpSocket, output_data, SETTINGS_SIZE + 1);
#else
        // SDL_net implementation
        SDLNet_Write32(*count_per_op, &output_data[1]);
        SDLNet_Write32(*count_per_op_denom_pot, &output_data[5]);
        SDLNet_Write32(*disable_extra_mem, &output_data[9]);
        SDLNet_Write32(*si_dma_duration, &output_data[13]);
        SDLNet_Write32(*emumode, &output_data[17]);
        SDLNet_Write32(*no_compiled_jump, &output_data[21]);
        SDLNet_TCP_Send(l_tcpSocket, &output_data[0], SETTINGS_SIZE + 1);
#endif
    }
    else
    {
        request = TCP_RECEIVE_SETTINGS;
        memcpy(&output_data[0], &request, 1);
#ifdef USE_SDL3NET
        // SDL3_net implementation
        NET_WriteToStreamSocket(l_tcpSocket, output_data, 1);
        int32_t recv = 0;
        while (recv < SETTINGS_SIZE)
            recv += NET_ReadFromStreamSocket(l_tcpSocket, &output_data[recv], SETTINGS_SIZE - recv);
        *count_per_op = SDL_Swap32LE(*(uint32_t*)&output_data[0]);
        *count_per_op_denom_pot = SDL_Swap32LE(*(uint32_t*)&output_data[4]);
        *disable_extra_mem = SDL_Swap32LE(*(uint32_t*)&output_data[8]);
        *si_dma_duration = SDL_Swap32LE(*(uint32_t*)&output_data[12]);
        *emumode = SDL_Swap32LE(*(uint32_t*)&output_data[16]);
        *no_compiled_jump = SDL_Swap32LE(*(uint32_t*)&output_data[20]);
#else
        // SDL_net implementation
        SDLNet_TCP_Send(l_tcpSocket, &output_data[0], 1);
        int32_t recv = 0;
        while (recv < SETTINGS_SIZE)
            recv += SDLNet_TCP_Recv(l_tcpSocket, &output_data[recv], SETTINGS_SIZE - recv);
        *count_per_op = SDLNet_Read32(&output_data[0]);
        *count_per_op_denom_pot = SDLNet_Read32(&output_data[4]);
        *disable_extra_mem = SDLNet_Read32(&output_data[8]);
        *si_dma_duration = SDLNet_Read32(&output_data[12]);
        *emumode = SDLNet_Read32(&output_data[16]);
        *no_compiled_jump = SDLNet_Read32(&output_data[20]);
#endif
    }
}

void netplay_check_sync(struct cp0* cp0)
{
    //This function is used to check if games have desynced
    //Every 600 VIs, it sends the value of the CP0 registers to the server
    //The server will compare the values, and update the status byte if it detects a desync
    if (!netplay_is_init())
        return;

    if (l_vi_counter % 600 == 0)
    {
        const uint32_t* cp0_regs = r4300_cp0_regs(cp0);

#ifdef USE_SDL3NET
        // SDL3_net implementation
        uint8_t packet_data[l_check_sync_packet_size];
        packet_data[0] = UDP_SYNC_DATA;
        uint32_t vi_counter_le = SDL_Swap32LE(l_vi_counter);
        memcpy(&packet_data[1], &vi_counter_le, 4); //current VI count
        for (int i = 0; i < CP0_REGS_COUNT; ++i)
        {
            uint32_t reg_le = SDL_Swap32LE(cp0_regs[i]);
            memcpy(&packet_data[(i * 4) + 5], &reg_le, 4);
        }
        
        NET_SendDatagram(l_udpSocket, l_server_address, 0, packet_data, l_check_sync_packet_size);
#else
        // SDL_net implementation
        l_check_sync_packet->data[0] = UDP_SYNC_DATA;
        SDLNet_Write32(l_vi_counter, &l_check_sync_packet->data[1]); //current VI count
        for (int i = 0; i < CP0_REGS_COUNT; ++i)
        {
            SDLNet_Write32(cp0_regs[i], &l_check_sync_packet->data[(i * 4) + 5]);
        }
        l_check_sync_packet->len = l_check_sync_packet_size;
        SDLNet_UDP_Send(l_udpSocket, l_udpChannel, l_check_sync_packet);
#endif
    }

    ++l_vi_counter;
}

void netplay_read_registration(struct controller_input_compat* cin_compats)
{
    //This function runs right before the game starts
    //The server shares the registration details about each player
    if (!netplay_is_init())
        return;

    l_cin_compats = cin_compats;

    uint32_t reg_id;
    char output_data = TCP_GET_REGISTRATION;
    char input_data[24];

#ifdef USE_SDL3NET
    // SDL3_net implementation
    NET_WriteToStreamSocket(l_tcpSocket, &output_data, 1);
    size_t recv = 0;
    while (recv < 24)
        recv += NET_ReadFromStreamSocket(l_tcpSocket, &input_data[recv], 24 - recv);
    uint32_t curr = 0;
    for (int i = 0; i < 4; ++i)
    {
        reg_id = SDL_Swap32LE(*(uint32_t*)&input_data[curr]);
        curr += 4;

        Controls[i].Type = CONT_TYPE_STANDARD; //make sure VRU is disabled

        if (reg_id == 0) //No one registered to control this player
        {
            Controls[i].Present = 0;
            Controls[i].Plugin = PLUGIN_NONE;
            Controls[i].RawData = 0;
            curr += 2;
        }
        else
        {
            Controls[i].Present = 1;
            if (i > 0 && input_data[curr] == PLUGIN_MEMPAK) // only P1 can use mempak
                Controls[i].Plugin = PLUGIN_NONE;
            else if (input_data[curr] == PLUGIN_TRANSFER_PAK) // Transferpak not supported during netplay
                Controls[i].Plugin = PLUGIN_NONE;
            else
                Controls[i].Plugin = input_data[curr];
            l_plugin[i] = Controls[i].Plugin;
            ++curr;
            Controls[i].RawData = input_data[curr];
            ++curr;
        }
    }
#else
    // SDL_net implementation
    SDLNet_TCP_Send(l_tcpSocket, &output_data, 1);
    size_t recv = 0;
    while (recv < 24)
        recv += SDLNet_TCP_Recv(l_tcpSocket, &input_data[recv], 24 - recv);
    uint32_t curr = 0;
    for (int i = 0; i < 4; ++i)
    {
        reg_id = SDLNet_Read32(&input_data[curr]);
        curr += 4;

        Controls[i].Type = CONT_TYPE_STANDARD; //make sure VRU is disabled

        if (reg_id == 0) //No one registered to control this player
        {
            Controls[i].Present = 0;
            Controls[i].Plugin = PLUGIN_NONE;
            Controls[i].RawData = 0;
            curr += 2;
        }
        else
        {
            Controls[i].Present = 1;
            if (i > 0 && input_data[curr] == PLUGIN_MEMPAK) // only P1 can use mempak
                Controls[i].Plugin = PLUGIN_NONE;
            else if (input_data[curr] == PLUGIN_TRANSFER_PAK) // Transferpak not supported during netplay
                Controls[i].Plugin = PLUGIN_NONE;
            else
                Controls[i].Plugin = input_data[curr];
            l_plugin[i] = Controls[i].Plugin;
            ++curr;
            Controls[i].RawData = input_data[curr];
            ++curr;
        }
    }
#endif
}

static void netplay_send_raw_input(struct pif* pif)
{
    for (int i = 0; i < 4; ++i)
    {
        if (l_netplay_control[i] != -1)
        {
            if (pif->channels[i].tx && pif->channels[i].tx_buf[0] == JCMD_CONTROLLER_READ)
                netplay_send_input(i, *(uint32_t*)pif->channels[i].rx_buf);
        }
    }
}

static void netplay_get_raw_input(struct pif* pif)
{
    for (int i = 0; i < 4; ++i)
    {
        if (Controls[i].Present == 1)
        {
            if (pif->channels[i].tx)
            {
                *pif->channels[i].rx &= ~0xC0; //Always show the controller as connected

                if(pif->channels[i].tx_buf[0] == JCMD_CONTROLLER_READ)
                {
                    *(uint32_t*)pif->channels[i].rx_buf = netplay_get_input(i);
                }
                else if ((pif->channels[i].tx_buf[0] == JCMD_STATUS || pif->channels[i].tx_buf[0] == JCMD_RESET) && Controls[i].RawData)
                {
                    //a bit of a hack for raw input controllers, force the status
                    uint16_t type = JDT_JOY_ABS_COUNTERS | JDT_JOY_PORT;
                    pif->channels[i].rx_buf[0] = (uint8_t)(type >> 0);
                    pif->channels[i].rx_buf[1] = (uint8_t)(type >> 8);
                    pif->channels[i].rx_buf[2] = 0;
                }
                else if (pif->channels[i].tx_buf[0] == JCMD_PAK_READ && Controls[i].RawData)
                {
                    //also a hack for raw input, we return "mempak not present" if the game tries to read the mempak
                    pif->channels[i].rx_buf[32] = 255;
                }
                else if (pif->channels[i].tx_buf[0] == JCMD_PAK_WRITE && Controls[i].RawData)
                {
                    //also a hack for raw input, we return "mempak not present" if the game tries to write to mempak
                    pif->channels[i].rx_buf[0] = 255;
                }
            }
        }
    }
}

void netplay_update_input(struct pif* pif)
{
    if (netplay_is_init())
    {
        netplay_send_raw_input(pif);
        netplay_get_raw_input(pif);
    }
}

m64p_error netplay_send_config(char* data, int size)
{
    if (!netplay_is_init())
        return M64ERR_NOT_INIT;

    if (l_netplay_control[0] != -1 || size == 1) //Only P1 sends settings, we allow all players to send if the size is 1, this may be a request packet
    {
#ifdef USE_SDL3NET
        // SDL3_net implementation
        int result = NET_WriteToStreamSocket(l_tcpSocket, data, size);
        if (result < size)
            return M64ERR_SYSTEM_FAIL;
        return M64ERR_SUCCESS;
#else
        // SDL_net implementation
        int result = SDLNet_TCP_Send(l_tcpSocket, data, size);
        if (result < size)
            return M64ERR_SYSTEM_FAIL;
        return M64ERR_SUCCESS;
#endif
    }
    else
        return M64ERR_INVALID_STATE;
}

m64p_error netplay_receive_config(char* data, int size)
{
    if (!netplay_is_init())
        return M64ERR_NOT_INIT;

    if (l_netplay_control[0] == -1) //Only P2-4 receive settings
    {
#ifdef USE_SDL3NET
        // SDL3_net implementation
        int recv = 0;
        while (recv < size)
        {
            recv += NET_ReadFromStreamSocket(l_tcpSocket, &data[recv], size - recv);
            if (recv < 1)
                return M64ERR_SYSTEM_FAIL;
        }
        return M64ERR_SUCCESS;
#else
        // SDL_net implementation
        int recv = 0;
        while (recv < size)
        {
            recv += SDLNet_TCP_Recv(l_tcpSocket, &data[recv], size - recv);
            if (recv < 1)
                return M64ERR_SYSTEM_FAIL;
        }
        return M64ERR_SUCCESS;
#endif
    }
    else
        return M64ERR_INVALID_STATE;
}

// Fair Input Delay Functions
int core_get_fair_input_delay(void)
{
    return l_fair_input_delay;
}

int core_get_input_delay_frames(void)
{
    return l_input_delay_frames;
}

void core_set_fair_input_delay(int enabled)
{
    l_fair_input_delay = enabled;
}

void core_set_input_delay_frames(int frames)
{
    if (frames >= 0 && frames <= 10) {
        l_input_delay_frames = frames;
    }
}

// Frame Synchronization Functions
int core_get_frame_sync_enabled(void)
{
    return l_frame_sync_enabled;
}

void core_set_frame_sync_enabled(int enabled)
{
    l_frame_sync_enabled = enabled;
}

uint32_t core_get_lead_frame(void)
{
    return l_lead_frame;
}

void core_set_lead_frame(uint32_t frame)
{
    l_lead_frame = frame;
}

uint32_t core_get_local_frame(void)
{
    return l_local_frame;
}

void core_set_local_frame(uint32_t frame)
{
    l_local_frame = frame;
}

uint32_t core_get_frame_sync_threshold(void)
{
    return l_frame_sync_threshold;
}

void core_set_frame_sync_threshold(uint32_t threshold)
{
    l_frame_sync_threshold = threshold;
}

// Check if we need to slow down due to frame synchronization
int core_should_slow_down(void)
{
    if (!l_frame_sync_enabled) {
        return 0;
    }
    
    // If we're ahead of the lead frame, slow down
    if (l_local_frame > l_lead_frame) {
        return 1;
    }
    
    // If we're more than threshold frames behind, don't slow down
    if ((l_lead_frame - l_local_frame) > l_frame_sync_threshold) {
        return 0;
    }
    
    // If we're within threshold but behind, slow down to catch up
    if (l_local_frame < l_lead_frame) {
        return 1;
    }
    
    return 0;
}

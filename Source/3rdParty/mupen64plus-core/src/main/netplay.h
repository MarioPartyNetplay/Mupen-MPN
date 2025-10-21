/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - netplay.h                                               *
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

#ifndef __NETPLAY_H__
#define __NETPLAY_H__

#include "device/r4300/cp0.h"
#include "device/pif/pif.h"
#include "main/util.h"

#ifdef M64P_NETPLAY
#ifdef USE_SDL3NET
#include <SDL3_net/SDL_net.h>
// Typedef for UDP packet structure - SDL3_net uses NET_Datagram instead of UDPpacket
typedef NET_Datagram netplay_udp_packet;
#else
#include <SDL_net.h>
// Typedef for UDP packet structure
typedef UDPpacket netplay_udp_packet;
#endif
#endif

#define NETPLAY_CORE_VERSION 1

struct netplay_event {
    uint32_t buttons;
    uint8_t plugin;
    uint32_t count;
    struct netplay_event* next;
};

struct controller_input_compat;

#ifdef M64P_NETPLAY

m64p_error netplay_start(const char* host, int port);
m64p_error netplay_stop();
uint8_t netplay_register_player(uint8_t player, uint8_t plugin, uint8_t rawdata, uint32_t reg_id);
int netplay_lag();
void netplay_set_controller(uint8_t player);
int netplay_is_init();
int netplay_get_controller(uint8_t player);
file_status_t netplay_read_storage(const char *filename, void *data, size_t size);
void netplay_sync_settings(uint32_t *count_per_op, uint32_t *count_per_op_denom_pot, uint32_t *disable_extra_mem, int32_t *si_dma_duration, uint32_t *emumode, int32_t *no_compiled_jump);
void netplay_check_sync(struct cp0* cp0);
int netplay_next_controller();
void netplay_read_registration(struct controller_input_compat* cin_compats);
void netplay_update_input(struct pif* pif);
m64p_error netplay_send_config(char* data, int size);
m64p_error netplay_receive_config(char* data, int size);

// Enhanced throttling functions
uint8_t netplay_get_throttle_level(uint8_t player);
void netplay_reset_throttling(uint8_t player);
uint8_t netplay_get_buffer_health(uint8_t player);
uint8_t netplay_get_player_lag(uint8_t player);
int netplay_has_throttled_players();
void netplay_get_throttling_stats(uint8_t* throttle_levels, uint8_t* buffer_health, uint8_t* player_lags);
void netplay_log_throttling_status();
uint8_t netplay_get_total_throttle_level();

// Fair Input Delay Functions
int core_get_fair_input_delay(void);
int core_get_input_delay_frames(void);
void core_set_fair_input_delay(int enabled);
void core_set_input_delay_frames(int frames);

#else

static osal_inline m64p_error netplay_start(const char* host, int port)
{
    return M64ERR_INCOMPATIBLE;
}

static osal_inline m64p_error netplay_stop()
{
    return M64ERR_INCOMPATIBLE;
}

static osal_inline uint8_t netplay_register_player(uint8_t player, uint8_t plugin, uint8_t rawdata, uint32_t reg_id)
{
    return 0;
}

static osal_inline int netplay_lag()
{
    return 0;
}

static osal_inline void netplay_set_controller(uint8_t player)
{
}

static osal_inline int netplay_is_init()
{
    return 0;
}

static osal_inline int netplay_get_controller(uint8_t player)
{
    return 0;
}

static osal_inline file_status_t netplay_read_storage(const char *filename, void *data, size_t size)
{
    return 0;
}

static osal_inline void netplay_sync_settings(uint32_t *count_per_op, uint32_t *count_per_op_denom_pot, uint32_t *disable_extra_mem, int32_t *si_dma_duration, uint32_t *emumode, int32_t *no_compiled_jump)
{
}

static osal_inline void netplay_check_sync(struct cp0* cp0)
{
}

static osal_inline int netplay_next_controller()
{
    return 0;
}

static osal_inline void netplay_read_registration(struct controller_input_compat* cin_compats)
{
}

static osal_inline void netplay_update_input(struct pif* pif)
{
}

static osal_inline void netplay_set_plugin(uint8_t control_id, uint8_t plugin)
{
}

static osal_inline m64p_error netplay_send_config(char* data, int size)
{
    return M64ERR_INCOMPATIBLE;
}

static osal_inline m64p_error netplay_receive_config(char* data, int size)
{
    return M64ERR_INCOMPATIBLE;
}

static osal_inline uint8_t netplay_get_throttle_level(uint8_t player)
{
    return 0;
}

static osal_inline void netplay_reset_throttling(uint8_t player)
{
}

static osal_inline uint8_t netplay_get_buffer_health(uint8_t player)
{
    return 0;
}

static osal_inline uint8_t netplay_get_player_lag(uint8_t player)
{
    return 0;
}

static osal_inline int netplay_has_throttled_players()
{
    return 0;
}

static osal_inline void netplay_get_throttling_stats(uint8_t* throttle_levels, uint8_t* buffer_health, uint8_t* player_lags)
{
}

static osal_inline void netplay_log_throttling_status()
{
}

static osal_inline uint8_t netplay_get_total_throttle_level()
{
    return 0;
}

#endif

#endif

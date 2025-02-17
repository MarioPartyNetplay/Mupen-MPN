/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - cheat.c                                                 *
 *   Mupen64Plus homepage: https://mupen64plus.org/                        *
 *   Copyright (C) 2009 Richard Goedeken                                   *
 *   Copyright (C) 2008 Okaygo                                             *
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

/* gameshark and xploder64 reference: http://doc.kodewerx.net/hacking_n64.html */

#include <SDL.h>
#include <SDL_thread.h>

#define __STDC_FORMAT_MACROS
#include <inttypes.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cheat.h"
#include "eventloop.h"
#include "list.h"

#include "api/callbacks.h"
#include "api/m64p_types.h"
#include "device/r4300/r4300_core.h"
#include "device/rdram/rdram.h"
#include "osal/preproc.h"

/* local definitions */
#define CHEAT_CODE_MAGIC_VALUE UINT32_C(0xDEAD0000)

typedef struct cheat_code {
    uint32_t address;
    uint32_t value;
    uint32_t old_value;
    struct list_head list;
} cheat_code_t;

typedef struct cheat {
    char *name;
    int enabled;
    int was_enabled;
    struct list_head cheat_codes;
    struct list_head list;
} cheat_t;

#define MAX_CONDITION_DEPTH 32

/* Structure to track conditional states */
typedef struct condition_state {
    int active;      /* Whether this condition block is active */
    int else_seen;   /* Whether we've seen an else for this block */
    uint32_t ba;     /* Base address for this condition block */
    uint32_t po;     /* Pointer offset for this condition block */
} condition_state_t;

/* private functions */
/* Helper functions for reading and writing memory - same as before */
static uint32_t read_address_32bit(struct r4300_core* r4300, uint32_t address)
{
    return *(uint32_t*)(((unsigned char*)r4300->rdram->dram + ((address & 0xFFFFFF)^S32)));
}

static uint16_t read_address_16bit(struct r4300_core* r4300, uint32_t address)
{
    return *(uint16_t*)(((unsigned char*)r4300->rdram->dram + ((address & 0xFFFFFF)^S16)));
}

static uint8_t read_address_8bit(struct r4300_core* r4300, uint32_t address)
{
    return *(uint8_t*)(((unsigned char*)r4300->rdram->dram + ((address & 0xFFFFFF)^S8)));
}

static void update_address_32bit(struct r4300_core* r4300, uint32_t address, uint32_t new_value)
{
    *(uint32_t*)(((unsigned char*)r4300->rdram->dram + ((address & 0xFFFFFF)^S32))) = new_value;
    invalidate_r4300_cached_code(r4300, address & 0xFFFFFF, 4);
}

static void update_address_16bit(struct r4300_core* r4300, uint32_t address, uint16_t new_value)
{
    *(uint16_t*)(((unsigned char*)r4300->rdram->dram + ((address & 0xFFFFFF)^S16))) = new_value;
    invalidate_r4300_cached_code(r4300, address & 0xFFFFFF, 2);
}

static void update_address_8bit(struct r4300_core* r4300, uint32_t address, uint8_t new_value)
{
    *(uint8_t*)(((unsigned char*)r4300->rdram->dram + ((address & 0xFFFFFF)^S8))) = new_value;
    invalidate_r4300_cached_code(r4300, address & 0xFFFFFF, 1);
}

/* Value extraction helpers - same as before */
static uint8_t extract_8bit_value(uint32_t value)
{
    return (uint8_t)(value & 0xFF);
}

static uint16_t extract_16bit_value(uint32_t value)
{
    return (uint16_t)(value & 0xFFFF);
}

/* Comparison functions - same as before */
static int check_equal(struct r4300_core* r4300, uint32_t address, uint32_t value, int size)
{
    switch(size) {
        case 1: return read_address_8bit(r4300, address) == extract_8bit_value(value);
        case 2: return read_address_16bit(r4300, address) == extract_16bit_value(value);
        case 4: return read_address_32bit(r4300, address) == value;
        default: return 0;
    }
}

static int check_not_equal(struct r4300_core* r4300, uint32_t address, uint32_t value, int size)
{
    return !check_equal(r4300, address, value, size);
}

static int check_greater(struct r4300_core* r4300, uint32_t address, uint32_t value, int size)
{
    switch(size) {
        case 1: return read_address_8bit(r4300, address) > extract_8bit_value(value);
        case 2: return read_address_16bit(r4300, address) > extract_16bit_value(value);
        case 4: return read_address_32bit(r4300, address) > value;
        default: return 0;
    }
}

static int check_less(struct r4300_core* r4300, uint32_t address, uint32_t value, int size)
{
    switch(size) {
        case 1: return read_address_8bit(r4300, address) < extract_8bit_value(value);
        case 2: return read_address_16bit(r4300, address) < extract_16bit_value(value);
        case 4: return read_address_32bit(r4300, address) < value;
        default: return 0;
    }
}

static int execute_cheat(struct r4300_core* r4300, uint32_t address, uint32_t value, uint32_t* old_value,
                        condition_state_t* conditions, int* condition_depth)
{
    uint8_t code_type = (address >> 24) & 0xFF;
    uint32_t base_address = address & 0xFFFFFF;
    int is_gs_button = (code_type & 0x01) && event_gameshark_active();
    
    /* Handle endif/terminator codes first */
    if (code_type == 0xE0) {
        /* Full terminator - clear all conditions */
        *condition_depth = 0;
        /* Apply any BA/PO modifications */
        if ((value >> 16) & 0xFFFF) {
            conditions[0].ba = ((value >> 16) & 0xFFFF) << 16;
        }
        if (value & 0xFFFF) {
            conditions[0].po = (value & 0xFFFF) << 16;
        }
        return 1;
    }
    
    if (code_type == 0xE2) {
        uint8_t num_endifs = extract_8bit_value(address);
        int invert = (address >> 8) & 0x01;
        
        /* Apply endifs */
        if (num_endifs > *condition_depth) {
            num_endifs = *condition_depth;
        }
        *condition_depth -= num_endifs;
        
        /* Handle else functionality */
        if (invert && *condition_depth > 0) {
            conditions[*condition_depth - 1].active = !conditions[*condition_depth - 1].active;
            conditions[*condition_depth - 1].else_seen = 1;
        }
        
        /* Apply any BA/PO modifications */
        if ((value >> 16) & 0xFFFF) {
            conditions[*condition_depth].ba = ((value >> 16) & 0xFFFF) << 16;
        }
        if (value & 0xFFFF) {
            conditions[*condition_depth].po = (value & 0xFFFF) << 16;
        }
        
        return 1;
    }
    
    /* Check if we're in an inactive condition block */
    for (int i = 0; i < *condition_depth; i++) {
        if (!conditions[i].active) {
            return 1;
        }
    }
    
    /* Handle legacy D0/D1 conditional codes */
    if ((code_type & 0xF0) == 0xD0) {
        int old_style_result = 0;
        switch(code_type) {
            case 0xD0: /* 8-bit equal */
            case 0xD8:
                if ((code_type == 0xD8) && !event_gameshark_active()) {
                    return 0;
                }
                old_style_result = check_equal(r4300, base_address, value, 1);
                break;
            case 0xD1: /* 16-bit equal */
            case 0xD9:
                if ((code_type == 0xD9) && !event_gameshark_active()) {
                    return 0;
                }
                old_style_result = check_equal(r4300, base_address, value, 2);
                break;
        }
        return old_style_result;
    }
    
    /* Handle new style conditional codes */
    if (code_type >= 0x20 && code_type <= 0x36) {
        int condition_result = 0;
        
        /* Evaluate condition */
        switch(code_type) {
            case 0x20: condition_result = check_equal(r4300, base_address, value, 4); break;
            case 0x22: condition_result = check_not_equal(r4300, base_address, value, 4); break;
            case 0x24: condition_result = check_greater(r4300, base_address, value, 4); break;
            case 0x26: condition_result = check_less(r4300, base_address, value, 4); break;
            case 0x28: condition_result = check_equal(r4300, base_address, value, 2); break;
            case 0x2A: condition_result = check_not_equal(r4300, base_address, value, 2); break;
            case 0x2C: condition_result = check_greater(r4300, base_address, value, 2); break;
            case 0x2E: condition_result = check_less(r4300, base_address, value, 2); break;
            case 0x30: condition_result = check_equal(r4300, base_address, value, 1); break;
            case 0x32: condition_result = check_not_equal(r4300, base_address, value, 1); break;
            case 0x34: condition_result = check_greater(r4300, base_address, value, 1); break;
            case 0x36: condition_result = check_less(r4300, base_address, value, 1); break;
        }
        
        /* Push new condition state */
        if (*condition_depth < MAX_CONDITION_DEPTH) {
            conditions[*condition_depth].active = condition_result;
            conditions[*condition_depth].else_seen = 0;
            conditions[*condition_depth].ba = conditions[*condition_depth > 0 ? *condition_depth - 1 : 0].ba;
            conditions[*condition_depth].po = conditions[*condition_depth > 0 ? *condition_depth - 1 : 0].po;
            (*condition_depth)++;
        }
        
        return 1;
    }

    /* Handle legacy code types */
    switch(code_type & 0xF0) {
        case 0x80:
        case 0x81:
        case 0xA0:
        case 0xA1:
        case 0xA8:
        case 0xA9:
            if (old_value && (*old_value == CHEAT_CODE_MAGIC_VALUE)) {
                *old_value = (code_type & 0x01) ? read_address_16bit(r4300, base_address) 
                                               : read_address_8bit(r4300, base_address);
            }
            if ((code_type & 0x28) == 0x28 && !event_gameshark_active()) {
                return 1;
            }
            if (code_type & 0x01) {
                update_address_16bit(r4300, base_address, (uint16_t)value);
            } else {
                update_address_8bit(r4300, base_address, (uint8_t)value);
            }
            return 1;
    }

    /* Handle direct memory writes */
    switch(code_type) {
        case 0x00:  /* 8-bit write */
        case 0x01:  /* 8-bit GS write */
            if (!is_gs_button && (code_type & 0x01)) return 1;
            if (old_value && (*old_value == CHEAT_CODE_MAGIC_VALUE)) {
                *old_value = read_address_8bit(r4300, base_address);
            }
            update_address_8bit(r4300, base_address + conditions[*condition_depth-1].ba,
                              extract_8bit_value(value));
            return 1;

        case 0x02:  /* 16-bit write */
        case 0x03:  /* 16-bit GS write */
            if (!is_gs_button && (code_type & 0x01)) return 1;
            if (old_value && (*old_value == CHEAT_CODE_MAGIC_VALUE)) {
                *old_value = read_address_16bit(r4300, base_address);
            }
            update_address_16bit(r4300, base_address + conditions[*condition_depth-1].ba,
                               extract_16bit_value(value));
            return 1;

        case 0x04:  /* 32-bit write */
        case 0x05:  /* 32-bit GS write */
            if (!is_gs_button && (code_type & 0x01)) return 1;
            if (old_value && (*old_value == CHEAT_CODE_MAGIC_VALUE)) {
                *old_value = read_address_32bit(r4300, base_address);
            }
            update_address_32bit(r4300, base_address + conditions[*condition_depth-1].ba, value);
            return 1;
    }

    return 1;
}

static cheat_t *find_or_create_cheat(struct cheat_ctx* ctx, const char *name)
{
    cheat_t *cheat;
    int found = 0;

    list_for_each_entry_t(cheat, &ctx->active_cheats, cheat_t, list) {
        if (strcmp(cheat->name, name) == 0) {
            found = 1;
            break;
        }
    }

    if (found)
    {
        /* delete any pre-existing cheat codes */
        cheat_code_t *code, *safe;

        list_for_each_entry_safe_t(code, safe, &cheat->cheat_codes, cheat_code_t, list) {
            list_del(&code->list);
            free(code);
        }

        cheat->enabled = 0;
        cheat->was_enabled = 0;
    }
    else
    {
        cheat = malloc(sizeof(*cheat));
        cheat->name = strdup(name);
        cheat->enabled = 0;
        cheat->was_enabled = 0;
        INIT_LIST_HEAD(&cheat->cheat_codes);
        list_add_tail(&cheat->list, &ctx->active_cheats);
    }

    return cheat;
}


/* public functions */
void cheat_init(struct cheat_ctx* ctx)
{
    ctx->mutex = SDL_CreateMutex();
    INIT_LIST_HEAD(&ctx->active_cheats);
}

void cheat_uninit(struct cheat_ctx* ctx)
{
    if (ctx->mutex != NULL) {
        SDL_DestroyMutex(ctx->mutex);
    }
    ctx->mutex = NULL;
}

void cheat_apply_cheats(struct cheat_ctx* ctx, struct r4300_core* r4300, int entry)
{
    cheat_t *cheat;
    cheat_code_t *code;
    int cond_failed;

    if (list_empty(&ctx->active_cheats))
        return;

    if (ctx->mutex == NULL || SDL_LockMutex(ctx->mutex) != 0)
    {
        DebugMessage(M64MSG_ERROR, "Internal error: failed to lock mutex in cheat_apply_cheats()");
        return;
    }

    list_for_each_entry_t(cheat, &ctx->active_cheats, cheat_t, list) {
        if (cheat->enabled)
        {
            cheat->was_enabled = 1;
            switch(entry)
            {
            case ENTRY_BOOT:
                list_for_each_entry_t(code, &cheat->cheat_codes, cheat_code_t, list) {
                    /* code should only be written once at boot time */
                    if ((code->address & 0xF0000000) == 0xF0000000) {
                        execute_cheat(r4300, code->address, code->value, &code->old_value);
                    }
                }
                break;
            case ENTRY_VI:
                /* a cheat starts without failed preconditions */
                cond_failed = 0;

                list_for_each_entry_t(code, &cheat->cheat_codes, cheat_code_t, list) {
                    /* conditional cheat codes */
                    if ((code->address & 0xF0000000) == 0xD0000000)
                    {
                        /* if code needs GS button pressed and it's not, skip it */
                        if (((code->address & 0xFF000000) == 0xD8000000 ||
                                    (code->address & 0xFF000000) == 0xD9000000 ||
                                    (code->address & 0xFF000000) == 0xDA000000 ||
                                    (code->address & 0xFF000000) == 0xDB000000) &&
                                !event_gameshark_active()) {
                            /* if condition false, skip next code non-test code */
                            cond_failed = 1;
                        }

                        /* if condition false, skip next code non-test code */
                        if (!execute_cheat(r4300, code->address, code->value, NULL)) {
                            cond_failed = 1;
                        }
                    }
                    else {
                        /* preconditions were false for this non-test code
                         * reset the condition state and skip the cheat
                         */
                        if (cond_failed) {
                            cond_failed = 0;
                            continue;
                        }

                        switch (code->address & 0xFF000000) {
                        /* GS button triggers cheat code */
                        case 0x88000000:
                        case 0x89000000:
                        case 0xA8000000:
                        case 0xA9000000:
                            if (event_gameshark_active()) {
                                execute_cheat(r4300, code->address, code->value, NULL);
                            }
                            break;
                            /* normal cheat code */
                        default:
                            /* exclude boot-time cheat codes */
                            if ((code->address & 0xF0000000) != 0xF0000000) {
                                execute_cheat(r4300, code->address, code->value, &code->old_value);
                            }
                            break;
                        }
                    }
                }
                break;
            default:
                break;
            }
        }
        /* if cheat was enabled, but is now disabled, restore old memory values */
        else if (cheat->was_enabled)
        {
            cheat->was_enabled = 0;
            switch(entry)
            {
            case ENTRY_VI:
                list_for_each_entry_t(code, &cheat->cheat_codes, cheat_code_t, list) {
                    /* set memory back to old value and clear saved copy of old value */
                    if(code->old_value != CHEAT_CODE_MAGIC_VALUE)
                    {
                        execute_cheat(r4300, code->address, code->old_value, NULL);
                        code->old_value = CHEAT_CODE_MAGIC_VALUE;
                    }
                }
                break;
            default:
                break;
            }
        }
    }

    SDL_UnlockMutex(ctx->mutex);
}


void cheat_delete_all(struct cheat_ctx* ctx)
{
    cheat_t *cheat, *safe_cheat;
    cheat_code_t *code, *safe_code;

    if (list_empty(&ctx->active_cheats))
        return;

    if (ctx->mutex == NULL || SDL_LockMutex(ctx->mutex) != 0)
    {
        DebugMessage(M64MSG_ERROR, "Internal error: failed to lock mutex in cheat_delete_all()");
        return;
    }

    list_for_each_entry_safe_t(cheat, safe_cheat, &ctx->active_cheats, cheat_t, list) {
        free(cheat->name);

        list_for_each_entry_safe_t(code, safe_code, &cheat->cheat_codes, cheat_code_t, list) {
            list_del(&code->list);
            free(code);
        }
        list_del(&cheat->list);
        free(cheat);
    }

    SDL_UnlockMutex(ctx->mutex);
}

int cheat_set_enabled(struct cheat_ctx* ctx, const char* name, int enabled)
{
    cheat_t *cheat = NULL;

    if (list_empty(&ctx->active_cheats))
        return 0;

    if (ctx->mutex == NULL || SDL_LockMutex(ctx->mutex) != 0)
    {
        DebugMessage(M64MSG_ERROR, "Internal error: failed to lock mutex in cheat_set_enabled()");
        return 0;
    }

    list_for_each_entry_t(cheat, &ctx->active_cheats, cheat_t, list) {
        if (strcmp(name, cheat->name) == 0)
        {
            cheat->enabled = enabled;
            SDL_UnlockMutex(ctx->mutex);
            return 1;
        }
    }

    SDL_UnlockMutex(ctx->mutex);
    return 0;
}

int cheat_add_new(struct cheat_ctx* ctx, const char* name, m64p_cheat_code* code_list, int num_codes)
{
    cheat_t *cheat;
    int i, j;

    if (ctx->mutex == NULL || SDL_LockMutex(ctx->mutex) != 0)
    {
        DebugMessage(M64MSG_ERROR, "Internal error: failed to lock mutex in cheat_add_new()");
        return 0;
    }

    /* create a new cheat function or erase the codes in an existing cheat function */
    cheat = find_or_create_cheat(ctx, name);
    if (cheat == NULL)
    {
        SDL_UnlockMutex(ctx->mutex);
        return 0;
    }

    /* default for new cheats is enabled */
    cheat->enabled = 1;

    for (i = 0; i < num_codes; i++)
    {
        /* if this is a 'patch' code, convert it and dump out all of the individual codes */
        if ((code_list[i].address & 0xFFFF0000) == 0x50000000 && i < num_codes - 1)
        {
            int code_count = ((code_list[i].address & 0xFF00) >> 8);
            int incr_addr = code_list[i].address & 0xFF;
            int incr_value = code_list[i].value;
            int cur_addr = code_list[i+1].address;
            int cur_value = code_list[i+1].value;
            i += 1;
            for (j = 0; j < code_count; j++)
            {
                cheat_code_t *code = malloc(sizeof(*code));
                code->address = cur_addr;
                code->value = cur_value;
                code->old_value = CHEAT_CODE_MAGIC_VALUE;
                list_add_tail(&code->list, &cheat->cheat_codes);
                cur_addr += incr_addr;
                cur_value += incr_value;
            }
        }
        else
        {
            /* just a normal code */
            cheat_code_t *code = malloc(sizeof(*code));
            code->address = code_list[i].address;
            code->value = code_list[i].value;
            code->old_value = CHEAT_CODE_MAGIC_VALUE;
            list_add_tail(&code->list, &cheat->cheat_codes);
        }
    }

    SDL_UnlockMutex(ctx->mutex);
    return 1;
}

static char *strtok_compat(char *str, const char *delim, char **saveptr)
{
    char *p;

    if (str == NULL)
        str = *saveptr;

    if (str == NULL)
        return NULL;

    str += strspn(str, delim);
    if ((p = strpbrk(str, delim)) != NULL) {
        *saveptr = p + 1;
        *p = '\0';
    } else {
        *saveptr = NULL;
    }
    return str;
}

static int cheat_parse_hacks_code(char *code, m64p_cheat_code **hack)
{
    char *saveptr = NULL;
    char *input, *token;
    int num_codes;
    m64p_cheat_code *hackbuf;
    int ret;

    *hack = NULL;

    /* count number of possible cheatcodes */
    input = code;
    num_codes = 0;
    while ((strchr(input, ','))) {
        input++;
        num_codes++;
    }
    num_codes++;

    /* allocate buffer */
    hackbuf = malloc(sizeof(*hackbuf) * num_codes);
    if (!hackbuf)
        return 0;

    /* parse cheatcodes */
    input = code;
    num_codes = 0;
    while ((token = strtok_compat(input, ",", &saveptr))) {
        input = NULL;

        ret = sscanf(token, "%08" SCNx32 " %04X", &hackbuf[num_codes].address,
                (unsigned int*)&hackbuf[num_codes].value);
        if (ret == 2)
            num_codes++;
    }

    if (num_codes == 0)
        free(hackbuf);
    else
        *hack = hackbuf;

    return num_codes;
}

int cheat_add_hacks(struct cheat_ctx* ctx, const char* rom_cheats)
{
    char *cheat_raw = NULL;
    char *saveptr = NULL;
    char *input, *token;
    unsigned int i = 0;
    int num_codes;
    char cheatname[32];
    m64p_cheat_code *hack;

    if (!rom_cheats)
        return 0;

    /* copy ini entry for tokenizing */
    cheat_raw = strdup(rom_cheats);
    if (!cheat_raw)
        goto out;

    /* split into cheats for the cheat engine */
    input = cheat_raw;
    while ((token = strtok_compat(input, ";", &saveptr))) {
        input = NULL;

        snprintf(cheatname, sizeof(cheatname), "HACK%u", i);
        cheatname[sizeof(cheatname) - 1] = '\0';

        /* parse and add cheat */
        num_codes = cheat_parse_hacks_code(token, &hack);
        if (num_codes <= 0)
            continue;

        cheat_add_new(ctx, cheatname, hack, num_codes);
        free(hack);
        i++;
    }

out:
    free(cheat_raw);
    return 0;
}
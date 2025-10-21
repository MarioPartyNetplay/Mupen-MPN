/*
 * Circular Buffer Implementation
 * Simple circular buffer implementation for audio processing
 */

#include "circular_buffer.hpp"
#include <stdlib.h>
#include <string.h>

void init_cbuff(struct circular_buffer* cbuff, size_t size)
{
    cbuff->data = malloc(size);
    cbuff->size = size;
    cbuff->head = 0;
    cbuff->tail = 0;
}

void release_cbuff(struct circular_buffer* cbuff)
{
    if (cbuff->data != nullptr)
    {
        free(cbuff->data);
        cbuff->data = nullptr;
    }
    cbuff->size = 0;
    cbuff->head = 0;
    cbuff->tail = 0;
}

void* cbuff_head(struct circular_buffer* cbuff, size_t* available)
{
    size_t used = (cbuff->head - cbuff->tail) % cbuff->size;
    *available = cbuff->size - used - 1; // Leave one byte free to distinguish full from empty
    
    if (*available == 0)
        return nullptr;
    
    return (char*)cbuff->data + cbuff->head;
}

const void* cbuff_tail(struct circular_buffer* cbuff, size_t* available)
{
    size_t used = (cbuff->head - cbuff->tail) % cbuff->size;
    *available = used;
    
    if (*available == 0)
        return nullptr;
    
    return (char*)cbuff->data + cbuff->tail;
}

void produce_cbuff_data(struct circular_buffer* cbuff, size_t size)
{
    cbuff->head = (cbuff->head + size) % cbuff->size;
}

void consume_cbuff_data(struct circular_buffer* cbuff, size_t size)
{
    cbuff->tail = (cbuff->tail + size) % cbuff->size;
}

/*
 * Circular Buffer Header
 * Simple circular buffer implementation for audio processing
 */

#ifndef CIRCULAR_BUFFER_HPP
#define CIRCULAR_BUFFER_HPP

#include <stddef.h>

struct circular_buffer
{
    void* data;
    size_t size;
    size_t head;
    size_t tail;
};

void init_cbuff(struct circular_buffer* cbuff, size_t size);
void release_cbuff(struct circular_buffer* cbuff);
void* cbuff_head(struct circular_buffer* cbuff, size_t* available);
const void* cbuff_tail(struct circular_buffer* cbuff, size_t* available);
void produce_cbuff_data(struct circular_buffer* cbuff, size_t size);
void consume_cbuff_data(struct circular_buffer* cbuff, size_t size);

#endif // CIRCULAR_BUFFER_HPP

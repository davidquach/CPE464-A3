#ifndef BUFFER_H
#define BUFFER_H

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "pdu.h"
#include <stdlib.h>


struct buffer {
    int seq_num; // Sequence Number
    int valid; // Valid flag
    uint8_t packet[1407]; 
};

struct window {
    uint32_t upper;
    uint32_t current;
    uint32_t lower;
    int size;
    struct buffer* window_buffer;
};

int window_isvalid(struct window* input_window, uint32_t seq_num);

// Checks if window is full
int window_full(struct window* input_window);

// Creates server buffer based off window size input
void window_create(struct window* input_window, int window_size);

// Updates lower and upper to match recent RR
void window_slide(struct window* input_window, uint32_t rr_num);

// Increments current in window
void window_CURUpdate(struct window* input_window);

// Add packet to window
void window_add(struct window* input_window, uint32_t seq_num, uint8_t* packet, int32_t packet_len);

// Removes packet after RR
void window_remove(struct window* input_window, uint32_t seq_num);

// Prints window structure 
void window_print(struct window* input_window);

uint8_t* window_get_lower(struct window* input_window);

uint8_t* window_get_packet(struct window* input_window, uint32_t seq_num);

void window_print_test(struct window* input_window, uint32_t data_len, uint32_t eof_len, uint32_t eof_seq);

#endif // BUFFER_H

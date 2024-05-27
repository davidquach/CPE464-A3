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

int window_isvalid(struct window* input_window, uint32_t seq_num) {
    uint32_t index = (seq_num) % input_window->size;
    return input_window->window_buffer[index].valid;
}

uint8_t* window_get_lower(struct window* input_window) {
    uint32_t index = (input_window->lower) % input_window->size;
    return input_window->window_buffer[index].packet;
}

uint8_t* window_get_packet(struct window* input_window, uint32_t seq_num) {
    uint32_t index = (seq_num) % input_window->size;
    return input_window->window_buffer[index].packet;
}

int window_full(struct window* input_window) {
    return (input_window->current == input_window->upper);
}

// Creates server buffer based off window size input
void window_create(struct window* input_window, int window_size) {
    input_window->lower = 1;
    input_window->current = 1;
    input_window->upper = input_window->current + window_size;
    input_window->size = window_size;
    input_window->window_buffer = calloc(window_size, (size_t)sizeof(struct buffer));
    
    if (input_window->window_buffer== NULL) {
        printf("Error: Unable to allocate space for buffer.\n");
        exit(1);
    }

}

// Updates lower and upper to match recent RR
void window_slide(struct window* input_window, uint32_t rr_num) {
    input_window->lower = rr_num;
    input_window->upper = input_window->lower + input_window->size;
}


// Increments current in window
void window_CURUpdate(struct window* input_window) {
    input_window->current++;
}


// Add packet to window
void window_add(struct window* input_window, uint32_t seq_num, uint8_t* packet, int32_t packet_len) {
    uint32_t index = (seq_num) % input_window->size;
    memcpy(input_window->window_buffer[index].packet, packet, packet_len);
    input_window->window_buffer[index].seq_num = seq_num;
    input_window->window_buffer[index].valid = 1;

    // printPacket(input_window->window_buffer[index].packet, packet_len);

}


// Removes packet after RR
void window_remove(struct window* input_window, uint32_t seq_num) {
    uint32_t index = (seq_num) % input_window->size;
    input_window->window_buffer[index].valid = 0;
}


// Prints window structure 
void window_print(struct window* input_window) {
    printf("\n\nsize: %d, lower: %d, current: %d, upper: %d\n\n", input_window->size, input_window->lower, input_window->current, input_window->upper);
    // printf("\n            ");
    // for (int i = 1; i < 13 ; i++) {
    //     printf("%d       ", i);
    // }
    
    // printf("\nCurrent     ");
    // for (int i = 1; i < input_window->current; i++) {
    //     printf("        ");
    // }
    // printf("|");
    // printf("\n");

    // printf("\nLower       ");
    // for (int i = 1; i < input_window->lower; i++) {
    //     printf("        ");
    // }
    // printf("|");
    // printf("\n");

    // printf("\nUpper       ");
    // for (int i = 1; i < input_window->upper; i++) {
    //     printf("        ");
    // }
    // printf("|");
    // printf("\n\n");

    for (int i = 0; i < input_window->size; i++) 
    {
        if (input_window->window_buffer[i].valid == 1)
        {
            printf("Index %d:", i);
            printPacket(input_window->window_buffer[i].packet, 12);
            printf("\n");
        }
    }
}

void window_print_test(struct window* input_window, uint32_t data_len, uint32_t eof_len, uint32_t eof_seq) {
    for (int i = 0; i < input_window->size; i++) 
    {
        if (input_window->window_buffer[i].valid == 1)
        {
            printf("Index %d:", i);
            
            if (i == 7) {
                printPacket(input_window->window_buffer[i].packet, eof_seq);

            }
            else {
                printPacket(input_window->window_buffer[i].packet, data_len);
            }

            printf("\n");
        }
    }
}




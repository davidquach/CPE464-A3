#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

struct buffer_entry {
    int seq_num; // Sequence Number
    int valid; // Valid flag
    uint8_t buffer[1400];
};


// Creates server buffer based off window size input
struct buffer_entry* buffer_create(int window_size) {
    struct sequence* server_buffer = calloc(window_size, (size_t)sizeof(struct buffer_entry));
    
    if (server_buffer == NULL) {
        printf("Error: Unable to allocate space for buffer.\n");
        return NULL;
    }
    
    return server_buffer;
}

// Add new element to server buffer
void buffer_add(struct buffer_entry* server_buffer, int window_size, int seq_num, uint8_t* buffer) {
    int index = seq_num % window_size;
    server_buffer[index].seq_num = seq_num;
    server_buffer[index].valid = 1;
}

void buffer_remove(struct buffer_entry* server_buffer, int window_size, int seq_num) {
    int index = seq_num % window_size;
    server_buffer[index].valid = 0; 
}

void buffer_free(struct buffer_entry* server_buffer) {
    free(server_buffer);
}

void buffer_isValid(struct buffer_entry* server_buffer, int window_size, int seq_num) {
    int index = seq_num % window_size;
    return server_buffer[index].valid;
}





#include "memory.h"

#include <stdlib.h>

unsigned char *memory_new(size_t size) { return malloc(size); }

void memory_store(unsigned char *buffer, size_t index, unsigned char value) {
  buffer[index] = value;
}

unsigned char memory_load(const unsigned char *buffer, size_t index) {
  return buffer[index];
}

void memory_free(unsigned char *buffer) { free(buffer); }

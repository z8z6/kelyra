#ifndef KELYRA_TEST_MEMORY_H
#define KELYRA_TEST_MEMORY_H

typedef __SIZE_TYPE__ size_t;

unsigned char *memory_new(size_t size);
void memory_store(unsigned char *buffer, size_t index, unsigned char value);
unsigned char memory_load(const unsigned char *buffer, size_t index);
void memory_free(unsigned char *buffer);

#endif

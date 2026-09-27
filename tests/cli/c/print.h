#ifndef KELYRA_TEST_PRINT_H
#define KELYRA_TEST_PRINT_H

int print(void);

typedef struct Pair {
  int left;
  int right;
} Pair;

#include "records.h"

Pair make_pair(int left, int right);
int sum_pair(Pair pair);
Pair *pair_pointer(void);
int sum_pair_pointer(const Pair *pair);
int sum_many(int count, ...);
int print_format(const char *format, ...);
float float_add(float left, float right);
double double_add(double left, double right);
Packet make_packet(void);
int inspect_packet(Packet packet);
Status status_value(void);
Anon make_anon(void);

#endif

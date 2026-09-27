#ifndef KELYRA_TEST_RECORDS_H
#define KELYRA_TEST_RECORDS_H

typedef enum Status {
  STATUS_OK = 0,
  STATUS_FAILURE = -7
} Status;

typedef union Number {
  int integer;
  float fractional;
} Number;

typedef struct Packet {
  Pair pair;
  Number number;
  unsigned char bytes[3];
  struct {
    int extra;
  };
  unsigned flags : 2;
} Packet;

typedef struct {
  int value;
} Anon;

#endif

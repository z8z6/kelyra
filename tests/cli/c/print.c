#include "print.h"

#include <stdarg.h>
#include <stdio.h>

int print(void) {
  puts("hello from C");
  return 0;
}

Pair make_pair(int left, int right) { return (Pair){left, right}; }

int sum_pair(Pair pair) { return pair.left + pair.right; }

Pair *pair_pointer(void) {
  static Pair pair = {20, 22};
  return &pair;
}

int sum_pair_pointer(const Pair *pair) { return pair->left + pair->right; }

int sum_many(int count, ...) {
  va_list args;
  va_start(args, count);
  int result = 0;
  for (int i = 0; i < count; ++i)
    result += va_arg(args, int);
  va_end(args);
  return result;
}

int print_format(const char *format, ...) {
  va_list args;
  va_start(args, format);
  int result = vprintf(format, args);
  va_end(args);
  return result;
}

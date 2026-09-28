#include "trace.h"

static long long events;
void trace(int event) { events = events * 10 + event; }
void trace_reset(void) { events = 0; }
long long trace_value(void) { return events; }

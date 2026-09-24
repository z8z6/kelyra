static int increment(int value) { return value + 1; }

int (*get_callback(void))(int) { return increment; }

int apply_callback(int (*callback)(int), int value) { return callback(value); }

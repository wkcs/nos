/* Copyright (C) 2026 NOS contributors */
int nos_dynamic_bias = 17;
int nos_dynamic_initialized;
__attribute__((constructor)) static void initialize(void)
{ nos_dynamic_bias = 29; nos_dynamic_initialized = 1; }
int nos_dynamic_add(int value) { return value + nos_dynamic_bias; }

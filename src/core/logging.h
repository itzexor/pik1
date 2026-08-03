#pragma once

#include <stdbool.h>

void pik_log_set_timestamps(bool enabled);
void pik_log(const char *tag, const char *fmt, ...);
_Noreturn void pik_die(const char *tag, const char *fmt, ...);

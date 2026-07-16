#include "logging.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int failures;

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #expr); \
        failures++; \
    } \
} while (0)

static int timestamp_prefix(const char *s) {
    return isdigit((unsigned char)s[0]) &&
           isdigit((unsigned char)s[1]) &&
           isdigit((unsigned char)s[2]) &&
           isdigit((unsigned char)s[3]) &&
           s[4] == '-' &&
           isdigit((unsigned char)s[5]) &&
           isdigit((unsigned char)s[6]) &&
           s[7] == '-' &&
           isdigit((unsigned char)s[8]) &&
           isdigit((unsigned char)s[9]) &&
           s[10] == ' ' &&
           isdigit((unsigned char)s[11]) &&
           isdigit((unsigned char)s[12]) &&
           s[13] == ':' &&
           isdigit((unsigned char)s[14]) &&
           isdigit((unsigned char)s[15]) &&
           s[16] == ':' &&
           isdigit((unsigned char)s[17]) &&
           isdigit((unsigned char)s[18]) &&
           s[19] == ' ';
}

static void read_stderr_line(void (*fn)(void), char *buf, size_t cap) {
    FILE *tmp = tmpfile();
    int saved = dup(STDERR_FILENO);
    CHECK(tmp != NULL);
    CHECK(saved >= 0);
    CHECK(dup2(fileno(tmp), STDERR_FILENO) >= 0);

    fn();
    fflush(stderr);

    CHECK(dup2(saved, STDERR_FILENO) >= 0);
    close(saved);

    rewind(tmp);
    CHECK(fgets(buf, (int)cap, tmp) != NULL);
    fclose(tmp);
}

static void log_plain(void) {
    pik_log_set_timestamps(false);
    pik_log("test", "plain %d", 7);
}

static void log_timestamped(void) {
    pik_log_set_timestamps(true);
    pik_log("test", "stamped");
    pik_log_set_timestamps(false);
}

int main(void) {
    char line[256];

    read_stderr_line(log_plain, line, sizeof(line));
    CHECK(strcmp(line, "[test] plain 7\n") == 0);

    read_stderr_line(log_timestamped, line, sizeof(line));
    CHECK(timestamp_prefix(line));
    CHECK(strstr(line, "[test] stamped\n") != NULL);

    if (failures) {
        fprintf(stderr, "test_logging: %d failure(s)\n", failures);
        return 1;
    }
    puts("test_logging: ok");
    return 0;
}

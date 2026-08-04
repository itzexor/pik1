#include "control.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #expr); \
        failures++; \
    } \
} while (0)

static void test_action_names(void) {
    CHECK(strcmp(pik_control_action_name(PIK_CONTROL_ACTION_RESTART_PEER), "restart-peer") == 0);
    CHECK(strcmp(pik_control_action_name(PIK_CONTROL_ACTION_REBOOT_PEER), "reboot-peer") == 0);
    CHECK(strcmp(pik_control_action_name(PIK_CONTROL_ACTION_POWEROFF_PEER), "poweroff-peer") == 0);
    CHECK(strcmp(pik_control_action_name(PIK_CONTROL_ACTION_STATUS), "status") == 0);
    CHECK(strcmp(pik_control_action_name(PIK_CONTROL_ACTION_WIFI_RESET_PEER), "wifi-reset-peer") == 0);
    CHECK(strcmp(pik_control_action_name((pik_control_action_t)99), "unknown") == 0);
}

static void test_ack_names(void) {
    CHECK(strcmp(pik_control_ack_status_name(PIK_CONTROL_ACK_OK), "ok") == 0);
    CHECK(strcmp(pik_control_ack_status_name(PIK_CONTROL_ACK_UNKNOWN_ACTION), "unknown-action") == 0);
    CHECK(strcmp(pik_control_ack_status_name(PIK_CONTROL_ACK_INTERNAL_ERROR), "internal-error") == 0);
    CHECK(strcmp(pik_control_ack_status_name((pik_control_ack_status_t)99), "unknown-status") == 0);
}

static void test_public_enum_values(void) {
    CHECK(PIK_CONTROL_ROLE_PTY == 1);
    CHECK(PIK_CONTROL_ROLE_MCU == 2);
    CHECK(PIK_CONTROL_TCP_NONE == 0);
    CHECK(PIK_CONTROL_TCP_LISTEN == 1);
    CHECK(PIK_CONTROL_TCP_FORWARD == 2);
    CHECK(PIK_CONTROL_ACTION_WIFI_RESET_PEER == 5);
    CHECK(PIK_CONTROL_SERVICE_SERIAL == (1u << 0));
    CHECK(PIK_CONTROL_SERVICE_TUNNEL == (1u << 1));
}

int main(void) {
    test_action_names();
    test_ack_names();
    test_public_enum_values();

    if (failures) {
        fprintf(stderr, "test_control_names: %d failure(s)\n", failures);
        return 1;
    }
    puts("test_control_names: ok");
    return 0;
}

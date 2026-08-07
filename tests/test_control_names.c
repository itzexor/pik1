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
    pik_control_action_t action = 0;

    CHECK(strcmp(pik_control_action_name(PIK_CONTROL_ACTION_RESTART_PIK1), "restart-pik1") == 0);
    CHECK(strcmp(pik_control_action_name(PIK_CONTROL_ACTION_REBOOT), "reboot") == 0);
    CHECK(strcmp(pik_control_action_name(PIK_CONTROL_ACTION_POWEROFF), "poweroff") == 0);
    CHECK(strcmp(pik_control_action_name(PIK_CONTROL_ACTION_STATUS), "status") == 0);
    CHECK(strcmp(pik_control_action_name(PIK_CONTROL_ACTION_RESTART_WIFI), "restart-wifi") == 0);
    CHECK(strcmp(pik_control_action_name(PIK_CONTROL_ACTION_RESTART_KLIPPER), "restart-klipper") == 0);
    CHECK(strcmp(pik_control_action_name((pik_control_action_t)99), "unknown") == 0);

    CHECK(pik_control_parse_action("status", &action));
    CHECK(action == PIK_CONTROL_ACTION_STATUS);
    CHECK(pik_control_parse_action("restart-pik1", &action));
    CHECK(action == PIK_CONTROL_ACTION_RESTART_PIK1);
    CHECK(pik_control_parse_action("restart-wifi", &action));
    CHECK(action == PIK_CONTROL_ACTION_RESTART_WIFI);
    CHECK(pik_control_parse_action("restart-klipper", &action));
    CHECK(action == PIK_CONTROL_ACTION_RESTART_KLIPPER);
    CHECK(!pik_control_parse_action("bogus", &action));
    CHECK(!pik_control_parse_action(NULL, &action));

    CHECK(pik_control_action_valid(PIK_CONTROL_ACTION_RESTART_PIK1));
    CHECK(pik_control_action_valid(PIK_CONTROL_ACTION_RESTART_KLIPPER));
    CHECK(!pik_control_action_valid((pik_control_action_t)0));
    CHECK(!pik_control_action_valid((pik_control_action_t)99));
}

static void test_ack_names(void) {
    CHECK(strcmp(pik_control_ack_status_name(PIK_CONTROL_ACK_OK), "ok") == 0);
    CHECK(strcmp(pik_control_ack_status_name(PIK_CONTROL_ACK_UNKNOWN_ACTION), "unknown-action") == 0);
    CHECK(strcmp(pik_control_ack_status_name(PIK_CONTROL_ACK_INTERNAL_ERROR), "internal-error") == 0);
    CHECK(strcmp(pik_control_ack_status_name((pik_control_ack_status_t)99), "unknown-status") == 0);

    CHECK(pik_control_ack_status_valid(PIK_CONTROL_ACK_OK));
    CHECK(pik_control_ack_status_valid(PIK_CONTROL_ACK_INTERNAL_ERROR));
    CHECK(!pik_control_ack_status_valid((pik_control_ack_status_t)3));
}

static void test_tcp_role_names(void) {
    CHECK(strcmp(pik_control_tcp_role_name(PIK_CONTROL_TCP_NONE), "none") == 0);
    CHECK(strcmp(pik_control_tcp_role_name(PIK_CONTROL_TCP_LISTEN), "listen") == 0);
    CHECK(strcmp(pik_control_tcp_role_name(PIK_CONTROL_TCP_FORWARD), "forward") == 0);
    CHECK(strcmp(pik_control_tcp_role_name((pik_control_tcp_role_t)9), "unknown") == 0);

    CHECK(pik_control_tcp_role_valid(PIK_CONTROL_TCP_NONE));
    CHECK(pik_control_tcp_role_valid(PIK_CONTROL_TCP_FORWARD));
    CHECK(!pik_control_tcp_role_valid((pik_control_tcp_role_t)3));
}

static void test_service_names(void) {
    char buf[PIK_CONTROL_SERVICE_NAMES_MAX];

    pik_control_service_names(0, buf, sizeof(buf));
    CHECK(strcmp(buf, "none") == 0);
    pik_control_service_names(PIK_CONTROL_SERVICE_SERIAL, buf, sizeof(buf));
    CHECK(strcmp(buf, "serial") == 0);
    pik_control_service_names(PIK_CONTROL_SERVICE_TUNNEL, buf, sizeof(buf));
    CHECK(strcmp(buf, "tunnel") == 0);
    pik_control_service_names(PIK_CONTROL_SERVICE_SERIAL | PIK_CONTROL_SERVICE_TUNNEL,
                              buf, sizeof(buf));
    CHECK(strcmp(buf, "serial,tunnel") == 0);
    /* Unknown bits name no service. */
    pik_control_service_names(1u << 7, buf, sizeof(buf));
    CHECK(strcmp(buf, "none") == 0);

    /* A short buffer truncates at an entry boundary and stays terminated. */
    char small[8];
    pik_control_service_names(PIK_CONTROL_SERVICE_SERIAL | PIK_CONTROL_SERVICE_TUNNEL,
                              small, sizeof(small));
    CHECK(strcmp(small, "serial") == 0);
    char tiny[3];
    pik_control_service_names(PIK_CONTROL_SERVICE_SERIAL, tiny, sizeof(tiny));
    CHECK(strcmp(tiny, "no") == 0);
}

static void test_public_enum_values(void) {
    CHECK(PIK_CONTROL_ROLE_PTY == 1);
    CHECK(PIK_CONTROL_ROLE_MCU == 2);
    CHECK(PIK_CONTROL_TCP_NONE == 0);
    CHECK(PIK_CONTROL_TCP_LISTEN == 1);
    CHECK(PIK_CONTROL_TCP_FORWARD == 2);
    CHECK(PIK_CONTROL_ACTION_RESTART_WIFI == 5);
    CHECK(PIK_CONTROL_ACTION_RESTART_KLIPPER == 6);
    CHECK(PIK_CONTROL_SERVICE_SERIAL == (1u << 0));
    CHECK(PIK_CONTROL_SERVICE_TUNNEL == (1u << 1));
}

int main(void) {
    test_action_names();
    test_ack_names();
    test_tcp_role_names();
    test_service_names();
    test_public_enum_values();

    if (failures) {
        fprintf(stderr, "test_control_names: %d failure(s)\n", failures);
        return 1;
    }
    puts("test_control_names: ok");
    return 0;
}

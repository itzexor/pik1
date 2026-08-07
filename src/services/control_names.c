// Control vocabulary: names, parsing and validity for the control enums.
//
// Kept apart from control.c so the tests that stub the control service still
// share these tables instead of restating the wire names.

#include "control.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct {
    uint32_t value;
    const char *name;
} vocab_t;

#define VOCAB_ENTRY(sym, value, name) { (uint32_t)(sym), (name) },
#define VOCAB(table) (table), (sizeof(table) / sizeof((table)[0]))

static const vocab_t action_vocab[] = {
    PIK_CONTROL_ACTION_LIST(VOCAB_ENTRY)
};

static const vocab_t ack_vocab[] = {
    PIK_CONTROL_ACK_LIST(VOCAB_ENTRY)
};

static const vocab_t tcp_vocab[] = {
    PIK_CONTROL_TCP_LIST(VOCAB_ENTRY)
};

static const vocab_t service_vocab[] = {
    PIK_CONTROL_SERVICE_LIST(VOCAB_ENTRY)
};

/* NULL when the value is not in the table; callers supply their own
 * placeholder for log output. */
static const char *vocab_name(const vocab_t *v, size_t n, uint32_t value) {
    for (size_t i = 0; i < n; i++)
        if (v[i].value == value)
            return v[i].name;
    return NULL;
}

static bool vocab_parse(const vocab_t *v, size_t n, const char *name,
                        uint32_t *value) {
    if (!name) return false;
    for (size_t i = 0; i < n; i++) {
        if (strcmp(name, v[i].name) == 0) {
            *value = v[i].value;
            return true;
        }
    }
    return false;
}

const char *pik_control_action_name(pik_control_action_t action) {
    const char *name = vocab_name(VOCAB(action_vocab), (uint32_t)action);
    return name ? name : "unknown";
}

bool pik_control_parse_action(const char *name, pik_control_action_t *action) {
    uint32_t value;
    if (!action || !vocab_parse(VOCAB(action_vocab), name, &value))
        return false;
    *action = (pik_control_action_t)value;
    return true;
}

bool pik_control_action_valid(pik_control_action_t action) {
    return vocab_name(VOCAB(action_vocab), (uint32_t)action) != NULL;
}

const char *pik_control_ack_status_name(pik_control_ack_status_t status) {
    const char *name = vocab_name(VOCAB(ack_vocab), (uint32_t)status);
    return name ? name : "unknown-status";
}

bool pik_control_ack_status_valid(pik_control_ack_status_t status) {
    return vocab_name(VOCAB(ack_vocab), (uint32_t)status) != NULL;
}

const char *pik_control_tcp_role_name(pik_control_tcp_role_t role) {
    const char *name = vocab_name(VOCAB(tcp_vocab), (uint32_t)role);
    return name ? name : "unknown";
}

bool pik_control_tcp_role_valid(pik_control_tcp_role_t role) {
    return vocab_name(VOCAB(tcp_vocab), (uint32_t)role) != NULL;
}

void pik_control_service_names(uint32_t flags, char *buf, size_t cap) {
    if (!buf || cap == 0) return;

    size_t len = 0;
    buf[0] = '\0';
    for (size_t i = 0; i < sizeof(service_vocab) / sizeof(service_vocab[0]); i++) {
        if (!(flags & service_vocab[i].value))
            continue;
        size_t name_len = strlen(service_vocab[i].name);
        size_t need = name_len + (len ? 1u : 0u);
        if (len + need + 1u > cap)
            break;
        if (len)
            buf[len++] = ',';
        memcpy(buf + len, service_vocab[i].name, name_len);
        len += name_len;
        buf[len] = '\0';
    }
    if (len)
        return;

    static const char none[] = "none";
    size_t n = cap - 1u < sizeof(none) - 1u ? cap - 1u : sizeof(none) - 1u;
    memcpy(buf, none, n);
    buf[n] = '\0';
}

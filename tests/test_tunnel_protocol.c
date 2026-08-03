#include "test_session_harness.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

static int failures;

/* pick a free localhost TCP port */
static int free_port(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in sa = { .sin_family = AF_INET };
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t sl = sizeof(sa);
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 ||
        getsockname(fd, (struct sockaddr *)&sa, &sl) < 0) {
        close(fd);
        return -1;
    }
    int port = ntohs(sa.sin_port);
    close(fd);
    return port;
}

static bool fixture_init_mode(session_fixture_t *fx, tunnel_mode_t mode, int port) {
    session_fixture_cfg_t cfg = {
        .role = mode == TUNNEL_MODE_LISTEN ? PIK_CONTROL_ROLE_MCU
                                           : PIK_CONTROL_ROLE_PTY,
        .tunnel_mode = mode,
        .tunnel_host = "127.0.0.1",
        .tunnel_port = port,
    };
    if (!sfx_init(fx, &cfg)) return false;
    /* the fixture's peer HELLO uses the opposite role */
    return true;
}

static bool handshake_as(session_fixture_t *fx, pik_control_role_t peer_role) {
    sfx_frame_t f;
    if (!sfx_read_frame(fx, &f)) return false;
    if (f.type != PIK_FRAME_CTRL_HELLO) return false;
    if (!sfx_send_peer_hello(fx, peer_role, PIK1_PROTOCOL_VERSION)) return false;
    if (!sfx_dispatch_one(fx, 1000)) return false;
    if (!pik_control_ready()) return false;
    if (!sfx_read_frame(fx, &f) || f.type != PIK_FRAME_CTRL_HELLO) return false;
    if (!sfx_read_frame(fx, &f) || f.type != PIK_FRAME_CTRL_CONFIG) return false;
    return true;
}

static int tcp_connect_local(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in sa = { .sin_family = AF_INET,
                              .sin_port = htons((uint16_t)port) };
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        close(fd);
        return -1;
    }
    test_set_nonblock(fd);
    return fd;
}

static bool send_tun(session_fixture_t *fx, uint8_t type, uint8_t conn,
                     uint8_t gen, const uint8_t *data, size_t dlen) {
    uint8_t payload[PIK_TUN_MAX_PAYLOAD];
    payload[0] = conn;
    payload[1] = gen;
    if (dlen) memcpy(payload + PIK_TUN_PREFIX_LEN, data, dlen);
    return sfx_send_peer_frame(fx, type, PIK_CH_TUNNEL, payload,
                               PIK_TUN_PREFIX_LEN + dlen);
}

/* read frames until one of the given type arrives (skipping others) */
static bool read_frame_of_type(session_fixture_t *fx, uint8_t type, sfx_frame_t *f) {
    for (int i = 0; i < 16; i++) {
        if (!sfx_read_frame(fx, f)) return false;
        if (f->type == type) return true;
    }
    return false;
}

/* Listener mode. */
static void test_listen_open_data_close(void) {
    session_fixture_t fx;
    sfx_frame_t f;
    int port = free_port();
    CHECK(port > 0);

    CHECK(fixture_init_mode(&fx, TUNNEL_MODE_LISTEN, port));
    CHECK(handshake_as(&fx, PIK_CONTROL_ROLE_PTY));
    CHECK(tunnel_active());

    /* connect a local client: an OPEN with a fresh generation goes out */
    int client = tcp_connect_local(port);
    CHECK(client >= 0);
    CHECK(sfx_dispatch_one(&fx, 1000));  /* accept */
    CHECK(read_frame_of_type(&fx, PIK_FRAME_TUN_OPEN, &f));
    CHECK(f.ch == PIK_CH_TUNNEL);
    CHECK(f.payload_len == PIK_TUN_PREFIX_LEN);
    uint8_t conn = f.payload[0];
    uint8_t gen = f.payload[1];
    CHECK(gen == 1);

    /* client data is framed with the conn/gen prefix */
    CHECK(write(client, "abc", 3) == 3);
    CHECK(sfx_dispatch_one(&fx, 1000));
    CHECK(read_frame_of_type(&fx, PIK_FRAME_TUN_DATA, &f));
    CHECK(f.payload_len == PIK_TUN_PREFIX_LEN + 3);
    CHECK(f.payload[0] == conn && f.payload[1] == gen);
    CHECK(memcmp(f.payload + PIK_TUN_PREFIX_LEN, "abc", 3) == 0);

    /* peer data reaches the client */
    CHECK(send_tun(&fx, PIK_FRAME_TUN_DATA, conn, gen, (const uint8_t *)"xyz", 3));
    CHECK(sfx_settle(&fx));
    char buf[8];
    CHECK(test_wait_fd(client, false, 1000));
    CHECK(read(client, buf, sizeof(buf)) == 3);
    CHECK(memcmp(buf, "xyz", 3) == 0);

    /* peer CLOSE tears the client down */
    CHECK(send_tun(&fx, PIK_FRAME_TUN_CLOSE, conn, gen, NULL, 0));
    CHECK(sfx_settle(&fx));
    CHECK(test_wait_fd(client, false, 1000));
    CHECK(read(client, buf, sizeof(buf)) == 0);
    close(client);
    sfx_cleanup(&fx);
}

static void test_listen_client_close_sends_close(void) {
    session_fixture_t fx;
    sfx_frame_t f;
    int port = free_port();
    CHECK(port > 0);

    CHECK(fixture_init_mode(&fx, TUNNEL_MODE_LISTEN, port));
    CHECK(handshake_as(&fx, PIK_CONTROL_ROLE_PTY));

    int client = tcp_connect_local(port);
    CHECK(client >= 0);
    CHECK(sfx_dispatch_one(&fx, 1000));
    CHECK(read_frame_of_type(&fx, PIK_FRAME_TUN_OPEN, &f));
    uint8_t conn = f.payload[0], gen = f.payload[1];

    close(client);
    CHECK(sfx_dispatch_one(&fx, 1000));
    CHECK(read_frame_of_type(&fx, PIK_FRAME_TUN_CLOSE, &f));
    CHECK(f.payload[0] == conn && f.payload[1] == gen);
    sfx_cleanup(&fx);
}

static void test_generation_disambiguates_reused_slot(void) {
    session_fixture_t fx;
    sfx_frame_t f;
    int port = free_port();
    CHECK(port > 0);

    CHECK(fixture_init_mode(&fx, TUNNEL_MODE_LISTEN, port));
    CHECK(handshake_as(&fx, PIK_CONTROL_ROLE_PTY));

    int client1 = tcp_connect_local(port);
    CHECK(client1 >= 0);
    CHECK(sfx_dispatch_one(&fx, 1000));
    CHECK(read_frame_of_type(&fx, PIK_FRAME_TUN_OPEN, &f));
    uint8_t conn = f.payload[0];
    uint8_t gen1 = f.payload[1];

    /* close and reconnect: same slot, new generation */
    close(client1);
    CHECK(sfx_dispatch_one(&fx, 1000));
    CHECK(read_frame_of_type(&fx, PIK_FRAME_TUN_CLOSE, &f));
    int client2 = tcp_connect_local(port);
    CHECK(client2 >= 0);
    CHECK(sfx_dispatch_one(&fx, 1000));
    CHECK(read_frame_of_type(&fx, PIK_FRAME_TUN_OPEN, &f));
    CHECK(f.payload[0] == conn);
    uint8_t gen2 = f.payload[1];
    CHECK(gen2 == (uint8_t)(gen1 + 1));

    /* in-flight DATA from the old incarnation is silently dropped */
    CHECK(send_tun(&fx, PIK_FRAME_TUN_DATA, conn, gen1, (const uint8_t *)"old", 3));
    CHECK(sfx_settle(&fx));
    CHECK(pik_session_up());
    char buf[8];
    CHECK(!test_wait_fd(client2, false, 200));

    /* current-generation DATA still flows */
    CHECK(send_tun(&fx, PIK_FRAME_TUN_DATA, conn, gen2, (const uint8_t *)"new", 3));
    CHECK(sfx_settle(&fx));
    CHECK(test_wait_fd(client2, false, 1000));
    CHECK(read(client2, buf, sizeof(buf)) == 3);
    CHECK(memcmp(buf, "new", 3) == 0);
    close(client2);
    sfx_cleanup(&fx);
}

static void test_open_in_listen_mode_fails(void) {
    session_fixture_t fx;
    int port = free_port();
    CHECK(port > 0);

    CHECK(fixture_init_mode(&fx, TUNNEL_MODE_LISTEN, port));
    CHECK(handshake_as(&fx, PIK_CONTROL_ROLE_PTY));
    CHECK(send_tun(&fx, PIK_FRAME_TUN_OPEN, 0, 1, NULL, 0));
    CHECK(!sfx_dispatch_one(&fx, 1000));
    sfx_cleanup(&fx);
}

static void test_short_tunnel_frame_fails(void) {
    session_fixture_t fx;
    int port = free_port();
    CHECK(port > 0);

    CHECK(fixture_init_mode(&fx, TUNNEL_MODE_LISTEN, port));
    CHECK(handshake_as(&fx, PIK_CONTROL_ROLE_PTY));
    uint8_t one = 0;
    CHECK(sfx_send_peer_frame(&fx, PIK_FRAME_TUN_DATA, PIK_CH_TUNNEL, &one, 1));
    CHECK(!sfx_dispatch_one(&fx, 1000));
    sfx_cleanup(&fx);
}

static void test_zero_generation_fails(void) {
    session_fixture_t fx;
    int port = free_port();
    CHECK(port > 0);

    CHECK(fixture_init_mode(&fx, TUNNEL_MODE_LISTEN, port));
    CHECK(handshake_as(&fx, PIK_CONTROL_ROLE_PTY));
    CHECK(send_tun(&fx, PIK_FRAME_TUN_DATA, 0, 0,
                   (const uint8_t *)"x", 1));
    CHECK(!sfx_dispatch_one(&fx, 1000));
    sfx_cleanup(&fx);
}

static void test_tunnel_control_payload_fails(void) {
    session_fixture_t fx;
    int port = free_port();
    CHECK(port > 0);

    CHECK(fixture_init_mode(&fx, TUNNEL_MODE_LISTEN, port));
    CHECK(handshake_as(&fx, PIK_CONTROL_ROLE_PTY));
    CHECK(send_tun(&fx, PIK_FRAME_TUN_CLOSE, 0, 1,
                   (const uint8_t *)"x", 1));
    CHECK(!sfx_dispatch_one(&fx, 1000));
    sfx_cleanup(&fx);
}

/* Forward mode. */
typedef struct {
    int listen_fd;
    int port;
} target_t;

static bool target_start(target_t *t) {
    t->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (t->listen_fd < 0) return false;
    int one = 1;
    setsockopt(t->listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in sa = { .sin_family = AF_INET };
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t sl = sizeof(sa);
    if (bind(t->listen_fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 ||
        getsockname(t->listen_fd, (struct sockaddr *)&sa, &sl) < 0 ||
        listen(t->listen_fd, 4) < 0) {
        close(t->listen_fd);
        return false;
    }
    t->port = ntohs(sa.sin_port);
    return true;
}

static void test_forward_open_data_roundtrip(void) {
    session_fixture_t fx;
    sfx_frame_t f;
    target_t target;
    CHECK(target_start(&target));

    CHECK(fixture_init_mode(&fx, TUNNEL_MODE_FORWARD, target.port));
    CHECK(handshake_as(&fx, PIK_CONTROL_ROLE_MCU));
    CHECK(tunnel_active());

    /* OPEN dials the target */
    CHECK(send_tun(&fx, PIK_FRAME_TUN_OPEN, 2, 7, NULL, 0));
    CHECK(sfx_settle(&fx));
    CHECK(test_wait_fd(target.listen_fd, false, 1000));
    int srv = accept(target.listen_fd, NULL, NULL);
    CHECK(srv >= 0);
    test_set_nonblock(srv);

    /* peer DATA reaches the target with the OPEN's generation */
    CHECK(send_tun(&fx, PIK_FRAME_TUN_DATA, 2, 7, (const uint8_t *)"ping", 4));
    while (sfx_dispatch_one(&fx, 100)) {}
    char buf[8];
    CHECK(test_wait_fd(srv, false, 1000));
    CHECK(read(srv, buf, sizeof(buf)) == 4);
    CHECK(memcmp(buf, "ping", 4) == 0);

    /* target data comes back framed with the same conn/gen */
    CHECK(write(srv, "pong", 4) == 4);
    CHECK(sfx_dispatch_one(&fx, 1000));
    CHECK(read_frame_of_type(&fx, PIK_FRAME_TUN_DATA, &f));
    CHECK(f.payload[0] == 2 && f.payload[1] == 7);
    CHECK(f.payload_len == PIK_TUN_PREFIX_LEN + 4);
    CHECK(memcmp(f.payload + PIK_TUN_PREFIX_LEN, "pong", 4) == 0);

    close(srv);
    close(target.listen_fd);
    sfx_cleanup(&fx);
}

static void test_forward_duplicate_open_fails(void) {
    session_fixture_t fx;
    target_t target;
    CHECK(target_start(&target));

    CHECK(fixture_init_mode(&fx, TUNNEL_MODE_FORWARD, target.port));
    CHECK(handshake_as(&fx, PIK_CONTROL_ROLE_MCU));
    CHECK(send_tun(&fx, PIK_FRAME_TUN_OPEN, 1, 1, NULL, 0));
    CHECK(sfx_settle(&fx));
    CHECK(send_tun(&fx, PIK_FRAME_TUN_OPEN, 1, 2, NULL, 0));
    CHECK(!sfx_dispatch_one(&fx, 1000));
    close(target.listen_fd);
    sfx_cleanup(&fx);
}

static void test_data_for_closed_conn_answers_close(void) {
    session_fixture_t fx;
    sfx_frame_t f;
    target_t target;
    CHECK(target_start(&target));

    CHECK(fixture_init_mode(&fx, TUNNEL_MODE_FORWARD, target.port));
    CHECK(handshake_as(&fx, PIK_CONTROL_ROLE_MCU));
    CHECK(send_tun(&fx, PIK_FRAME_TUN_DATA, 5, 9, (const uint8_t *)"x", 1));
    CHECK(sfx_dispatch_one(&fx, 1000));
    CHECK(pik_session_up());
    CHECK(read_frame_of_type(&fx, PIK_FRAME_TUN_CLOSE, &f));
    CHECK(f.payload[0] == 5 && f.payload[1] == 9);
    close(target.listen_fd);
    sfx_cleanup(&fx);
}

static void test_tunnel_frame_without_tunnel_fails(void) {
    session_fixture_t fx;
    session_fixture_cfg_t cfg = { .role = PIK_CONTROL_ROLE_PTY };
    CHECK(sfx_init(&fx, &cfg));
    CHECK(sfx_handshake(&fx));
    CHECK(send_tun(&fx, PIK_FRAME_TUN_DATA, 0, 1, (const uint8_t *)"x", 1));
    CHECK(!sfx_dispatch_one(&fx, 1000));
    sfx_cleanup(&fx);
}

/* Shared-link TX scheduling. */
static void test_control_overtakes_tunnel_backlog(void) {
    session_fixture_t fx;
    sfx_frame_t f;
    int port = free_port();
    CHECK(port > 0);

    CHECK(fixture_init_mode(&fx, TUNNEL_MODE_LISTEN, port));
    CHECK(handshake_as(&fx, PIK_CONTROL_ROLE_PTY));

    /* queue a large tunnel backlog without giving the link a chance to
     * drain: only ~ADMIT_TARGET bytes may enter the wire FIFO, the rest
     * waits in the tunnel class queue */
    static uint8_t blob[PIK_TUN_MAX_PAYLOAD];
    memset(blob, 'T', sizeof(blob));
    blob[0] = 0;
    blob[1] = 1;
    const int n_tunnel = 30;
    for (int i = 0; i < n_tunnel; i++)
        CHECK(pik_session_enqueue(PIK_SESSION_CLASS_TUNNEL, PIK_FRAME_TUN_DATA,
                                  PIK_CH_TUNNEL, blob, sizeof(blob)));
    CHECK(pik_session_backlog(PIK_SESSION_CLASS_TUNNEL) > 0);

    /* a control frame queued afterwards must overtake the queued portion */
    CHECK(pik_session_enqueue(PIK_SESSION_CLASS_CONTROL, PIK_FRAME_CTRL_PING,
                              PIK_CH_CONTROL, NULL, 0));

    int ping_pos = -1;
    int seen = 0;
    for (int i = 0; i < n_tunnel + 1; i++) {
        CHECK(sfx_read_frame(&fx, &f));
        if (f.type == PIK_FRAME_CTRL_PING && ping_pos < 0) ping_pos = seen;
        seen++;
    }
    CHECK(ping_pos >= 0);
    CHECK(ping_pos < 10);       /* far ahead of the 30-frame tunnel burst */
    sfx_cleanup(&fx);
}

int main(void) {
    test_listen_open_data_close();
    test_listen_client_close_sends_close();
    test_generation_disambiguates_reused_slot();
    test_open_in_listen_mode_fails();
    test_short_tunnel_frame_fails();
    test_zero_generation_fails();
    test_tunnel_control_payload_fails();
    test_forward_open_data_roundtrip();
    test_forward_duplicate_open_fails();
    test_data_for_closed_conn_answers_close();
    test_tunnel_frame_without_tunnel_fails();
    test_control_overtakes_tunnel_backlog();

    if (failures) {
        fprintf(stderr, "test_tunnel_protocol: %d failure(s)\n", failures);
        return 1;
    }
    puts("test_tunnel_protocol: ok");
    return 0;
}

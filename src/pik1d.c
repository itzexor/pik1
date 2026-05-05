// src/pik1d.c — pik1 daemon: argument parsing, child management, main entry

#include "serialmux.h"
#include <errno.h>
#include <libgen.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/signalfd.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define RECONNECT_MIN_MS 500
#define RECONNECT_MAX_MS 8000

static void log_msg(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fputs("[pik1] ", stderr);
    vfprintf(stderr, fmt, ap); va_end(ap);
    fputc('\n', stderr);
}
#define LOG(...) log_msg(__VA_ARGS__)
#define DIE(...) do { log_msg(__VA_ARGS__); exit(1); } while(0)

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// ── child (tcpbridge) process management ─────────────────────────────────────

static struct {
    pid_t   pid;
    char   *argv[8];
    int64_t restart_at_ms;
    int     backoff_ms;
    int     sig_fd;
} g_child = { .pid = -1, .backoff_ms = RECONNECT_MIN_MS, .sig_fd = -1 };

static void child_spawn(void) {
    pid_t pid = fork();
    if (pid < 0) {
        LOG("child: fork: %s", strerror(errno));
        g_child.restart_at_ms = now_ms() + g_child.backoff_ms;
        g_child.backoff_ms = g_child.backoff_ms * 2;
        if (g_child.backoff_ms > RECONNECT_MAX_MS) g_child.backoff_ms = RECONNECT_MAX_MS;
        return;
    }
    if (pid == 0) {
        sigset_t all; sigfillset(&all);
        sigprocmask(SIG_UNBLOCK, &all, NULL);
        execvp(g_child.argv[0], g_child.argv);
        fprintf(stderr, "child: execvp %s: %s\n", g_child.argv[0], strerror(errno));
        _exit(127);
    }
    g_child.pid = pid;
    g_child.restart_at_ms = 0;
    g_child.backoff_ms = RECONNECT_MIN_MS;
    LOG("child: spawned %s pid=%d", g_child.argv[0], pid);
}

static void child_aux_cb(void) {
    struct signalfd_siginfo si;
    while (read(g_child.sig_fd, &si, sizeof(si)) == (ssize_t)sizeof(si)) {
        if (si.ssi_signo == SIGTERM) {
            if (g_child.pid > 0) {
                kill(g_child.pid, SIGTERM);
                waitpid(g_child.pid, NULL, 0);
            }
            exit(0);
        }
        if ((pid_t)si.ssi_pid != g_child.pid) continue;
        int status;
        waitpid(g_child.pid, &status, WNOHANG);
        g_child.pid = -1;
        LOG("child: exited status=%d, restart in %dms", status, g_child.backoff_ms);
        g_child.restart_at_ms = now_ms() + g_child.backoff_ms;
        g_child.backoff_ms = g_child.backoff_ms * 2;
        if (g_child.backoff_ms > RECONNECT_MAX_MS) g_child.backoff_ms = RECONNECT_MAX_MS;
    }
}

static void child_tick_cb(void) {
    if (g_child.pid >= 0 || g_child.restart_at_ms == 0) return;
    if (now_ms() >= g_child.restart_at_ms)
        child_spawn();
}

static int64_t child_deadline_fn(void) {
    if (g_child.pid >= 0 || g_child.restart_at_ms == 0) return INT64_MAX;
    return g_child.restart_at_ms;
}

// ── argument parsing + main ───────────────────────────────────────────────────
static void usage(const char *prog) {
    fprintf(stderr,
        "Usage:\n"
        "  %s --usb VID:PID  mcu:N:DEV:BAUD [...] [tcp:ADDR:PORT]\n"
        "  %s --usb VID:PID  pty:N:SYMLINK  [...] [tcp:HOST:PORT]\n",
        prog, prog);
    exit(1);
}

int main(int argc, char **argv) {
    if (argc < 4) usage(argv[0]);

    // Resolve directory of this executable for sibling tcpbridge lookup
    char self_dir[512] = "";
    {
        char tmp[512];
        strncpy(tmp, argv[0], sizeof(tmp) - 1);
        tmp[sizeof(tmp) - 1] = '\0';
        strncpy(self_dir, dirname(tmp), sizeof(self_dir) - 1);
    }

    int argi = 1;
    if (strcmp(argv[argi], "--usb") != 0 || argi + 1 >= argc) usage(argv[0]);
    const char *vidpid = argv[argi + 1];
    argi += 2;

    if (argi >= argc) usage(argv[0]);
    bool is_mcu = strncmp(argv[argi], "mcu:", 4) == 0;
    bool is_pty = strncmp(argv[argi], "pty:", 4) == 0;
    if (!is_mcu && !is_pty) usage(argv[0]);
    (void)is_pty;

    serialmux_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    strncpy(cfg.vidpid, vidpid, sizeof(cfg.vidpid) - 1);
    cfg.aux_fd = -1;

    while (argi < argc && cfg.n_channels < MAX_CHANNELS) {
        char *spec = argv[argi];
        if (strncmp(spec, "tcp:", 4) == 0) break;
        argi++;

        ch_spec_t *s = &cfg.channels[cfg.n_channels];
        memset(s, 0, sizeof(*s));

        if (is_mcu) {
            char baud_str[16];
            if (sscanf(spec, "mcu:%hhu:%127[^:]:%15s",
                       &s->ch_id, s->dev, baud_str) != 3)
                DIE("bad mcu spec: %s", spec);
            s->baud = atoi(baud_str);
            s->type = CH_MCU;
        } else {
            if (sscanf(spec, "pty:%hhu:%127s", &s->ch_id, s->path) != 2)
                DIE("bad pty spec: %s", spec);
            s->type = CH_PTY;
        }
        cfg.n_channels++;
    }

    if (!cfg.n_channels) usage(argv[0]);

    // Optional tcp: spec — spawn tcpbridge child
    bool has_tcp = false;
    if (argi < argc && strncmp(argv[argi], "tcp:", 4) == 0) {
        char addr[64];
        int  port;
        if (sscanf(argv[argi] + 4, "%63[^:]:%d", addr, &port) != 2)
            DIE("bad tcp spec: %s", argv[argi]);
        argi++;

        const char *td = serialmux_find_dev(vidpid, 1);
        if (!td) DIE("tcp: no tunnel device found (need two ACM endpoints)");

        // Static storage for child argv strings
        static char tb_path[576], tdev[64], addr_port[128];
        snprintf(addr_port, sizeof(addr_port), "%s:%d", addr, port);
        strncpy(tdev, td, sizeof(tdev) - 1);

        int i = 0;
        snprintf(tb_path, sizeof(tb_path), "%s/tcpbridge", self_dir);
        g_child.argv[i++] = (access(tb_path, X_OK) == 0) ? tb_path : "tcpbridge";
        g_child.argv[i++] = tdev;
        g_child.argv[i++] = is_mcu ? "listen" : "forward";
        g_child.argv[i++] = addr_port;
        g_child.argv[i]   = NULL;

        sigset_t mask;
        sigemptyset(&mask);
        sigaddset(&mask, SIGCHLD);
        sigaddset(&mask, SIGTERM);
        sigprocmask(SIG_BLOCK, &mask, NULL);
        g_child.sig_fd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
        if (g_child.sig_fd < 0) DIE("signalfd: %s", strerror(errno));

        cfg.aux_fd      = g_child.sig_fd;
        cfg.aux_cb      = child_aux_cb;
        cfg.tick_cb     = child_tick_cb;
        cfg.deadline_fn = child_deadline_fn;

        has_tcp = true;
        LOG("mode=%s channels=%d tcp=%s:%d tunnel=%s",
            is_mcu ? "exporter" : "host", cfg.n_channels, addr, port, tdev);
    } else {
        LOG("mode=%s channels=%d", is_mcu ? "exporter" : "host", cfg.n_channels);
    }

    if (has_tcp) child_spawn();
    serialmux_run(&cfg);
    return 0;
}

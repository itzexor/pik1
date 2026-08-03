#include "app_config.h"

#include "logging.h"
#include "util.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DIE(...) pik_die("pik1", __VA_ARGS__)

_Noreturn void pik_app_config_usage(const char *prog) {
    fprintf(stderr,
        "Usage:\n"
        "  %s --version\n"
        "  %s --control status-peer|restart-peer|reboot-peer|poweroff-peer\n"
        "  %s --usb  mcu:N:DEV:BAUD [...] [listen:BIND_ADDR:PORT]\n"
        "  %s --ffs  pty:N:SYMLINK  [...] [forward:TARGET_HOST:PORT]\n",
        prog, prog, prog, prog);
    exit(1);
}

static void parse_mcu_channel(const char *spec, ch_spec_t *s) {
    char ch_str[16], baud_str[16], extra;
    if (sscanf(spec, "mcu:%15[^:]:%127[^:]:%15[^:]%c",
               ch_str, s->dev, baud_str, &extra) != 3)
        DIE("bad mcu spec: %s", spec);
    if (!pik_parse_uint8(ch_str, &s->ch_id))
        DIE("bad mcu channel id: %s", spec);
    if (!pik_parse_positive_int(baud_str, &s->baud))
        DIE("bad mcu baud: %s", spec);
    s->type = CH_MCU;
}

static void parse_pty_channel(const char *spec, ch_spec_t *s) {
    char ch_str[16], extra;
    if (sscanf(spec, "pty:%15[^:]:%127s%c", ch_str, s->path, &extra) != 2)
        DIE("bad pty spec: %s", spec);
    if (!pik_parse_uint8(ch_str, &s->ch_id))
        DIE("bad pty channel id: %s", spec);
    s->type = CH_PTY;
}

static void parse_tcp_spec(app_config_t *cfg, const char *spec) {
    const char *hostport;
    if (strncmp(spec, "listen:", 7) == 0) {
        cfg->tcp_mode_name = "listen";
        cfg->tcp_role = PIK_CONTROL_TCP_LISTEN;
        cfg->tunnel_mode = TUNNEL_MODE_LISTEN;
        hostport = spec + 7;
    } else if (strncmp(spec, "forward:", 8) == 0) {
        cfg->tcp_mode_name = "forward";
        cfg->tcp_role = PIK_CONTROL_TCP_FORWARD;
        cfg->tunnel_mode = TUNNEL_MODE_FORWARD;
        hostport = spec + 8;
    } else {
        DIE("bad tcp spec: %s", spec);
    }

    char port_str[16], extra;
    if (sscanf(hostport, "%63[^:]:%15[^:]%c",
               cfg->tcp_addr, port_str, &extra) != 2 ||
        !pik_parse_port(port_str, &cfg->tcp_port))
        DIE("bad tcp spec: %s", spec);
    cfg->has_tcp = true;
}

void pik_app_config_parse(int argc, char **argv, app_config_t *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->tunnel_mode = TUNNEL_MODE_NONE;
    cfg->tcp_role = PIK_CONTROL_TCP_NONE;
    if (argc < 2)
        pik_app_config_usage(argv[0]);

    int argi = 1;
    if (strcmp(argv[argi], "--usb") == 0) {
        cfg->mode = APP_MODE_K1;
        cfg->uart_name = "mcu";
        cfg->control_role = PIK_CONTROL_ROLE_MCU;
    } else if (strcmp(argv[argi], "--ffs") == 0) {
        cfg->mode = APP_MODE_PI;
        cfg->uart_name = "pty";
        cfg->control_role = PIK_CONTROL_ROLE_PTY;
    } else {
        pik_app_config_usage(argv[0]);
    }
    argi++;

    bool k1_mode = cfg->mode == APP_MODE_K1;
    if (argi >= argc) pik_app_config_usage(argv[0]);
    if (k1_mode && strncmp(argv[argi], "mcu:", 4) != 0)
        DIE("--usb bulk mode is only valid on the K1/MCU side; use --ffs on the Pi side");
    if (!k1_mode && strncmp(argv[argi], "pty:", 4) != 0)
        DIE("--ffs is only valid on the Pi/PTY side");

    bool seen_ch[UINT8_MAX + 1] = { false };
    while (argi < argc) {
        char *spec = argv[argi];
        if (strncmp(spec, "listen:", 7) == 0 ||
            strncmp(spec, "forward:", 8) == 0)
            break;
        if (cfg->mux.n_channels >= MAX_CHANNELS)
            DIE("too many channels (max %d): %s", MAX_CHANNELS, spec);
        argi++;

        ch_spec_t *s = &cfg->mux.channels[cfg->mux.n_channels];
        memset(s, 0, sizeof(*s));
        if (k1_mode)
            parse_mcu_channel(spec, s);
        else
            parse_pty_channel(spec, s);

        if (!pik_mux_cli_valid(s->ch_id))
            DIE("channel id out of range (valid %u..%u): %s",
                PIK_MUX_CLI_BASE, PIK_MUX_CLI_LAST, spec);
        if (seen_ch[s->ch_id])
            DIE("duplicate channel id: %u", s->ch_id);
        seen_ch[s->ch_id] = true;
        cfg->mux.n_channels++;
    }
    if (!cfg->mux.n_channels) pik_app_config_usage(argv[0]);

    if (argi < argc &&
        (strncmp(argv[argi], "listen:", 7) == 0 ||
         strncmp(argv[argi], "forward:", 8) == 0)) {
        parse_tcp_spec(cfg, argv[argi]);
        argi++;
    }

    if (argi != argc) DIE("unexpected argument: %s", argv[argi]);
}

#include "main.h"

#include <getopt.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MESSAGE_COUNT_DEFAULT 1000
#define PAYLOAD_SIZE_DEFAULT 16
#define TCP_PORT_DEFAULT 1502
#define TCP_RX_FIFO_CAPACITY_DEFAULT 4096

// --------------------------------------------------------------------------------------
// Usage

void
main_usage(const char* exe_name)
{
    fprintf(stdout,
      "Usage:\n"
      "  %s --tcp-master --host <addr> [--port N] [--tcp-nodelay] [--keepalive]\n"
      "  %s --tcp-slave [--host <bind-addr>] [--port N] [--tcp-nodelay] [--keepalive]\n"
      "\n"
      "Options:\n"
      "  --tcp-master           Use TCP master (client)\n"
      "  --tcp-slave            Use TCP slave (server)\n"
      "  --host <addr>          TCP host\n"
      "                         master: remote IP/hostname to connect to\n"
      "                         slave:  local bind IP/hostname (default 0.0.0.0)\n"
      "  --port <n>             TCP port (default %d)\n"
      "  --tcp-nodelay          Enable TCP_NODELAY\n"
      "  --keepalive            Enable SO_KEEPALIVE\n"
      "  --rx-fifo-capacity <n> RX FIFO size for TCP backend (default %d)\n"
      "  --connect-timeout-ms <n>\n"
      "                         TCP connect timeout hint in ms (default 0)\n"
      "  --accept-timeout-ms <n>\n"
      "                         TCP accept timeout hint in ms (default 0)\n"
      "  --message-count <n>    Number of messages to send (default %d)\n"
      "  --payload-size <n>     Size of each message payload (default %d)\n"
      "  -h, --help             Show this help\n"
      "\n",
      exe_name,
      exe_name,
      TCP_PORT_DEFAULT,
      TCP_RX_FIFO_CAPACITY_DEFAULT,
      MESSAGE_COUNT_DEFAULT,
      PAYLOAD_SIZE_DEFAULT);
}

// --------------------------------------------------------------------------------------
// Config init

void
main_config_init(main_config_t* cfg)
{
    if (cfg == NULL)
        return;

    memset(cfg, 0, sizeof(*cfg));
    cfg->mode = MAIN_MODE_NONE;

    cfg->host = NULL;
    cfg->port = TCP_PORT_DEFAULT;

    cfg->tcp_nodelay = false;
    cfg->keepalive = false;
    cfg->rx_fifo_capacity = TCP_RX_FIFO_CAPACITY_DEFAULT;
    cfg->connect_timeout_ms = 0;
    cfg->accept_timeout_ms = 0;

    cfg->message_count = MESSAGE_COUNT_DEFAULT;
    cfg->payload_size = PAYLOAD_SIZE_DEFAULT;

    cfg->help_requested = false;
}

// --------------------------------------------------------------------------------------
// Helpers

static bool
parse_int_strict(const char* s, int* out)
{
    if (s == NULL || *s == '\0')
        return false;

    char* end = NULL;
    long v = strtol(s, &end, 10);

    if (end == s || *end != '\0')
        return false;

    if (v < -2147483648L || v > 2147483647L)
        return false;

    *out = (int)v;
    return true;
}

static dterr_t*
fail_badarg(const char* fmt, ...)
{
    dterr_t* dterr = NULL;

    va_list ap;
    va_start(ap, fmt);

    char buf[512];
    vsnprintf(buf, sizeof(buf), fmt, ap);

    va_end(ap);

    dterr = dterr_new(DTERR_BADARG, DTERR_LOC, NULL, "%s", buf);
    return dterr;
}

static dterr_t*
fail_range(const char* fmt, ...)
{
    dterr_t* dterr = NULL;

    va_list ap;
    va_start(ap, fmt);

    char buf[512];
    vsnprintf(buf, sizeof(buf), fmt, ap);

    va_end(ap);

    dterr = dterr_new(DTERR_RANGE, DTERR_LOC, NULL, "%s", buf);
    return dterr;
}

static dterr_t*
set_mode_once(main_config_t* cfg, main_mode_t mode)
{
    if (cfg->mode != MAIN_MODE_NONE)
        return fail_badarg("Multiple modes specified (choose one of --tcp-master, --tcp-slave)");

    cfg->mode = mode;
    return NULL;
}

static dterr_t*
validate_cfg(const char* exe_name, const main_config_t* cfg)
{
    if (cfg == NULL)
        return dterr_new(DTERR_ARGUMENT_NULL, DTERR_LOC, NULL, "cfg is NULL");

    if (cfg->help_requested)
        return NULL;

    if (cfg->mode == MAIN_MODE_NONE)
    {
        main_usage(exe_name);
        return dterr_new(DTERR_BADARG, DTERR_LOC, NULL, "Missing mode: specify exactly one of --tcp-master or --tcp-slave");
    }

    if (cfg->port <= 0 || cfg->port > 65535)
        return fail_range("Invalid port %d (expected 1..65535)", cfg->port);

    if (cfg->mode == MAIN_MODE_TCP_MASTER)
    {
        if (cfg->host == NULL || cfg->host[0] == '\0')
            return dterr_new(DTERR_BADARG, DTERR_LOC, NULL, "--tcp-master requires --host <addr>");
    }

    // For TCP slave, host is optional; NULL means bind all interfaces.

    if (cfg->rx_fifo_capacity <= 0)
        return fail_range("Invalid rx-fifo-capacity %d (expected > 0)", (int)cfg->rx_fifo_capacity);

    if (cfg->connect_timeout_ms < 0)
        return fail_range("Invalid connect-timeout-ms %d (expected >= 0)", (int)cfg->connect_timeout_ms);

    if (cfg->accept_timeout_ms < 0)
        return fail_range("Invalid accept-timeout-ms %d (expected >= 0)", (int)cfg->accept_timeout_ms);

    if (cfg->message_count < 0)
        return fail_range("Invalid message-count %d (expected >= 0)", (int)cfg->message_count);

    if (cfg->payload_size < 0)
        return fail_range("Invalid payload-size %d (expected >= 0)", (int)cfg->payload_size);

    return NULL;
}

// --------------------------------------------------------------------------------------
// Translation helper

dterr_t*
main_build_tcp_config(const main_config_t* cfg, dtiox_linux_tcp_config_t* tcp_cfg)
{
    if (cfg == NULL)
        return dterr_new(DTERR_ARGUMENT_NULL, DTERR_LOC, NULL, "cfg is NULL");

    if (tcp_cfg == NULL)
        return dterr_new(DTERR_ARGUMENT_NULL, DTERR_LOC, NULL, "tcp_cfg is NULL");

    memset(tcp_cfg, 0, sizeof(*tcp_cfg));

    tcp_cfg->rx_fifo_capacity = cfg->rx_fifo_capacity;
    tcp_cfg->tcp_nodelay = cfg->tcp_nodelay;
    tcp_cfg->keepalive = cfg->keepalive;
    tcp_cfg->connect_timeout_ms = cfg->connect_timeout_ms;
    tcp_cfg->accept_timeout_ms = cfg->accept_timeout_ms;

    switch (cfg->mode)
    {
        case MAIN_MODE_TCP_MASTER:
            tcp_cfg->mode = DTIOX_LINUX_TCP_MODE_CLIENT;
            tcp_cfg->remote_host = cfg->host;
            tcp_cfg->remote_port = cfg->port;
            break;

        case MAIN_MODE_TCP_SLAVE:
            tcp_cfg->mode = DTIOX_LINUX_TCP_MODE_SERVER;
            tcp_cfg->local_bind_host = cfg->host; // NULL => 0.0.0.0
            tcp_cfg->local_bind_port = cfg->port;
            break;

        default:
            return dterr_new(
              DTERR_BADARG, DTERR_LOC, NULL, "main_build_tcp_config requires MAIN_MODE_TCP_MASTER or MAIN_MODE_TCP_SLAVE");
    }

    return NULL;
}

// --------------------------------------------------------------------------------------
// Parser

dterr_t*
main_parse_args(int argc, char** argv, main_config_t* cfg)
{
    dterr_t* dterr = NULL;

    DTERR_ASSERT_NOT_NULL(argv);
    DTERR_ASSERT_NOT_NULL(cfg);

    main_config_init(cfg);

    static struct option long_options[] = { { "tcp-master", no_argument, 0, 'm' },
        { "tcp-slave", no_argument, 0, 's' },
        { "host", required_argument, 0, 'H' },
        { "ip", required_argument, 0, 'i' }, // alias for backward compatibility
        { "port", required_argument, 0, 'p' },
        { "tcp-nodelay", no_argument, 0, 'n' },
        { "keepalive", no_argument, 0, 'k' },
        { "rx-fifo-capacity", required_argument, 0, 'f' },
        { "connect-timeout-ms", required_argument, 0, 'x' },
        { "accept-timeout-ms", required_argument, 0, 'a' },
        { "message-count", required_argument, 0, 'c' },
        { "payload-size", required_argument, 0, 'b' },
        { "help", no_argument, 0, 'h' },
        { 0, 0, 0, 0 } };

    optind = 1;

    int opt;
    while ((opt = getopt_long(argc, argv, "msH:i:p:nkf:x:a:c:b:h", long_options, NULL)) != -1)
    {
        switch (opt)
        {
            case 'm':
                DTERR_C(set_mode_once(cfg, MAIN_MODE_TCP_MASTER));
                break;

            case 's':
                DTERR_C(set_mode_once(cfg, MAIN_MODE_TCP_SLAVE));
                break;

            case 'H':
            case 'i':
                cfg->host = optarg;
                break;

            case 'p':
            {
                int v = 0;
                if (!parse_int_strict(optarg, &v))
                {
                    dterr = fail_badarg("Invalid --port value: %s", optarg);
                    goto cleanup;
                }
                cfg->port = v;
                break;
            }

            case 'n':
                cfg->tcp_nodelay = true;
                break;

            case 'k':
                cfg->keepalive = true;
                break;

            case 'f':
            {
                int v = 0;
                if (!parse_int_strict(optarg, &v))
                {
                    dterr = fail_badarg("Invalid --rx-fifo-capacity value: %s", optarg);
                    goto cleanup;
                }
                cfg->rx_fifo_capacity = (int32_t)v;
                break;
            }

            case 'x':
            {
                int v = 0;
                if (!parse_int_strict(optarg, &v))
                {
                    dterr = fail_badarg("Invalid --connect-timeout-ms value: %s", optarg);
                    goto cleanup;
                }
                cfg->connect_timeout_ms = (int32_t)v;
                break;
            }

            case 'a':
            {
                int v = 0;
                if (!parse_int_strict(optarg, &v))
                {
                    dterr = fail_badarg("Invalid --accept-timeout-ms value: %s", optarg);
                    goto cleanup;
                }
                cfg->accept_timeout_ms = (int32_t)v;
                break;
            }

            case 'c':
            {
                int v = 0;
                if (!parse_int_strict(optarg, &v))
                {
                    dterr = fail_badarg("Invalid --message-count value: %s", optarg);
                    goto cleanup;
                }
                if (v < 0)
                {
                    dterr = fail_range("Invalid message-count %d (expected >= 0)", v);
                    goto cleanup;
                }
                cfg->message_count = (int32_t)v;
                break;
            }

            case 'b':
            {
                int v = 0;
                if (!parse_int_strict(optarg, &v))
                {
                    dterr = fail_badarg("Invalid --payload-size value: %s", optarg);
                    goto cleanup;
                }
                if (v < 0)
                {
                    dterr = fail_range("Invalid payload-size %d (expected >= 0)", v);
                    goto cleanup;
                }
                cfg->payload_size = (int32_t)v;
                break;
            }

            case 'h':
                cfg->help_requested = true;
                main_usage(argv[0]);
                goto cleanup;

            default:
                main_usage(argv[0]);
                dterr = dterr_new(DTERR_BADARG, DTERR_LOC, NULL, "Bad arguments");
                goto cleanup;
        }
    }

    if (optind < argc)
    {
        main_usage(argv[0]);
        dterr = fail_badarg("Unexpected positional argument: %s", argv[optind]);
        goto cleanup;
    }

    DTERR_C(validate_cfg(argv[0], cfg));

cleanup:
    return dterr;
}
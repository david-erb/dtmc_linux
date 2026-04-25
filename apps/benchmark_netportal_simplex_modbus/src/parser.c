#include "main.h"

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MESSAGE_COUNT_DEFAULT 1000
#define PAYLOAD_SIZE_DEFAULT 16

// --------------------------------------------------------------------------------------
// Usage

void
main_usage(const char* exe_name)
{
    fprintf(stdout,
      "Usage:\n"
      "  %s --rtu --device <path> [--slave-id N]\n"
      "  %s --tcp-master --ip <addr> [--port N]\n"
      "  %s --tcp-slave [--port N]\n"
      "\n"
      "Options:\n"
      "  --rtu                 Use Modbus RTU master over UART device\n"
      "  --tcp-master          Use Modbus TCP master (client)\n"
      "  --tcp-slave           Use Modbus TCP slave (server)\n"
      "  --device <path>       UART device path (e.g. /dev/ttyUSB0)\n"
      "  --ip <addr>           IP address/hostname for TCP master\n"
      "  --port <n>            TCP port (default 1502)\n"
      "  --slave-id <n>        Modbus slave id for RTU (default 1)\n"
      "  --message-count <n>   Number of messages to send (default %d)\n"
      "  --payload-size <n>    Size of each message payload (default %d)\n"
      "  -h, --help            Show this help\n"
      "\n",
      exe_name,
      exe_name,
      exe_name,
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
    cfg->port = 1502;
    cfg->slave_id = 1;
    cfg->message_count = MESSAGE_COUNT_DEFAULT;
    cfg->payload_size = PAYLOAD_SIZE_DEFAULT;
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

    // dterr_new is variadic, not va_list-based, so we need to format ourselves.
    // Keep this local and simple.
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
validate_cfg(const char* exe_name, const main_config_t* cfg)
{
    if (cfg == NULL)
        return dterr_new(DTERR_ARGUMENT_NULL, DTERR_LOC, NULL, "cfg is NULL");

    if (cfg->mode == MAIN_MODE_NONE)
    {
        main_usage(exe_name);
        return dterr_new(
          DTERR_BADARG, DTERR_LOC, NULL, "Missing mode: specify exactly one of --rtu, --tcp-master, --tcp-slave");
    }

    // Port is only meaningful for TCP modes but validate regardless.
    if (cfg->port <= 0 || cfg->port > 65535)
        return fail_range("Invalid port %d (expected 1..65535)", cfg->port);

    if (cfg->mode == MAIN_MODE_RTU_MASTER)
    {
        if (cfg->device == NULL || cfg->device[0] == '\0')
            return dterr_new(DTERR_BADARG, DTERR_LOC, NULL, "--rtu requires --device <path>");

        if (cfg->slave_id < 1 || cfg->slave_id > 247)
            return fail_range("Invalid slave-id %d (expected 1..247)", cfg->slave_id);
    }

    if (cfg->mode == MAIN_MODE_TCP_MASTER)
    {
        if (cfg->ip == NULL || cfg->ip[0] == '\0')
            return dterr_new(DTERR_BADARG, DTERR_LOC, NULL, "--tcp-master requires --ip <addr>");
    }

    if (cfg->mode == MAIN_MODE_TCP_SLAVE)
    {
        // no additional required args
    }

    if (cfg->message_count < 0)
        return fail_range("Invalid message-count %d (expected >= 0)", (int)cfg->message_count);

    if (cfg->payload_size < 0)
        return fail_range("Invalid payload-size %d (expected >= 0)", (int)cfg->payload_size);

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

    static struct option long_options[] = { { "rtu", no_argument, 0, 'r' },
        { "tcp-master", no_argument, 0, 'm' },
        { "tcp-slave", no_argument, 0, 's' },
        { "device", required_argument, 0, 'd' },
        { "ip", required_argument, 0, 'i' },
        { "port", required_argument, 0, 'p' },
        { "slave-id", required_argument, 0, 'l' },
        { "message-count", required_argument, 0, 'c' },
        { "payload-size", required_argument, 0, 'b' },
        { "help", no_argument, 0, 'h' },
        { 0, 0, 0, 0 } };

    // Reset getopt state (important if parse_args is called in tests)
    optind = 1;

    int opt;

    while ((opt = getopt_long(argc, argv, "rmsd:i:p:l:c:b:h", long_options, NULL)) != -1)
    {
        switch (opt)
        {
            case 'r':
                if (cfg->mode != MAIN_MODE_NONE)
                {
                    dterr = fail_badarg("Multiple modes specified (choose one of --rtu, --tcp-master, --tcp-slave)");
                    goto cleanup;
                }
                cfg->mode = MAIN_MODE_RTU_MASTER;
                break;

            case 'm':
                if (cfg->mode != MAIN_MODE_NONE)
                {
                    dterr = fail_badarg("Multiple modes specified (choose one of --rtu, --tcp-master, --tcp-slave)");
                    goto cleanup;
                }
                cfg->mode = MAIN_MODE_TCP_MASTER;
                break;

            case 's':
                if (cfg->mode != MAIN_MODE_NONE)
                {
                    dterr = fail_badarg("Multiple modes specified (choose one of --rtu, --tcp-master, --tcp-slave)");
                    goto cleanup;
                }
                cfg->mode = MAIN_MODE_TCP_SLAVE;
                break;

            case 'd':
                cfg->device = optarg;
                break;

            case 'i':
                cfg->ip = optarg;
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

            case 'l':
            {
                int v = 0;
                if (!parse_int_strict(optarg, &v))
                {
                    dterr = fail_badarg("Invalid --slave-id value: %s", optarg);
                    goto cleanup;
                }
                cfg->slave_id = v;
                break;
            }

            case 'h':
                main_usage(argv[0]);
                dterr = NULL; // help is not an error
                goto cleanup;

            case 'c':
            {
                int v = 0;
                if (!parse_int_strict(optarg, &v))
                {
                    dterr = fail_badarg("Invalid --message-count value: %s", optarg);
                    goto cleanup;
                }

                // store as int32_t (range-check to be safe)
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

                // store as int32_t (range-check to be safe)
                if (v < 0)
                {
                    dterr = fail_range("Invalid --payload-size %d (expected >= 0)", v);
                    goto cleanup;
                }

                cfg->payload_size = (int32_t)v;
                break;
            }

            default:
                main_usage(argv[0]);
                dterr = dterr_new(DTERR_BADARG, DTERR_LOC, NULL, "Bad arguments");
                goto cleanup;
        }
    }

    // Reject unexpected positional args (strict)
    if (optind < argc)
    {
        main_usage(argv[0]);
        dterr = fail_badarg("Unexpected positional argument: %s", argv[optind]);
        goto cleanup;
    }

    // Validate cross-field requirements
    DTERR_C(validate_cfg(argv[0], cfg));

cleanup:
    return dterr;
}
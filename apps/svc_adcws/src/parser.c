#include "main.h"

#include <getopt.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RX_HOST_DEFAULT "localhost"
#define RX_PORT_DEFAULT 14040
#define TX_HOST_DEFAULT "0.0.0.0"
#define TX_PORT_DEFAULT 14090

// --------------------------------------------------------------------------------------
// Usage

void
main_usage(const char* exe_name)
{
    fprintf(stdout,
      "Usage:\n"
      "  %s [--rx-host [addr]] [--rx-port [n]] [--tx-host [addr]] [--tx-port [n]]\n"
      "\n"
      "Options:\n"
      "  --rx-host [addr]       Input host address or hostname (default %s)\n"
      "  --rx-port [n]          Input TCP/IP port (default %d)\n"
      "  --tx-host [addr]       Output host address or hostname (default %s)\n"
      "  --tx-port [n]          Output TCP/IP port (default %d)\n"
      "  -v, --verbose          Enable verbose logging\n"
      "  -h, --help             Show this help\n"
      "\n",
      exe_name,
      RX_HOST_DEFAULT,
      RX_PORT_DEFAULT,
      TX_HOST_DEFAULT,
      TX_PORT_DEFAULT);
}

// --------------------------------------------------------------------------------------
// Config init

void
main_config_init(main_config_t* cfg)
{
    if (cfg == NULL)
        return;

    memset(cfg, 0, sizeof(*cfg));

    cfg->rx_host = RX_HOST_DEFAULT;
    cfg->rx_port = RX_PORT_DEFAULT;
    cfg->tx_host = TX_HOST_DEFAULT;
    cfg->tx_port = TX_PORT_DEFAULT;

    cfg->help_requested = false;
    cfg->verbose = false;
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

// --------------------------------------------------------------------------------------

static const char*
getopt_optional_arg(int argc, char** argv)
{
    if (optarg != NULL)
        return optarg;

    if (optind < argc && argv[optind] != NULL && argv[optind][0] != '-')
    {
        return argv[optind++];
    }

    return NULL;
}

// --------------------------------------------------------------------------------------

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

// --------------------------------------------------------------------------------------

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

// --------------------------------------------------------------------------------------

static dterr_t*
validate_cfg(const char* exe_name, const main_config_t* cfg)
{
    if (cfg == NULL)
        return dterr_new(DTERR_ARGUMENT_NULL, DTERR_LOC, NULL, "cfg is NULL");

    if (cfg->help_requested)
        return NULL;

    if (cfg->rx_host == NULL || cfg->rx_host[0] == '\0')
    {
        main_usage(exe_name);
        return dterr_new(DTERR_BADARG, DTERR_LOC, NULL, "Invalid --rx-host value");
    }

    if (cfg->rx_port <= 0 || cfg->rx_port > 65535)
    {
        main_usage(exe_name);
        return dterr_new(DTERR_BADARG, DTERR_LOC, NULL, "Invalid --rx-port value: %d", cfg->rx_port);
    }

    if (cfg->tx_host == NULL || cfg->tx_host[0] == '\0')
    {
        main_usage(exe_name);
        return dterr_new(DTERR_BADARG, DTERR_LOC, NULL, "Invalid --tx-host value");
    }

    if (cfg->tx_port <= 0 || cfg->tx_port > 65535)
    {
        main_usage(exe_name);
        return dterr_new(DTERR_BADARG, DTERR_LOC, NULL, "Invalid --tx-port value: %d", cfg->tx_port);
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

    static struct option long_options[] = { //
        { "rx-host", optional_argument, 0, 'r' },
        { "rx-port", optional_argument, 0, 'p' },
        { "tx-host", optional_argument, 0, 't' },
        { "tx-port", optional_argument, 0, 'q' },
        { "verbose", no_argument, 0, 'v' },
        { "help", no_argument, 0, 'h' },
        { 0, 0, 0, 0 }
    };

    optind = 1;

    int opt;
    while ((opt = getopt_long(argc, argv, "r::p::t::q::vh", long_options, NULL)) != -1)
    {
        switch (opt)
        {
            case 'r':
            {
                const char* s = getopt_optional_arg(argc, argv);
                if (s != NULL)
                    cfg->rx_host = s;
                break;
            }

            case 'p':
            {
                const char* s = getopt_optional_arg(argc, argv);
                if (s != NULL)
                {
                    int v = 0;
                    if (!parse_int_strict(s, &v))
                    {
                        dterr = fail_badarg("Invalid --rx-port value: %s", s);
                        goto cleanup;
                    }
                    cfg->rx_port = v;
                }
                break;
            }

            case 't':
            {
                const char* s = getopt_optional_arg(argc, argv);
                if (s != NULL)
                    cfg->tx_host = s;
                break;
            }

            case 'q':
            {
                const char* s = getopt_optional_arg(argc, argv);
                if (s != NULL)
                {
                    int v = 0;
                    if (!parse_int_strict(s, &v))
                    {
                        dterr = fail_badarg("Invalid --tx-port value: %s", s);
                        goto cleanup;
                    }
                    cfg->tx_port = v;
                }
                break;
            }

            case 'v':
                cfg->verbose = true;
                break;

            case 'h':
                cfg->help_requested = true;
                main_usage(argv[0]);
                goto cleanup;

            default:
                main_usage(argv[0]);
                dterr = dterr_new(DTERR_BADARG, DTERR_LOC, NULL, "Bad arguments %c", opt);
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
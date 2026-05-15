#include <dtcore/dterr.h>
#include <dtcore/dtunittest.h>

#include <dtmc_base/dthttpd.h>

#include <dtmc/dthttpd_linux_socket.h>

#include <dtmc_base_tests.h>

#define TAG "test_dthttpd_socket"

// --------------------------------------------------------------------------------------------
static dterr_t*
_create_server(dthttpd_handle* out_handle)
{
    dterr_t* dterr = NULL;
    dthttpd_linux_socket_t* o = NULL;
    dthttpd_linux_socket_config_t c = { 0 };

    DTERR_ASSERT_NOT_NULL(out_handle);
    *out_handle = NULL;

    c.bind_port = 8080;
    c.max_concurrent_connections = 2;

    DTERR_C(dthttpd_linux_socket_create(&o));
    DTERR_C(dthttpd_linux_socket_configure(o, &c));

    *out_handle = (dthttpd_handle)o;
    o = NULL;

cleanup:
    if (o != NULL)
        dthttpd_dispose((dthttpd_handle)o);
    return dterr;
}

// --------------------------------------------------------------------------------------------
static dterr_t*
test_dtmc_linux_dthttpd_socket_simple(void)
{
    dterr_t* dterr = NULL;
    dthttpd_handle httpd_handle = NULL;

    DTERR_C(_create_server(&httpd_handle));
    DTERR_C(test_dtmc_base_dthttpd_simple(httpd_handle));

cleanup:
    dthttpd_dispose(httpd_handle);
    return dterr;
}

// --------------------------------------------------------------------------------------------
static dterr_t*
test_dtmc_linux_dthttpd_socket_post_json(void)
{
    dterr_t* dterr = NULL;
    dthttpd_handle httpd_handle = NULL;

    DTERR_C(_create_server(&httpd_handle));
    DTERR_C(test_dtmc_base_dthttpd_post_json(httpd_handle));

cleanup:
    dthttpd_dispose(httpd_handle);
    return dterr;
}

// --------------------------------------------------------------------------------------------
static dterr_t*
test_dtmc_linux_dthttpd_socket_post_empty_body(void)
{
    dterr_t* dterr = NULL;
    dthttpd_handle httpd_handle = NULL;

    DTERR_C(_create_server(&httpd_handle));
    DTERR_C(test_dtmc_base_dthttpd_post_empty_body(httpd_handle));

cleanup:
    dthttpd_dispose(httpd_handle);
    return dterr;
}

// --------------------------------------------------------------------------------------------
static dterr_t*
test_dtmc_linux_dthttpd_socket_get_not_found(void)
{
    dterr_t* dterr = NULL;
    dthttpd_handle httpd_handle = NULL;

    DTERR_C(_create_server(&httpd_handle));
    DTERR_C(test_dtmc_base_dthttpd_get_not_found(httpd_handle));

cleanup:
    dthttpd_dispose(httpd_handle);
    return dterr;
}

// --------------------------------------------------------------------------------------------
static dterr_t*
test_dtmc_linux_dthttpd_socket_post_no_callback(void)
{
    dterr_t* dterr = NULL;
    dthttpd_handle httpd_handle = NULL;

    DTERR_C(_create_server(&httpd_handle));
    DTERR_C(test_dtmc_base_dthttpd_post_no_callback(httpd_handle));

cleanup:
    dthttpd_dispose(httpd_handle);
    return dterr;
}

// --------------------------------------------------------------------------------------------
static dterr_t*
test_dtmc_linux_dthttpd_socket_post_sequential(void)
{
    dterr_t* dterr = NULL;
    dthttpd_handle httpd_handle = NULL;

    DTERR_C(_create_server(&httpd_handle));
    DTERR_C(test_dtmc_base_dthttpd_post_sequential(httpd_handle));

cleanup:
    dthttpd_dispose(httpd_handle);
    return dterr;
}

// --------------------------------------------------------------------------------------------
void
test_dtmc_linux_dthttpd_socket(DTUNITTEST_SUITE_ARGS)
{
    DTUNITTEST_RUN_TEST(test_dtmc_linux_dthttpd_socket_simple);
    DTUNITTEST_RUN_TEST(test_dtmc_linux_dthttpd_socket_post_json);
    DTUNITTEST_RUN_TEST(test_dtmc_linux_dthttpd_socket_post_empty_body);
    DTUNITTEST_RUN_TEST(test_dtmc_linux_dthttpd_socket_get_not_found);
    DTUNITTEST_RUN_TEST(test_dtmc_linux_dthttpd_socket_post_no_callback);
    DTUNITTEST_RUN_TEST(test_dtmc_linux_dthttpd_socket_post_sequential);
}

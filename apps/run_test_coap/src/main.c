#include <dtcore/dterr.h>

#include <dtmc_linux_tests.h>

int
main(int argc, char* argv[])
{
    const char* pattern = NULL;

    if (argc > 1)
    {
        pattern = argv[1];
    }

    dterr_t* dterr = NULL;

    dterr = test_net_matching(pattern);

    int rc = 0;
    if (dterr != NULL)
    {
        rc = 1;
    }

    return rc;
}
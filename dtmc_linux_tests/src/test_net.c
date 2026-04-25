#include <stdio.h>
#include <string.h>

#include <dtcore/dtbuffer.h>
#include <dtcore/dterr.h>
#include <dtcore/dtledger.h>
#include <dtcore/dtlog.h>
#include <dtcore/dtstr.h>

#include <dtcore/dtunittest.h>

#include <dtmc_linux_tests.h>

// ----------------------------------------------------------------
void
test_dtmc_linux_most_matching(DTUNITTEST_SUITE_ARGS)
{
    // ledgers we will check at end of each test
    dtledger_t* ledgers[10] = { 0 };
    {
        int i = 0;
        ledgers[i++] = dterr_ledger;
        ledgers[i++] = dtstr_ledger;
        ledgers[i++] = dtbuffer_ledger;
    }

    unittest_control->ledgers = ledgers;

    DTUNITTEST_RUN_SUITE(test_dtnetportal_mosquitto);

    DTUNITTEST_RUN_SUITE(test_dtnetportal_coap);
}

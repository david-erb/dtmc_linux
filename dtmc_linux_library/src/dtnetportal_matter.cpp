#include <app/ConcreteAttributePath.h>
#include <app/reporting/reporting.h> // <-- for MatterReportingAttributeChangeCallback
#include <lib/support/Span.h>

// If dt_topics_write_cache is implemented in C, declare it with C linkage:
extern "C" void
dt_topics_write_cache(uint32_t attrId, const uint8_t* data, size_t len);

extern "C" bool
dt_topics_publish_from_local(uint32_t attrId, const uint8_t* data, size_t len)
{
    using namespace chip::app;

    const ConcreteAttributePath path(/*endpoint*/ 1,
      /*cluster*/ 0xFFF100,
      /*attribute*/ attrId);

    // Notify reporting engine (void return)
    // MatterReportingAttributeChangeCallback(path);

    MatterReportingAttributeChangeCallback(1, 0xFFF100, attrId);

    return true;
}

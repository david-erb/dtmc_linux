// the default address resolver implementation used on Linux
#ifndef CHIP_ADDRESS_RESOLVE_IMPL_INCLUDE_HEADER
#define CHIP_ADDRESS_RESOLVE_IMPL_INCLUDE_HEADER <lib/address_resolve/AddressResolve_DefaultImpl.h>
#endif

// tell inet which endpoint implementations to use on Linux
#ifndef INET_UDP_END_POINT_IMPL_CONFIG_FILE
#define INET_UDP_END_POINT_IMPL_CONFIG_FILE <inet/UDPEndPointImplSockets.h>
#endif
#ifndef INET_TCP_END_POINT_IMPL_CONFIG_FILE
#define INET_TCP_END_POINT_IMPL_CONFIG_FILE <inet/TCPEndPointImplSockets.h>
#endif

// AttributeAccess registration — works with both pre- and post-registry SDKs.
#if __has_include(<app/AttributeAccessInterfaceRegistry.h>)
// Newer SDKs (v1.4+): registry split into its own header
#include <app/AttributeAccessInterface.h>
#include <app/AttributeAccessInterfaceRegistry.h>
#define DT_HAS_ATTR_REGISTRY 1
#elif __has_include(<app/AttributeAccessInterface.h>)
// Older SDKs: only the interface header exists + free functions
#include <app/AttributeAccessInterface.h>
#else
#error "Matter headers not found; ensure CHIP_ROOT/src is on your include path."
#endif

#include <app/ConcreteAttributePath.h>
#include <app/MessageDef/StatusIB.h> // for CHIP_IM_GLOBAL_STATUS + IM Status enum
#include <app/reporting/reporting.h>
#include <app/server/Server.h>

#include <credentials/DeviceAttestationCredsProvider.h>
#include <credentials/examples/DeviceAttestationCredsExample.h>

#include <lib/core/CHIPError.h>
#include <lib/support/CHIPMem.h>

#include <platform/CHIPDeviceLayer.h>
#include <platform/PlatformManager.h>

#include <system/SystemClock.h>
#include <system/SystemLayer.h>

using namespace chip;
using namespace chip::app;
using namespace chip::DeviceLayer;

// ---- Your custom cluster / attribute ids (from your ZAP file) -----------------
// Cluster 0xFC00 is manufacturer-specific per your XML; attribute 0x0001 chosen.
static constexpr ClusterId kDtackClusterId = static_cast<ClusterId>(0xFC00);    // :contentReference[oaicite:1]{index=1}
static constexpr AttributeId kCounterAttrId = static_cast<AttributeId>(0x0001); // :contentReference[oaicite:2]{index=2}
static constexpr EndpointId kEndpointId = 1;                                    // use endpoint 1 from your ZAP setup

// ---- Simple counter state -----------------------------------------------------
namespace
{
std::atomic<uint32_t> gCounter{ 0 };

// Encode the 32-bit counter as a 4-byte octet_string (big-endian for readability)
inline void
EncodeCounter(uint8_t out[4])
{
    uint32_t v = gCounter.load(std::memory_order_relaxed);
    out[0] = static_cast<uint8_t>((v >> 24) & 0xFF);
    out[1] = static_cast<uint8_t>((v >> 16) & 0xFF);
    out[2] = static_cast<uint8_t>((v >> 8) & 0xFF);
    out[3] = static_cast<uint8_t>(v & 0xFF);
}
} // namespace

// ---- AttributeAccessInterface for our custom attribute ------------------------
class DtackAttrAccess : public AttributeAccessInterface
{
  public:
    // Access only the attribute on our cluster across all endpoints, or bind to a specific endpoint.
    DtackAttrAccess()
      : AttributeAccessInterface(MakeOptional(kEndpointId), kDtackClusterId)
    {
    }

    CHIP_ERROR Read(const ConcreteReadAttributePath& path, AttributeValueEncoder& encoder) override
    {
        if (path.mAttributeId != kCounterAttrId)
            return CHIP_IM_GLOBAL_STATUS(UnsupportedAttribute);

        uint8_t buf[4];
        EncodeCounter(buf);
        // Attribute type in your XML is octet_string -> encode a ByteSpan.
        return encoder.Encode(ByteSpan(buf, sizeof(buf)));
    }

    CHIP_ERROR Write(const ConcreteDataAttributePath& path, AttributeValueDecoder& decoder) override
    {
        // Optional: allow writes to set the counter (accept 4-byte octet string).
        if (path.mAttributeId != kCounterAttrId)
            return CHIP_IM_GLOBAL_STATUS(UnsupportedAttribute);

        ByteSpan span;
        ReturnErrorOnFailure(decoder.Decode(span));
        if (span.size() != 4)
            return CHIP_ERROR_INVALID_ARGUMENT;

        uint32_t v = (static_cast<uint32_t>(span.data()[0]) << 24) | (static_cast<uint32_t>(span.data()[1]) << 16) |
                     (static_cast<uint32_t>(span.data()[2]) << 8) | static_cast<uint32_t>(span.data()[3]);
        gCounter.store(v, std::memory_order_relaxed);

        // Notify reporting that value changed.
        MatterReportingAttributeChangeCallback(ConcreteAttributePath(kEndpointId, kDtackClusterId, kCounterAttrId));
        return CHIP_NO_ERROR;
    }
};

static DtackAttrAccess gAttrAccess;

// ---- 1-second timer to increment and report ----------------------------------
static void
CounterTick(chip::System::Layer* layer, void* appState)
{
    // Bump the counter
    gCounter.fetch_add(1, std::memory_order_relaxed);

    // Tell the reporting engine the attribute changed
    MatterReportingAttributeChangeCallback(ConcreteAttributePath(kEndpointId, kDtackClusterId, kCounterAttrId));

    // Re-arm timer
    DeviceLayer::SystemLayer().StartTimer(System::Clock::Seconds16(1), CounterTick, nullptr);
}

// ---- Main device bring-up -----------------------------------------------------
int
main(int argc, char* argv[])
{
    // Initialize CHIP memory
    VerifyOrDie(CHIP_NO_ERROR == Platform::MemoryInit());

    // Set example DACs (replace with real credentials for production)
    Credentials::SetDeviceAttestationCredentialsProvider(Credentials::Examples::GetExampleDACProvider());

    // (Optional) Set fixed commissioning codes — defaults work with chip-tool’s test values.
    // Using VendorId 0xFFF1 (test), ProductId 0x8000, Passcode 20202021, Discriminator 3840
    // These are the same defaults chip-tool examples use.

    // Initialize the device/platform stack
    VerifyOrDie(CHIP_NO_ERROR == PlatformMgr().InitChipStack());

    // Register our attribute access override before Server::Init()
    // #if DT_HAS_ATTR_REGISTRY
    //     chip::app::AttributeAccessInterfaceRegistry::Instance().Register(&gAttrAccess);
    // #else
    //     // Legacy API
    //     registerAttributeAccessOverride(&gAttrAccess);
    // #endif

    // Start the Matter server (advertises over mDNS, opens CASE port 5540)
    {
        chip::CommonCaseDeviceServerInitParams initParams;
        VerifyOrDie(CHIP_NO_ERROR == chip::Server::GetInstance().Init(initParams));
    }

    // Start the 1-second periodic update
    VerifyOrDie(CHIP_NO_ERROR == DeviceLayer::SystemLayer().StartTimer(System::Clock::Seconds16(1), CounterTick, nullptr));

    // Run the event loop
    PlatformMgr().RunEventLoop();
    return 0;
}
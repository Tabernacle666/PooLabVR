#include "Core.h"
#include "ConsoleSpec.h"
namespace lab::ConsoleSpec {
uint32_t PhysicalCoreCount() {
    DWORD bytes = 0;
    if (GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &bytes) ||
        GetLastError() != ERROR_INSUFFICIENT_BUFFER || !bytes)
        Fail("Cannot query physical CPU cores");
    std::vector<std::byte> storage(bytes);
    auto* info = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(storage.data());
    if (!GetLogicalProcessorInformationEx(RelationProcessorCore, info, &bytes))
        Fail("Cannot query physical CPU cores");
    uint32_t count = 0;
    for (DWORD offset = 0; offset < bytes;) {
        auto* entry = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(storage.data() + offset);
        if (bytes - offset < sizeof(LOGICAL_PROCESSOR_RELATIONSHIP) + sizeof(DWORD) ||
            entry->Size < sizeof(LOGICAL_PROCESSOR_RELATIONSHIP) + sizeof(DWORD) || entry->Size > bytes - offset)
            Fail("Invalid physical CPU topology");
        if (entry->Relationship == RelationProcessorCore) ++count;
        offset += entry->Size;
    }
    return count;
}
void ValidateCpu() {
    if (PhysicalCoreCount() < MinPhysicalCores)
        Fail("LaboVR console requires 6 physical CPU cores");
}
}

// Compile and link only: the result is a PlayStation-target ELF, not a host app.
#include <cstdint>

struct Descriptor {
    std::uint32_t tone : 4;
    std::uint32_t program : 2;
    std::uint32_t volume : 4;
    std::uint32_t channel : 4;
    std::uint32_t bank : 2;

    constexpr std::uint32_t Pack() const {
        return tone | (program << 4) | (volume << 6) |
               (channel << 10) | (bank << 14);
    }
};

consteval Descriptor MakeDescriptor() {
    return {.tone = 10, .program = 1, .volume = 11, .channel = 1, .bank = 2};
}

static_assert(MakeDescriptor().Pack() == 0x86DA);

extern "C" std::uint64_t toolchainSmoke(std::uint64_t value, std::uint64_t divisor) {
    // 64-bit division on MIPS I requires the target libgcc runtime.
    return value / divisor + MakeDescriptor().Pack();
}

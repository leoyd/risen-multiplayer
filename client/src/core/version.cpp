#include "version.h"

#include <cstdio>
#include <cstring>

namespace mp {

namespace {

constexpr SupportedVersion kSupported[] = {
    {"Steam x64 (janvier 2023)", {0x63c9aff6u, 0x01417000u}, {0x63ca945du, 0x00d3a000u}},
};

std::uint16_t rd16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

std::uint32_t rd32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

}  // namespace

const SupportedVersion* find_supported_version(const ModuleFingerprint& game,
                                               const ModuleFingerprint& engine) {
    for (const auto& v : kSupported) {
        if (v.game == game && v.engine == engine) return &v;
    }
    return nullptr;
}

bool read_pe_fingerprint(const std::uint8_t* image, std::size_t size, ModuleFingerprint& out) {
    if (image == nullptr || size < 0x40) return false;
    if (image[0] != 'M' || image[1] != 'Z') return false;

    const std::uint32_t pe = rd32(image + 0x3c);
    // Signature (4) + IMAGE_FILE_HEADER (20) + OptionalHeader jusqu'à SizeOfImage (0x3c).
    if (pe > size || size - pe < 4 + 20 + 0x3c) return false;
    if (std::memcmp(image + pe, "PE\0\0", 4) != 0) return false;

    const std::uint8_t* file_header = image + pe + 4;
    if (rd16(file_header + 0) != 0x8664) return false;  // IMAGE_FILE_MACHINE_AMD64

    const std::uint8_t* optional = file_header + 20;
    if (rd16(optional + 0) != 0x20b) return false;  // PE32+

    out.time_date_stamp = rd32(file_header + 4);
    out.size_of_image = rd32(optional + 0x38);
    return true;
}

std::string format_fingerprint(const ModuleFingerprint& fp) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "TimeDateStamp=0x%08x SizeOfImage=0x%x",
                  static_cast<unsigned>(fp.time_date_stamp), static_cast<unsigned>(fp.size_of_image));
    return buf;
}

}  // namespace mp

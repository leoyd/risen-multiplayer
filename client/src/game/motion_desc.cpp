#include "game/motion_desc.h"

#include <cstring>

namespace mp::game {

namespace {

// x64 et le jeu sont little-endian : une copie d'octets suffit.
template <typename T>
void get(const std::uint8_t* desc, std::size_t offset, T& out) {
    std::memcpy(&out, desc + offset, sizeof out);
}

template <typename T>
void put(std::uint8_t* desc, std::size_t offset, const T& v) {
    std::memcpy(desc + offset, &v, sizeof v);
}

}  // namespace

void read_play_desc(const std::uint8_t (&desc)[kPlayDescSize], protocol::Motion& m) {
    get(desc, 0x00, m.d00);
    get(desc, 0x04, m.d04);
    get(desc, 0x08, m.d08);
    get(desc, 0x0C, m.d0c);
    get(desc, 0x10, m.d10);
    get(desc, 0x14, m.d14);
    get(desc, 0x18, m.d18);
}

void build_play_desc(const protocol::Motion& m, std::uint8_t (&out)[kPlayDescSize]) {
    std::memset(out, 0, kPlayDescSize);
    put(out, 0x00, m.d00);
    put(out, 0x04, m.d04);
    put(out, 0x08, m.d08);
    put(out, 0x0C, m.d0c);
    put(out, 0x10, m.d10);
    put(out, 0x14, m.d14);
    put(out, 0x18, m.d18);
}

protocol::Motion motion_from_call(int type, bool flag, const std::uint8_t (*desc)[kPlayDescSize],
                                  std::uint32_t ms) {
    protocol::Motion m;
    m.ms = ms;
    m.type = static_cast<std::uint8_t>(type);
    m.flags = static_cast<std::uint8_t>((flag ? protocol::kMotionFlagPlay : 0) |
                                        (desc != nullptr ? protocol::kMotionFlagDesc : 0));
    if (desc != nullptr) read_play_desc(*desc, m);
    return m;
}

const void* replay_desc(const protocol::Motion& m, std::uint8_t (&buf)[kPlayDescSize]) {
    if ((m.flags & protocol::kMotionFlagDesc) == 0) return nullptr;
    build_play_desc(m, buf);
    return buf;
}

bool replay_flag(const protocol::Motion& m) { return (m.flags & protocol::kMotionFlagPlay) != 0; }

}  // namespace mp::game

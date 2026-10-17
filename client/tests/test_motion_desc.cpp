#include "game/motion_desc.h"
#include "test.h"

#include <cstdint>
#include <cstring>

using mp::game::build_play_desc;
using mp::game::kPlayDescReadSize;
using mp::game::kPlayDescSize;
using mp::game::read_play_desc;
using mp::protocol::Motion;

namespace {

std::uint32_t bits(float f) {
    std::uint32_t u;
    std::memcpy(&u, &f, sizeof u);
    return u;
}

}  // namespace

TEST("descripteur : scalaires → 0x60 octets → scalaires") {
    Motion m;
    m.d00 = 0.25f;
    m.d04 = -0.0f;
    m.d08 = 1;
    m.d0c = 0.8f;
    m.d10 = 0xdeadbeefu;
    m.d14 = 1.5f;
    m.d18 = 2;
    std::uint8_t desc[kPlayDescSize];
    std::memset(desc, 0xCC, sizeof desc);  // tout doit être réécrit
    build_play_desc(m, desc);
    for (std::size_t i = kPlayDescReadSize; i < kPlayDescSize; ++i) CHECK_EQ(desc[i], 0);

    Motion back;
    back.net_id = 9;  // champs hors descripteur intacts
    read_play_desc(desc, back);
    CHECK_EQ(back.net_id, 9u);
    CHECK_EQ(bits(back.d00), bits(m.d00));
    CHECK_EQ(bits(back.d04), bits(m.d04));
    CHECK_EQ(back.d08, m.d08);
    CHECK_EQ(bits(back.d0c), bits(m.d0c));
    CHECK_EQ(back.d10, m.d10);
    CHECK_EQ(bits(back.d14), bits(m.d14));
    CHECK_EQ(back.d18, m.d18);
}

TEST("descripteur : offsets du moteur") {
    std::uint8_t raw[kPlayDescSize] = {};
    const float f00 = 0.3f, f04 = 0.6f, f0c = 1.0f, f14 = 0.5f;
    const std::uint32_t u08 = 1, u10 = 42, u18 = 2;
    std::memcpy(raw + 0x00, &f00, 4);
    std::memcpy(raw + 0x04, &f04, 4);
    std::memcpy(raw + 0x08, &u08, 4);
    std::memcpy(raw + 0x0C, &f0c, 4);
    std::memcpy(raw + 0x10, &u10, 4);
    std::memcpy(raw + 0x14, &f14, 4);
    std::memcpy(raw + 0x18, &u18, 4);
    for (std::size_t i = kPlayDescReadSize; i < kPlayDescSize; ++i) raw[i] = 0x5A;  // pointeurs ignorés

    Motion m;
    read_play_desc(raw, m);
    CHECK_EQ(bits(m.d00), bits(f00));
    CHECK_EQ(bits(m.d04), bits(f04));
    CHECK_EQ(m.d08, u08);
    CHECK_EQ(bits(m.d0c), bits(f0c));
    CHECK_EQ(m.d10, u10);
    CHECK_EQ(bits(m.d14), bits(f14));
    CHECK_EQ(m.d18, u18);

    std::uint8_t rebuilt[kPlayDescSize];
    build_play_desc(m, rebuilt);
    CHECK(std::memcmp(rebuilt, raw, kPlayDescReadSize) == 0);
}

TEST("descripteur : PlayMotion capturé → Motion → arguments de rejeu") {
    using mp::game::motion_from_call;
    using mp::game::replay_desc;
    using mp::game::replay_flag;
    namespace proto = mp::protocol;
    std::uint8_t raw[kPlayDescSize] = {};
    const float f00 = 0.2f;
    std::memcpy(raw, &f00, 4);
    raw[0x30] = 0x77;  // octet non lu par le moteur : ne doit pas revenir
    std::uint8_t buf[kPlayDescSize];

    // flag seul, sans descripteur : descripteur nul au rejeu, scalaires à zéro.
    Motion a = motion_from_call(3, true, nullptr, 5);
    CHECK_EQ(a.type, 3);
    CHECK_EQ(a.ms, 5u);
    CHECK_EQ(a.flags, proto::kMotionFlagPlay);
    CHECK_EQ(bits(a.d00), 0u);
    CHECK(replay_desc(a, buf) == nullptr);
    CHECK(replay_flag(a));

    // descripteur seul : reconstruit, flag faux.
    Motion b = motion_from_call(0, false, &raw, 0);
    CHECK_EQ(b.flags, proto::kMotionFlagDesc);
    CHECK_EQ(bits(b.d00), bits(f00));
    const void* d = replay_desc(b, buf);
    CHECK(d == buf);
    CHECK(std::memcmp(buf, raw, kPlayDescReadSize) == 0);
    CHECK_EQ(buf[0x30], 0);
    CHECK(!replay_flag(b));

    // les deux.
    Motion c = motion_from_call(4, true, &raw, 0);
    CHECK_EQ(c.flags, proto::kMotionFlagPlay | proto::kMotionFlagDesc);
    CHECK(replay_desc(c, buf) == buf);
    CHECK(replay_flag(c));
}

#include "core/version.h"
#include "test.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

using mp::ModuleFingerprint;

namespace {

constexpr ModuleFingerprint kGame{0x63c9aff6u, 0x01417000u};
constexpr ModuleFingerprint kEngine{0x63ca945du, 0x00d3a000u};

void wr16(std::vector<std::uint8_t>& b, std::size_t off, std::uint16_t v) {
    b[off] = static_cast<std::uint8_t>(v);
    b[off + 1] = static_cast<std::uint8_t>(v >> 8);
}

void wr32(std::vector<std::uint8_t>& b, std::size_t off, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b[off + i] = static_cast<std::uint8_t>(v >> (8 * i));
}

// En-tête PE32+ x86-64 minimal.
std::vector<std::uint8_t> fake_pe(std::uint32_t stamp, std::uint32_t image_size) {
    std::vector<std::uint8_t> b(0x200, 0);
    b[0] = 'M';
    b[1] = 'Z';
    const std::uint32_t pe = 0x80;
    wr32(b, 0x3c, pe);
    std::memcpy(b.data() + pe, "PE\0\0", 4);
    wr16(b, pe + 4, 0x8664);
    wr32(b, pe + 4 + 4, stamp);
    wr16(b, pe + 24, 0x20b);
    wr32(b, pe + 24 + 0x38, image_size);
    return b;
}

}  // namespace

TEST("version : empreinte connue") {
    const auto* v = mp::find_supported_version(kGame, kEngine);
    CHECK(v != nullptr);
}

TEST("version : Game.dll inconnue") {
    CHECK(mp::find_supported_version({0x12345678u, kGame.size_of_image}, kEngine) == nullptr);
    CHECK(mp::find_supported_version({kGame.time_date_stamp, 0x1000u}, kEngine) == nullptr);
}

TEST("version : Engine.dll inconnue") {
    CHECK(mp::find_supported_version(kGame, {0u, 0u}) == nullptr);
}

TEST("version : modules inversés refusés") {
    CHECK(mp::find_supported_version(kEngine, kGame) == nullptr);
}

TEST("pe : lecture d'un en-tête synthétique") {
    const auto img = fake_pe(0x63c9aff6u, 0x01417000u);
    ModuleFingerprint fp;
    CHECK(mp::read_pe_fingerprint(img.data(), img.size(), fp));
    CHECK(fp == kGame);
}

TEST("pe : en-têtes invalides rejetés") {
    ModuleFingerprint fp;
    CHECK(!mp::read_pe_fingerprint(nullptr, 0x200, fp));

    auto img = fake_pe(1, 2);
    CHECK(!mp::read_pe_fingerprint(img.data(), 0x20, fp));  // tronqué

    auto bad_mz = img;
    bad_mz[0] = 'X';
    CHECK(!mp::read_pe_fingerprint(bad_mz.data(), bad_mz.size(), fp));

    auto bad_sig = img;
    bad_sig[0x80] = 'X';
    CHECK(!mp::read_pe_fingerprint(bad_sig.data(), bad_sig.size(), fp));

    auto pe32 = img;
    wr16(pe32, 0x80 + 24, 0x10b);  // PE32 (32 bits)
    CHECK(!mp::read_pe_fingerprint(pe32.data(), pe32.size(), fp));

    auto i386 = img;
    wr16(i386, 0x80 + 4, 0x14c);
    CHECK(!mp::read_pe_fingerprint(i386.data(), i386.size(), fp));

    auto far_pe = img;
    wr32(far_pe, 0x3c, 0xfffffff0u);  // e_lfanew hors du tampon
    CHECK(!mp::read_pe_fingerprint(far_pe.data(), far_pe.size(), fp));
}

TEST("pe : format_fingerprint") {
    CHECK_EQ(mp::format_fingerprint(kGame), std::string("TimeDateStamp=0x63c9aff6 SizeOfImage=0x1417000"));
}

// Test d'intégration optionnel : lit les vraies DLL si risen_ori/ est présent (non versionné).
// RISEN_BIN_DIR n'est défini par CMake que si <RISEN_BIN_DIR>/Game.dll existe.
#ifdef RISEN_BIN_DIR
TEST("pe : vraies DLL du jeu") {
    auto load = [](const char* name, std::vector<std::uint8_t>& out) {
        std::ifstream f(std::string(RISEN_BIN_DIR) + "/" + name, std::ios::binary);
        if (!f) return false;
        out.assign(0x1000, 0);
        f.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
        out.resize(static_cast<std::size_t>(f.gcount()));
        return true;
    };
    std::vector<std::uint8_t> game, engine;
    if (!load("Game.dll", game) || !load("Engine.dll", engine)) {
        SKIP("Game.dll ou Engine.dll illisible dans " RISEN_BIN_DIR);
    }
    ModuleFingerprint g, e;
    CHECK(mp::read_pe_fingerprint(game.data(), game.size(), g));
    CHECK(mp::read_pe_fingerprint(engine.data(), engine.size(), e));
    CHECK(mp::find_supported_version(g, e) != nullptr);
}
#else
TEST("pe : vraies DLL du jeu") { SKIP("RISEN_BIN_DIR/Game.dll absent à la configuration"); }
#endif

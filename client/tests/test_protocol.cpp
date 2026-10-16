#include "core/protocol.h"
#include "test.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace mp::protocol;
using Bytes = std::vector<std::uint8_t>;

namespace {

#ifndef MP_VECTORS_DIR
#error "MP_VECTORS_DIR doit pointer vers protocol/vectors"
#endif

Bytes load_vector(const char* name) {
    const std::string path = std::string(MP_VECTORS_DIR) + "/" + name;
    Bytes out;
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) {
        std::printf("  vecteur introuvable : %s\n", path.c_str());
        ++::mptest::failures();
        return out;
    }
    std::uint8_t buf[512];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.insert(out.end(), buf, buf + n);
    std::fclose(f);
    return out;
}

// Copie exacte dans un tampon alloué à la taille près : sous ASan/valgrind, toute
// lecture au-delà serait détectée.
std::unique_ptr<std::uint8_t[]> exact_copy(const Bytes& b, std::size_t n) {
    std::unique_ptr<std::uint8_t[]> p(new std::uint8_t[n == 0 ? 1 : n]);
    for (std::size_t i = 0; i < n; ++i) p[i] = b[i];
    return p;
}

Hello sample_hello() {
    Hello h;
    h.mod_version = "0.1.0";
    h.fingerprint[kGame] = {0x63c9aff6u, 0x01417000u};
    h.fingerprint[kEngine] = {0x63ca945du, 0x00d3a000u};
    h.fingerprint[kScript] = {0x63c9b014u, 0x004d1000u};
    h.fingerprint[kSharedBase] = {0x63c569b6u, 0x0025a000u};
    h.nickname = "Leo";
    h.equipment = "It_Sword_01";
    return h;
}

bool same(const Hello& a, const Hello& b) {
    return a.protocol_version == b.protocol_version && a.mod_version == b.mod_version &&
           a.fingerprint == b.fingerprint && a.nickname == b.nickname && a.equipment == b.equipment;
}

// Comparaison bit à bit : distingue -0.0f de 0.0f.
std::uint32_t bits(float f) {
    std::uint32_t u;
    std::memcpy(&u, &f, sizeof u);
    return u;
}

bool same(const Welcome& a, const Welcome& b) {
    return a.conv == b.conv && bits(a.x) == bits(b.x) && bits(a.y) == bits(b.y) &&
           bits(a.z) == bits(b.z) && bits(a.yaw) == bits(b.yaw);
}

Decoded<ControlMessage> dc(const Bytes& b) { return decode_control(b.data(), b.size()); }
Decoded<ReliableMessage> dr(const Bytes& b) { return decode_reliable(b.data(), b.size()); }

Bytes enc(const ControlMessage& m) {
    Bytes out;
    CHECK(encode_control(m, out));
    return out;
}

Bytes enc(const ReliableMessage& m) {
    Bytes out;
    CHECK(encode_reliable(m, out));
    return out;
}

const char* const kValidControl[] = {"hello.bin", "welcome.bin", "reject.bin"};
const char* const kValidReliable[] = {"heartbeat.bin", "session_end.bin"};

}  // namespace

// --- Aller-retour ---------------------------------------------------------------

TEST("protocole : aller-retour Hello") {
    const Hello h = sample_hello();
    const auto d = dc(enc(h));
    CHECK(d.ok());
    const Hello* got = std::get_if<Hello>(&d.message);
    CHECK(got != nullptr && same(*got, h));
}

TEST("protocole : aller-retour Hello, textes de longueur maximale") {
    Hello h = sample_hello();
    h.mod_version.assign(kMaxModVersion, 'v');
    h.nickname = std::string(kMaxNickname, 'n');
    h.equipment.assign(kMaxEquipment, 'e');
    const Bytes b = enc(h);
    CHECK_EQ(b.size(), std::size_t{581});
    const auto d = dc(b);
    CHECK(d.ok());
    const Hello* got = std::get_if<Hello>(&d.message);
    CHECK(got != nullptr && same(*got, h));
}

TEST("protocole : aller-retour Hello, textes minimaux et UTF-8") {
    Hello h = sample_hello();
    h.mod_version.clear();
    h.nickname = "\xC3\xA9";  // « é », 2 octets
    h.equipment.clear();
    const Bytes b = enc(h);
    CHECK_EQ(b.size(), std::size_t{41});
    const auto d = dc(b);
    CHECK(d.ok());
    const Hello* got = std::get_if<Hello>(&d.message);
    CHECK(got != nullptr && same(*got, h));
}

TEST("protocole : aller-retour Welcome, Reject, Heartbeat, SessionEnd") {
    const Welcome w{0xdeadbeefu, -1.25f, 3.5e6f, -0.0f, -3.14159f};
    const auto dw = dc(enc(w));
    CHECK(dw.ok());
    const Welcome* gw = std::get_if<Welcome>(&dw.message);
    CHECK(gw != nullptr && same(*gw, w));

    for (int reason = 0; reason <= 255; ++reason) {
        const auto dj = dc(enc(Reject{static_cast<std::uint8_t>(reason)}));
        const Reject* gj = std::get_if<Reject>(&dj.message);
        CHECK(dj.ok() && gj != nullptr && gj->reason == reason);

        const auto de = dr(enc(SessionEnd{static_cast<std::uint8_t>(reason)}));
        const SessionEnd* ge = std::get_if<SessionEnd>(&de.message);
        CHECK(de.ok() && ge != nullptr && ge->reason == reason);
    }

    const auto dh = dr(enc(Heartbeat{}));
    CHECK(dh.ok() && std::holds_alternative<Heartbeat>(dh.message));
}

// --- Vecteurs -------------------------------------------------------------------

TEST("protocole : vecteur hello.bin") {
    const Bytes b = load_vector("hello.bin");
    const auto d = dc(b);
    CHECK(d.ok());
    const Hello* got = std::get_if<Hello>(&d.message);
    CHECK(got != nullptr && same(*got, sample_hello()));
    CHECK(enc(sample_hello()) == b);
    if (got != nullptr) CHECK(enc(*got) == b);
}

TEST("protocole : vecteur welcome.bin") {
    const Bytes b = load_vector("welcome.bin");
    const Welcome expected{0x12345678u, 100.0f, -50.5f, 2000.0f, 1.5f};
    const auto d = dc(b);
    CHECK(d.ok());
    const Welcome* got = std::get_if<Welcome>(&d.message);
    CHECK(got != nullptr && same(*got, expected));
    CHECK(enc(expected) == b);
    if (got != nullptr) CHECK(enc(*got) == b);
}

TEST("protocole : vecteur reject.bin") {
    const Bytes b = load_vector("reject.bin");
    const auto d = dc(b);
    const Reject* got = std::get_if<Reject>(&d.message);
    CHECK(d.ok() && got != nullptr && got->reason == static_cast<std::uint8_t>(RejectReason::ModVersion));
    CHECK(enc(Reject{2}) == b);
}

TEST("protocole : vecteur heartbeat.bin") {
    const Bytes b = load_vector("heartbeat.bin");
    const auto d = dr(b);
    CHECK(d.ok() && std::holds_alternative<Heartbeat>(d.message));
    CHECK(enc(Heartbeat{}) == b);
}

TEST("protocole : vecteur session_end.bin") {
    const Bytes b = load_vector("session_end.bin");
    const auto d = dr(b);
    const SessionEnd* got = std::get_if<SessionEnd>(&d.message);
    CHECK(d.ok() && got != nullptr && got->reason == static_cast<std::uint8_t>(EndReason::PlayerLeft));
    CHECK(enc(SessionEnd{1}) == b);
}

// --- Autre version --------------------------------------------------------------

TEST("protocole : Hello v2, version lisible mais décodage refusé") {
    const Bytes b = load_vector("hello_v2.bin");
    std::uint16_t version = 0;
    CHECK(peek_hello_version(b.data(), b.size(), version));
    CHECK_EQ(version, 2);
    CHECK(dc(b).error == DecodeError::UnsupportedVersion);
    // Seuls les 4 premiers octets sont figés : ils suffisent à peek_hello_version.
    CHECK(peek_hello_version(b.data(), 4, version));
    CHECK(!peek_hello_version(b.data(), 3, version));
}

TEST("protocole : peek_hello_version sur v1 et sur autre chose qu'un Hello") {
    std::uint16_t version = 0;
    const Bytes hello = load_vector("hello.bin");
    CHECK(peek_hello_version(hello.data(), hello.size(), version));
    CHECK_EQ(version, 1);
    const Bytes trunc = load_vector("hello_truncated.bin");
    CHECK(peek_hello_version(trunc.data(), trunc.size(), version));
    CHECK_EQ(version, 1);

    version = 77;
    const Bytes welcome = load_vector("welcome.bin");
    CHECK(!peek_hello_version(welcome.data(), welcome.size(), version));
    const Bytes other_channel = {9, 1, 1, 0};
    CHECK(!peek_hello_version(other_channel.data(), other_channel.size(), version));
    CHECK(!peek_hello_version(nullptr, 0, version));
    CHECK_EQ(version, 77);  // inchangée en cas d'échec
}

TEST("protocole : l'encodeur refuse une version autre que 1") {
    Hello h = sample_hello();
    h.protocol_version = 2;
    Bytes out = {0xAA};
    CHECK(!encode_control(h, out));
    CHECK(out == Bytes{0xAA});  // rien écrit
}

// --- Tronqué ----------------------------------------------------------------------

TEST("protocole : chaque vecteur valide coupé à chaque longueur -> Truncated") {
    for (const char* name : kValidControl) {
        const Bytes b = load_vector(name);
        for (std::size_t n = 0; n < b.size(); ++n) {
            const auto p = exact_copy(b, n);
            const auto d = decode_control(p.get(), n);
            if (d.error != DecodeError::Truncated) {
                std::printf("  %s coupé à %zu : %s\n", name, n, decode_error_name(d.error));
                ++::mptest::failures();
            }
        }
    }
    for (const char* name : kValidReliable) {
        const Bytes b = load_vector(name);
        for (std::size_t n = 0; n < b.size(); ++n) {
            const auto p = exact_copy(b, n);
            const auto d = decode_reliable(p.get(), n);
            if (d.error != DecodeError::Truncated) {
                std::printf("  %s coupé à %zu : %s\n", name, n, decode_error_name(d.error));
                ++::mptest::failures();
            }
        }
    }
    CHECK(decode_control(nullptr, 0).error == DecodeError::Truncated);
    CHECK(decode_reliable(nullptr, 0).error == DecodeError::Truncated);
}

TEST("protocole : vecteur hello_truncated.bin -> Truncated") {
    CHECK(dc(load_vector("hello_truncated.bin")).error == DecodeError::Truncated);
}

// --- Octets en trop ---------------------------------------------------------------

TEST("protocole : chaque vecteur valide + 1 octet -> TrailingBytes") {
    for (const char* name : kValidControl) {
        Bytes b = load_vector(name);
        b.push_back(0x00);
        CHECK(dc(b).error == DecodeError::TrailingBytes);
    }
    for (const char* name : kValidReliable) {
        Bytes b = load_vector(name);
        b.push_back(0x00);
        CHECK(dr(b).error == DecodeError::TrailingBytes);
    }
}

// --- Inconnu ----------------------------------------------------------------------

TEST("protocole : canal et type inconnus") {
    CHECK(dc(Bytes{9, 1}).error == DecodeError::UnknownChannel);
    CHECK(dc(Bytes{9}).error == DecodeError::UnknownChannel);
    CHECK(dc(Bytes{0, 16}).error == DecodeError::UnknownChannel);  // segment KCP, pas du contrôle
    CHECK(dc(load_vector("unknown_type.bin")).error == DecodeError::UnknownType);
    CHECK(dr(Bytes{99}).error == DecodeError::UnknownType);
    // Types réservés (18 à 25) et types hors de leur canal.
    for (std::uint8_t t = 18; t <= 25; ++t) CHECK(dr(Bytes{t}).error == DecodeError::UnknownType);
    CHECK(dc(Bytes{2, 16}).error == DecodeError::UnknownType);  // Heartbeat en contrôle
    CHECK(dr(Bytes{1, 1, 0}).error == DecodeError::UnknownType);  // Hello en fiable
    CHECK(dr(Bytes{0}).error == DecodeError::UnknownType);
}

// --- Texte hors bornes ------------------------------------------------------------

TEST("protocole : pseudo de 33 octets -> BadLength (vecteur)") {
    CHECK(dc(load_vector("hello_long_nickname.bin")).error == DecodeError::BadLength);
}

TEST("protocole : pseudo vide -> BadLength au décodage") {
    Bytes b = load_vector("hello.bin");
    // Remplace `03 'L' 'e' 'o'` (offset 0x2a) par `00`.
    b.erase(b.begin() + 0x2b, b.begin() + 0x2e);
    b[0x2a] = 0;
    CHECK(dc(b).error == DecodeError::BadLength);
}

TEST("protocole : l'encodeur refuse les textes hors bornes") {
    Bytes out = {0xAA};
    Hello h = sample_hello();
    h.nickname.clear();
    CHECK(!encode_control(h, out));
    h.nickname = std::string(kMaxNickname + 1, 'n');
    CHECK(!encode_control(h, out));
    h = sample_hello();
    h.equipment.assign(kMaxEquipment + 1, 'e');
    CHECK(!encode_control(h, out));
    h = sample_hello();
    h.mod_version.assign(kMaxModVersion + 1, 'v');
    CHECK(!encode_control(h, out));
    CHECK(out == Bytes{0xAA});
}

// --- Raison inconnue --------------------------------------------------------------

TEST("protocole : Reject raison 200 décodé, valeur brute conservée") {
    const auto d = dc(Bytes{2, 3, 200});
    const Reject* got = std::get_if<Reject>(&d.message);
    CHECK(d.ok() && got != nullptr && got->reason == 200);
    CHECK_EQ(std::string(reject_reason_name(200)), std::string("inconnue"));
    const auto e = dr(Bytes{17, 200});
    const SessionEnd* ge = std::get_if<SessionEnd>(&e.message);
    CHECK(e.ok() && ge != nullptr && ge->reason == 200);
    CHECK_EQ(std::string(end_reason_name(200)), std::string("inconnue"));
}

TEST("protocole : noms lisibles des raisons") {
    CHECK_EQ(std::string(reject_reason_name(1)), std::string("version du protocole différente"));
    CHECK_EQ(std::string(reject_reason_name(4)), std::string("session pleine"));
    CHECK_EQ(std::string(end_reason_name(3)), std::string("mort de l'hôte"));
    CHECK_EQ(std::string(decode_error_name(DecodeError::Truncated)), std::string("message tronqué"));
}

// --- Valeurs figées ---------------------------------------------------------------

TEST("protocole : valeurs figées du contrat") {
    CHECK_EQ(kProtocolVersion, 1);
    CHECK_EQ(static_cast<int>(Channel::Reliable), 0);
    CHECK_EQ(static_cast<int>(Channel::Snapshot), 1);
    CHECK_EQ(static_cast<int>(Channel::Control), 2);
    CHECK_EQ(static_cast<int>(MsgType::Hello), 1);
    CHECK_EQ(static_cast<int>(MsgType::Welcome), 2);
    CHECK_EQ(static_cast<int>(MsgType::Reject), 3);
    CHECK_EQ(static_cast<int>(MsgType::Heartbeat), 16);
    CHECK_EQ(static_cast<int>(MsgType::SessionEnd), 17);
    CHECK_EQ(static_cast<int>(RejectReason::Protocol), 1);
    CHECK_EQ(static_cast<int>(RejectReason::BadHello), 5);
    CHECK_EQ(static_cast<int>(EndReason::PlayerLeft), 1);
    CHECK_EQ(static_cast<int>(EndReason::InternalError), 4);
}

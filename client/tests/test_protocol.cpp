#include "core/protocol.h"
#include "test.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <limits>
#include <cmath>
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

Motion sample_motion() {
    Motion m;
    m.net_id = 2;
    m.ms = 1234;
    m.type = 0;
    m.ani_id = 7;
    m.flags = kMotionFlagPlay | kMotionFlagDesc;
    m.d00 = 0.25f;
    m.d04 = 0.5f;
    m.d08 = 1;
    m.d0c = 1.0f;
    m.d10 = 0;
    m.d14 = 0.75f;
    m.d18 = 2;
    return m;
}

bool same(const Motion& a, const Motion& b) {
    return a.net_id == b.net_id && a.ms == b.ms && a.type == b.type && a.ani_id == b.ani_id &&
           a.flags == b.flags && bits(a.d00) == bits(b.d00) && bits(a.d04) == bits(b.d04) && a.d08 == b.d08 &&
           bits(a.d0c) == bits(b.d0c) && a.d10 == b.d10 && bits(a.d14) == bits(b.d14) && a.d18 == b.d18;
}

Spawn sample_spawn() {
    Spawn m;
    m.net_id = 2;
    m.type = kSpawnTypeHero;
    m.x = 100.0f;
    m.y = -50.5f;
    m.z = 2000.0f;
    m.yaw = 1.5f;
    return m;
}

bool same(const Spawn& a, const Spawn& b) {
    return a.net_id == b.net_id && a.type == b.type && bits(a.x) == bits(b.x) && bits(a.y) == bits(b.y) &&
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
const char* const kValidReliable[] = {"heartbeat.bin", "session_end.bin", "motion.bin", "spawn.bin",
                                      "despawn.bin",   "aniname.bin",     "death.bin",
                                      "motion_param.bin", "weapon.bin", "weapon_sheathed.bin"};

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

TEST("protocole : aller-retour Motion, valeurs extrêmes") {
    Motion m;
    m.net_id = 0xffffffffu;
    m.ms = 0x89abcdefu;
    m.type = kMotionTypeCount - 1;
    m.ani_id = 0xffff;
    m.flags = kMotionFlagDesc;
    m.d00 = -0.0f;
    m.d04 = 3.5e6f;
    m.d08 = 0xffffffffu;
    m.d0c = -1.0f;
    m.d10 = 0x12345678u;
    m.d14 = 1e-30f;
    m.d18 = 7;
    const auto d = dr(enc(m));
    const Motion* got = std::get_if<Motion>(&d.message);
    CHECK(d.ok() && got != nullptr && same(*got, m));
    CHECK_EQ(enc(m).size(), static_cast<std::size_t>(41));
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

TEST("protocole : vecteur motion.bin") {
    const Bytes b = load_vector("motion.bin");
    const auto d = dr(b);
    const Motion* got = std::get_if<Motion>(&d.message);
    CHECK(d.ok() && got != nullptr && same(*got, sample_motion()));
    CHECK(enc(sample_motion()) == b);
    if (got != nullptr) CHECK(enc(*got) == b);
}

TEST("protocole : vecteur spawn.bin") {
    const Bytes b = load_vector("spawn.bin");
    const auto d = dr(b);
    const Spawn* got = std::get_if<Spawn>(&d.message);
    CHECK(d.ok() && got != nullptr && same(*got, sample_spawn()));
    CHECK(enc(sample_spawn()) == b);
}

TEST("protocole : vecteur despawn.bin") {
    const Bytes b = load_vector("despawn.bin");
    const auto d = dr(b);
    const Despawn* got = std::get_if<Despawn>(&d.message);
    CHECK(d.ok() && got != nullptr && got->net_id == 2);
    CHECK(enc(Despawn{2}) == b);
}

TEST("protocole : Spawn d'un type inconnu décodé, valeur brute conservée") {
    Spawn m = sample_spawn();
    m.type = 200;
    m.net_id = 0xFFFFFFFFu;
    const auto d = dr(enc(m));
    const Spawn* got = std::get_if<Spawn>(&d.message);
    CHECK(d.ok() && got != nullptr && same(*got, m));
}

// --- Champ hors domaine -----------------------------------------------------------

TEST("protocole : Motion de type 8 -> BadValue (vecteur)") {
    CHECK(dr(load_vector("motion_bad_type.bin")).error == DecodeError::BadValue);
}

TEST("protocole : Motion avec un bit de flags inconnu -> BadValue") {
    for (int bit = 2; bit < 8; ++bit) {
        Bytes b = load_vector("motion.bin");
        b[0x0c] = static_cast<std::uint8_t>(b[0x0c] | (1u << bit));
        CHECK(dr(b).error == DecodeError::BadValue);
    }
}

TEST("protocole : Motion sans descripteur, scalaires nuls exigés") {
    Motion m;
    m.flags = kMotionFlagPlay;  // pas de descripteur
    const auto d = dr(enc(m));
    const Motion* got = std::get_if<Motion>(&d.message);
    CHECK(d.ok() && got != nullptr && got->flags == kMotionFlagPlay);
    // Un scalaire non nul sans le bit 1 : refusé au décodage (d14 à l'offset 0x21).
    Bytes b = enc(m);
    b[0x24] = 0x3f;
    CHECK(dr(b).error == DecodeError::BadValue);
    Bytes out = {0xAA};
    m.d00 = -0.0f;  // bit de signe : non nul
    CHECK(!encode_reliable(m, out));
    CHECK(out == Bytes{0xAA});
}

TEST("protocole : l'encodeur refuse un Motion hors domaine") {
    Bytes out = {0xAA};
    Motion m = sample_motion();
    m.type = kMotionTypeCount;
    CHECK(!encode_reliable(m, out));
    m = sample_motion();
    m.flags = 0x04;
    CHECK(!encode_reliable(m, out));
    CHECK(out == Bytes{0xAA});
}

// --- Autre version --------------------------------------------------------------

TEST("protocole : Hello d'une version future, version lisible mais décodage refusé") {
    const Bytes b = load_vector("hello_future.bin");
    std::uint16_t version = 0;
    CHECK(peek_hello_version(b.data(), b.size(), version));
    CHECK_EQ(version, 0xffff);
    CHECK(dc(b).error == DecodeError::UnsupportedVersion);
    // Seuls les 4 premiers octets sont figés : ils suffisent à peek_hello_version.
    CHECK(peek_hello_version(b.data(), 4, version));
    CHECK(!peek_hello_version(b.data(), 3, version));
}

TEST("protocole : peek_hello_version sur v7 et sur autre chose qu'un Hello") {
    std::uint16_t version = 0;
    const Bytes hello = load_vector("hello.bin");
    CHECK(peek_hello_version(hello.data(), hello.size(), version));
    CHECK_EQ(version, 7);
    const Bytes trunc = load_vector("hello_truncated.bin");
    CHECK(peek_hello_version(trunc.data(), trunc.size(), version));
    CHECK_EQ(version, 7);

    version = 77;
    const Bytes welcome = load_vector("welcome.bin");
    CHECK(!peek_hello_version(welcome.data(), welcome.size(), version));
    const Bytes other_channel = {9, 1, 1, 0};
    CHECK(!peek_hello_version(other_channel.data(), other_channel.size(), version));
    CHECK(!peek_hello_version(nullptr, 0, version));
    CHECK_EQ(version, 77);  // inchangée en cas d'échec
}

TEST("protocole : l'encodeur refuse une version autre que 7") {
    Hello h = sample_hello();
    h.protocol_version = 6;
    Bytes out = {0xAA};
    CHECK(!encode_control(h, out));
    h.protocol_version = 8;
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
    // Types réservés (22, 23, 25) et types hors de leur canal.
    for (std::uint8_t t : {std::uint8_t{22}, std::uint8_t{23}, std::uint8_t{25}}) {
        CHECK(dr(Bytes{t}).error == DecodeError::UnknownType);
    }
    CHECK(dc(Bytes{2, 19}).error == DecodeError::UnknownType);  // Motion en contrôle
    CHECK(dc(Bytes{2, 20}).error == DecodeError::UnknownType);  // Spawn en contrôle
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
    CHECK_EQ(std::string(decode_error_name(DecodeError::BadValue)), std::string("champ hors domaine"));
}

// --- Valeurs figées ---------------------------------------------------------------

TEST("protocole : valeurs figées du contrat") {
    CHECK_EQ(kProtocolVersion, 7);
    CHECK_EQ(static_cast<int>(Channel::Reliable), 0);
    CHECK_EQ(static_cast<int>(Channel::Snapshot), 1);
    CHECK_EQ(static_cast<int>(Channel::Control), 2);
    CHECK_EQ(static_cast<int>(MsgType::Hello), 1);
    CHECK_EQ(static_cast<int>(MsgType::Welcome), 2);
    CHECK_EQ(static_cast<int>(MsgType::Reject), 3);
    CHECK_EQ(static_cast<int>(MsgType::Heartbeat), 16);
    CHECK_EQ(static_cast<int>(MsgType::SessionEnd), 17);
    CHECK_EQ(static_cast<int>(MsgType::Motion), 19);
    CHECK_EQ(static_cast<int>(MsgType::Spawn), 20);
    CHECK_EQ(static_cast<int>(MsgType::Despawn), 21);
    CHECK_EQ(static_cast<int>(MsgType::AniName), 18);
    CHECK_EQ(static_cast<int>(MsgType::Death), 24);
    CHECK_EQ(static_cast<int>(MsgType::MotionParam), 26);
    CHECK_EQ(static_cast<int>(MsgType::Weapon), 27);
    CHECK_EQ(kHostNetId, 1u);
    CHECK_EQ(kGuestNetId, 2u);
    CHECK_EQ(static_cast<int>(RejectReason::Protocol), 1);
    CHECK_EQ(static_cast<int>(RejectReason::BadHello), 5);
    CHECK_EQ(static_cast<int>(EndReason::PlayerLeft), 1);
    CHECK_EQ(static_cast<int>(EndReason::InternalError), 4);
}

// --- Snapshot (story 2.3, canal 1) -------------------------------------------------

namespace {

Snapshot sample_snapshot() {
    Snapshot s;
    s.tick = 1234;
    SnapshotEntity e;
    e.net_id = 2;
    e.x = 100.0f;
    e.y = -50.5f;
    e.z = 2000.0f;
    e.yaw = 1.5f;
    e.hp = 75;
    s.entities.push_back(e);
    return s;
}

bool same(const Snapshot& a, const Snapshot& b) {
    if (a.tick != b.tick || a.entities.size() != b.entities.size()) return false;
    for (std::size_t i = 0; i < a.entities.size(); ++i) {
        const SnapshotEntity &x = a.entities[i], &y = b.entities[i];
        if (x.net_id != y.net_id || bits(x.x) != bits(y.x) || bits(x.y) != bits(y.y) || bits(x.z) != bits(y.z) ||
            bits(x.yaw) != bits(y.yaw) || x.hp != y.hp) {
            return false;
        }
    }
    return true;
}

Decoded<Snapshot> ds(const Bytes& b) { return decode_snapshot(b.data(), b.size()); }

}  // namespace

TEST("protocole : vecteur snapshot.bin") {
    const Bytes b = load_vector("snapshot.bin");
    const auto d = ds(b);
    CHECK(d.ok() && same(d.message, sample_snapshot()));
    Bytes out;
    CHECK(encode_snapshot(sample_snapshot(), out));
    CHECK(out == b);
}

TEST("protocole : snapshot_truncated.bin → Truncated ; coupé, + 1 octet, autre canal") {
    CHECK(ds(load_vector("snapshot_truncated.bin")).error == DecodeError::Truncated);
    const Bytes b = load_vector("snapshot.bin");
    for (std::size_t n = 0; n < b.size(); ++n) {
        CHECK(ds(Bytes(b.begin(), b.begin() + static_cast<std::ptrdiff_t>(n))).error == DecodeError::Truncated);
    }
    Bytes longer = b;
    longer.push_back(0);
    CHECK(ds(longer).error == DecodeError::TrailingBytes);
    Bytes other = b;
    other[0] = 2;
    CHECK(ds(other).error == DecodeError::UnknownChannel);
    CHECK(ds(Bytes{}).error == DecodeError::Truncated);
}

TEST("protocole : snapshot vide, 54 entités (1194 octets), 55 refusées") {
    Snapshot s;
    s.tick = 7;
    Bytes out;
    CHECK(encode_snapshot(s, out));
    CHECK_EQ(out.size(), 6u);
    CHECK(ds(out).ok() && ds(out).message.entities.empty());
    s.entities.resize(kMaxSnapshotEntities);
    for (std::size_t i = 0; i < s.entities.size(); ++i) s.entities[i].net_id = static_cast<std::uint32_t>(i);
    CHECK(encode_snapshot(s, out));
    CHECK_EQ(out.size(), 1194u);
    CHECK(ds(out).ok() && same(ds(out).message, s));
    s.entities.resize(kMaxSnapshotEntities + 1);
    Bytes kept = {0xAA};
    CHECK(!encode_snapshot(s, kept));
    CHECK(kept == Bytes{0xAA});
}

TEST("protocole : snapshot annonçant 55 entités → BadValue au décodage") {
    Snapshot s;
    s.entities.resize(kMaxSnapshotEntities);
    Bytes b;
    CHECK(encode_snapshot(s, b));
    b[5] = static_cast<std::uint8_t>(kMaxSnapshotEntities + 1);
    b.insert(b.end(), kSnapshotEntitySize, 0);
    CHECK(ds(b).error == DecodeError::BadValue);
}

// --- AniName et Death (story 2.4) ------------------------------------------------------

TEST("protocole : vecteurs aniname.bin, death.bin, aniname_empty.bin") {
    const Bytes a = load_vector("aniname.bin");
    const auto da = dr(a);
    const AniName* got = std::get_if<AniName>(&da.message);
    CHECK(da.ok() && got != nullptr && got->ani_id == 7 &&
          got->name == "Hero_Stand_None_None_P0_Move_Run_N_Fwd_00_%_00_P0_400");
    if (got != nullptr) CHECK(enc(*got) == a);
    const Bytes d = load_vector("death.bin");
    const auto dd = dr(d);
    const Death* gd = std::get_if<Death>(&dd.message);
    CHECK(dd.ok() && gd != nullptr && gd->net_id == 2);
    CHECK(enc(Death{2}) == d);
    CHECK(dr(load_vector("aniname_empty.bin")).error == DecodeError::BadLength);
}

TEST("protocole : AniName — 255 octets acceptés, vide ou 256 refusés à l'encodage") {
    AniName m;
    m.ani_id = 0xFFFF;
    m.name.assign(kMaxAniName, 'a');
    const auto d = dr(enc(m));
    const AniName* got = std::get_if<AniName>(&d.message);
    CHECK(d.ok() && got != nullptr && got->ani_id == 0xFFFF && got->name == m.name);
    Bytes out = {0xAA};
    m.name.clear();
    CHECK(!encode_reliable(m, out));
    m.name.assign(kMaxAniName + 1, 'a');
    CHECK(!encode_reliable(m, out));
    CHECK(out == Bytes{0xAA});
}

// --- MotionParam (story 2.4b) ---------------------------------------------------------

TEST("protocole : vecteurs motion_param.bin et motion_param_bad.bin") {
    const Bytes b = load_vector("motion_param.bin");
    const auto d = dr(b);
    const MotionParam* got = std::get_if<MotionParam>(&d.message);
    CHECK(d.ok() && got != nullptr && got->net_id == 2 && got->ms == 1234 && got->type == 0 && got->kind == 0 &&
          got->value == 1.0f && got->time == 0.25f);
    if (got != nullptr) CHECK(enc(*got) == b);
    CHECK(dr(load_vector("motion_param_bad.bin")).error == DecodeError::BadValue);
}

TEST("protocole : MotionParam — type 8, nature 2, NaN ou Inf refusés (encodage et décodage)") {
    MotionParam m;
    m.kind = static_cast<std::uint8_t>(MotionParamKind::PlaySpeed);
    m.value = 0.5f;
    Bytes out = {0xAA};
    m.type = 8;
    CHECK(!encode_reliable(m, out));
    m.type = 7;
    m.kind = 2;
    CHECK(!encode_reliable(m, out));
    m.kind = 1;
    m.value = std::nanf("");
    CHECK(!encode_reliable(m, out));
    m.value = 0.5f;
    m.time = std::numeric_limits<float>::infinity();
    CHECK(!encode_reliable(m, out));
    CHECK(out == Bytes{0xAA});
    m.time = 0.0f;
    const auto d = dr(enc(m));
    CHECK(d.ok() && std::get_if<MotionParam>(&d.message) != nullptr);
    Bytes nan = load_vector("motion_param.bin");
    const float f = std::nanf("");
    std::memcpy(nan.data() + 0x0b, &f, 4);
    CHECK(dr(nan).error == DecodeError::BadValue);
}

TEST("protocole : MotionParam — domaine : [0, 100], durée [0, 60], vitesse sans durée ; décodage aussi") {
    auto bad_decoded = [](float value, float time, std::uint8_t kind, std::uint8_t type) {
        Bytes b = load_vector("motion_param.bin");
        b[0x09] = type;
        b[0x0a] = kind;
        std::memcpy(b.data() + 0x0b, &value, 4);
        std::memcpy(b.data() + 0x0f, &time, 4);
        return decode_reliable(b.data(), b.size()).error == DecodeError::BadValue;
    };
    CHECK(bad_decoded(1.0f, 0.25f, 0, 8));
    CHECK(bad_decoded(1.0f, std::numeric_limits<float>::infinity(), 0, 0));
    CHECK(bad_decoded(-0.5f, 0.25f, 0, 0));
    CHECK(bad_decoded(101.0f, 0.25f, 0, 0));
    CHECK(bad_decoded(1.0f, 61.0f, 0, 0));
    CHECK(bad_decoded(1.0f, 0.1f, 1, 0));  // vitesse avec durée
    CHECK(!bad_decoded(1.8f, 0.0f, 1, 7));
    CHECK(!bad_decoded(1.0f, 0.3f, 2, 0));   // fondu activé
    CHECK(!bad_decoded(0.0f, 0.2f, 3, 1));   // arrêt
    CHECK(bad_decoded(0.5f, 0.3f, 2, 0));    // fondu : valeur 0 ou 1
    CHECK(bad_decoded(2.0f, 0.2f, 3, 0));    // arrêt : valeur 0 ou 1
    CHECK(bad_decoded(1.0f, 0.2f, 4, 0));    // nature inconnue
    CHECK(decode_control(Bytes{2, 26}.data(), 2).error == DecodeError::UnknownType);
}

// --- Weapon (story 3.2) ----------------------------------------------------------------

TEST("protocole : vecteurs weapon.bin et weapon_sheathed.bin") {
    const Bytes b = load_vector("weapon.bin");
    const auto d = dr(b);
    const Weapon* got = std::get_if<Weapon>(&d.message);
    CHECK(d.ok() && got != nullptr && got->net_id == 2 && got->name == "It_1H_Sword");
    CHECK(enc(Weapon{2, "It_1H_Sword"}) == b);
    if (got != nullptr) CHECK(enc(*got) == b);
    const Bytes e = load_vector("weapon_sheathed.bin");
    const auto de = dr(e);
    const Weapon* ge = std::get_if<Weapon>(&de.message);
    CHECK(de.ok() && ge != nullptr && ge->net_id == 2 && ge->name.empty());
    CHECK(enc(Weapon{2, ""}) == e);
}

TEST("protocole : Weapon — 255 octets acceptés, 256 refusés à l'encodage ; tronqué au décodage") {
    Weapon m;
    m.net_id = 0xFFFFFFFFu;
    m.name.assign(kMaxWeaponName, 'w');
    const Bytes b = enc(m);
    CHECK_EQ(b.size(), std::size_t{261});
    const auto d = dr(b);
    const Weapon* got = std::get_if<Weapon>(&d.message);
    CHECK(d.ok() && got != nullptr && got->net_id == m.net_id && got->name == m.name);
    Bytes out = {0xAA};
    m.name.assign(kMaxWeaponName + 1, 'w');
    CHECK(!encode_reliable(m, out));
    CHECK(out == Bytes{0xAA});
    // Longueur annoncée plus grande que la suite : Truncated.
    Bytes cut = load_vector("weapon.bin");
    cut[0x05] = 0xFF;
    CHECK(dr(cut).error == DecodeError::Truncated);
    CHECK(decode_control(Bytes{2, 27}.data(), 2).error == DecodeError::UnknownType);  // pas en contrôle
}

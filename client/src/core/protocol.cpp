#include "core/protocol.h"

#include "core/config.h"

#include <cstring>

namespace mp::protocol {

static_assert(kMaxNickname == mp::kMaxNicknameLength,
              "le pseudo accepté par la config doit être encodable dans Hello");

namespace {

// --- Écriture -----------------------------------------------------------------

class Writer {
public:
    explicit Writer(std::vector<std::uint8_t>& out) : out_(out) {}

    void u8(std::uint8_t v) { out_.push_back(v); }
    void u16(std::uint16_t v) {
        out_.push_back(static_cast<std::uint8_t>(v));
        out_.push_back(static_cast<std::uint8_t>(v >> 8));
    }
    void u32(std::uint32_t v) {
        for (int shift = 0; shift < 32; shift += 8) out_.push_back(static_cast<std::uint8_t>(v >> shift));
    }
    void f32(float v) {
        static_assert(sizeof(float) == sizeof(std::uint32_t), "f32 IEEE 754 attendu");
        std::uint32_t bits;
        std::memcpy(&bits, &v, sizeof bits);
        u32(bits);
    }
    // Bornes vérifiées par l'appelant avant toute écriture.
    void str8(const std::string& s) {
        u8(static_cast<std::uint8_t>(s.size()));
        out_.insert(out_.end(), s.begin(), s.end());
    }

private:
    std::vector<std::uint8_t>& out_;
};

bool in_bounds(const std::string& s, std::size_t min, std::size_t max) {
    return s.size() >= min && s.size() <= max;
}

bool valid(const Hello& m) {
    return m.protocol_version == kProtocolVersion && in_bounds(m.mod_version, 0, kMaxModVersion) &&
           in_bounds(m.nickname, kMinNickname, kMaxNickname) && in_bounds(m.equipment, 0, kMaxEquipment);
}

void write(Writer& w, const Hello& m) {
    w.u8(static_cast<std::uint8_t>(MsgType::Hello));
    w.u16(m.protocol_version);
    w.str8(m.mod_version);
    for (const ModuleFingerprint& fp : m.fingerprint) {
        w.u32(fp.time_date_stamp);
        w.u32(fp.size_of_image);
    }
    w.str8(m.nickname);
    w.str8(m.equipment);
}

void write(Writer& w, const Welcome& m) {
    w.u8(static_cast<std::uint8_t>(MsgType::Welcome));
    w.u32(m.conv);
    w.f32(m.x);
    w.f32(m.y);
    w.f32(m.z);
    w.f32(m.yaw);
}

void write(Writer& w, const Reject& m) {
    w.u8(static_cast<std::uint8_t>(MsgType::Reject));
    w.u8(m.reason);
}

void write(Writer& w, const Heartbeat&) { w.u8(static_cast<std::uint8_t>(MsgType::Heartbeat)); }

void write(Writer& w, const SessionEnd& m) {
    w.u8(static_cast<std::uint8_t>(MsgType::SessionEnd));
    w.u8(m.reason);
}

std::uint32_t bits_of(float v) {
    std::uint32_t bits;
    std::memcpy(&bits, &v, sizeof bits);
    return bits;
}

bool valid(const Motion& m) {
    if (m.type >= kMotionTypeCount || (m.flags & ~(kMotionFlagPlay | kMotionFlagDesc)) != 0) return false;
    // Sans descripteur, les 7 scalaires sont nuls (PROTOCOL.md §4).
    return (m.flags & kMotionFlagDesc) != 0 ||
           (bits_of(m.d00) | bits_of(m.d04) | m.d08 | bits_of(m.d0c) | m.d10 | bits_of(m.d14) | m.d18) == 0;
}

void write(Writer& w, const Motion& m) {
    w.u8(static_cast<std::uint8_t>(MsgType::Motion));
    w.u32(m.net_id);
    w.u32(m.ms);
    w.u8(m.type);
    w.u16(m.ani_id);
    w.u8(m.flags);
    w.f32(m.d00);
    w.f32(m.d04);
    w.u32(m.d08);
    w.f32(m.d0c);
    w.u32(m.d10);
    w.f32(m.d14);
    w.u32(m.d18);
}

void write(Writer& w, const Spawn& m) {
    w.u8(static_cast<std::uint8_t>(MsgType::Spawn));
    w.u32(m.net_id);
    w.u8(m.type);
    w.f32(m.x);
    w.f32(m.y);
    w.f32(m.z);
    w.f32(m.yaw);
}

void write(Writer& w, const Despawn& m) {
    w.u8(static_cast<std::uint8_t>(MsgType::Despawn));
    w.u32(m.net_id);
}

// --- Lecture ------------------------------------------------------------------

// Curseur borné : chaque lecture vérifie la place restante et lève le drapeau
// `truncated` au lieu de déborder. Les lectures suivantes rendent alors 0.
class Reader {
public:
    Reader(const std::uint8_t* data, std::size_t size) : data_(data), size_(data ? size : 0) {}

    bool truncated() const { return truncated_; }

    std::uint8_t u8() {
        if (!take(1)) return 0;
        return data_[pos_ - 1];
    }
    std::uint16_t u16() {
        if (!take(2)) return 0;
        const std::uint8_t* p = data_ + pos_ - 2;
        return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
    }
    std::uint32_t u32() {
        if (!take(4)) return 0;
        const std::uint8_t* p = data_ + pos_ - 4;
        return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
               (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
    }
    float f32() {
        const std::uint32_t bits = u32();
        float v;
        std::memcpy(&v, &bits, sizeof v);
        return v;
    }
    // Lit un str8. Longueur hors [min, max] → bad_length (si le tampon la contient).
    void str8(std::string& out, std::size_t min, std::size_t max) {
        const std::size_t len = u8();
        if (truncated_) return;
        if (len < min || len > max) {
            bad_length_ = true;
            return;
        }
        if (!take(len)) return;
        out.assign(reinterpret_cast<const char*>(data_ + pos_ - len), len);
    }
    bool bad_length() const { return bad_length_; }

    // Erreur finale après lecture complète de la charge.
    DecodeError finish() const {
        if (truncated_) return DecodeError::Truncated;
        if (bad_length_) return DecodeError::BadLength;
        if (pos_ != size_) return DecodeError::TrailingBytes;
        return DecodeError::None;
    }

private:
    bool take(std::size_t n) {
        if (truncated_ || bad_length_) return false;
        if (n > size_ - pos_) {
            truncated_ = true;
            return false;
        }
        pos_ += n;
        return true;
    }

    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t pos_ = 0;
    bool truncated_ = false;
    bool bad_length_ = false;
};

DecodeError read(Reader& r, Hello& m) {
    m.protocol_version = r.u16();
    if (r.truncated()) return DecodeError::Truncated;
    // La suite d'un Hello d'une autre version n'est pas lisible : on s'arrête là.
    if (m.protocol_version != kProtocolVersion) return DecodeError::UnsupportedVersion;
    r.str8(m.mod_version, 0, kMaxModVersion);
    for (ModuleFingerprint& fp : m.fingerprint) {
        fp.time_date_stamp = r.u32();
        fp.size_of_image = r.u32();
    }
    r.str8(m.nickname, kMinNickname, kMaxNickname);
    r.str8(m.equipment, 0, kMaxEquipment);
    return r.finish();
}

DecodeError read(Reader& r, Welcome& m) {
    m.conv = r.u32();
    m.x = r.f32();
    m.y = r.f32();
    m.z = r.f32();
    m.yaw = r.f32();
    return r.finish();
}

DecodeError read(Reader& r, Reject& m) {
    m.reason = r.u8();
    return r.finish();
}

DecodeError read(Reader& r, Heartbeat&) { return r.finish(); }

DecodeError read(Reader& r, SessionEnd& m) {
    m.reason = r.u8();
    return r.finish();
}

DecodeError read(Reader& r, Motion& m) {
    m.net_id = r.u32();
    m.ms = r.u32();
    m.type = r.u8();
    m.ani_id = r.u16();
    m.flags = r.u8();
    m.d00 = r.f32();
    m.d04 = r.f32();
    m.d08 = r.u32();
    m.d0c = r.f32();
    m.d10 = r.u32();
    m.d14 = r.f32();
    m.d18 = r.u32();
    const DecodeError e = r.finish();
    if (e != DecodeError::None) return e;
    return valid(m) ? DecodeError::None : DecodeError::BadValue;
}

DecodeError read(Reader& r, Spawn& m) {
    m.net_id = r.u32();
    m.type = r.u8();
    m.x = r.f32();
    m.y = r.f32();
    m.z = r.f32();
    m.yaw = r.f32();
    return r.finish();
}

DecodeError read(Reader& r, Despawn& m) {
    m.net_id = r.u32();
    return r.finish();
}

template <typename Message, typename Variant>
Decoded<Variant> decode_as(Reader& r) {
    Decoded<Variant> d;
    Message m;
    d.error = read(r, m);
    if (d.ok()) d.message = std::move(m);
    return d;
}

}  // namespace

bool encode_control(const ControlMessage& message, std::vector<std::uint8_t>& out) {
    if (const Hello* hello = std::get_if<Hello>(&message); hello != nullptr && !valid(*hello)) return false;
    std::vector<std::uint8_t> buf;
    Writer w(buf);
    w.u8(static_cast<std::uint8_t>(Channel::Control));
    std::visit([&w](const auto& m) { write(w, m); }, message);
    if (buf.size() > kMaxDatagram) return false;
    out = std::move(buf);
    return true;
}

bool encode_reliable(const ReliableMessage& message, std::vector<std::uint8_t>& out) {
    if (const Motion* motion = std::get_if<Motion>(&message); motion != nullptr && !valid(*motion)) return false;
    std::vector<std::uint8_t> buf;
    Writer w(buf);
    std::visit([&w](const auto& m) { write(w, m); }, message);
    if (buf.size() > kMaxDatagram) return false;
    out = std::move(buf);
    return true;
}

Decoded<ControlMessage> decode_control(const std::uint8_t* data, std::size_t size) {
    Reader r(data, size);
    const std::uint8_t channel = r.u8();
    if (r.truncated()) return {DecodeError::Truncated, {}};
    if (channel != static_cast<std::uint8_t>(Channel::Control)) return {DecodeError::UnknownChannel, {}};
    const std::uint8_t type = r.u8();
    if (r.truncated()) return {DecodeError::Truncated, {}};
    switch (static_cast<MsgType>(type)) {
        case MsgType::Hello: return decode_as<Hello, ControlMessage>(r);
        case MsgType::Welcome: return decode_as<Welcome, ControlMessage>(r);
        case MsgType::Reject: return decode_as<Reject, ControlMessage>(r);
        default: return {DecodeError::UnknownType, {}};
    }
}

Decoded<ReliableMessage> decode_reliable(const std::uint8_t* data, std::size_t size) {
    Reader r(data, size);
    const std::uint8_t type = r.u8();
    if (r.truncated()) return {DecodeError::Truncated, {}};
    switch (static_cast<MsgType>(type)) {
        case MsgType::Heartbeat: return decode_as<Heartbeat, ReliableMessage>(r);
        case MsgType::SessionEnd: return decode_as<SessionEnd, ReliableMessage>(r);
        case MsgType::Motion: return decode_as<Motion, ReliableMessage>(r);
        case MsgType::Spawn: return decode_as<Spawn, ReliableMessage>(r);
        case MsgType::Despawn: return decode_as<Despawn, ReliableMessage>(r);
        default: return {DecodeError::UnknownType, {}};
    }
}

bool peek_hello_version(const std::uint8_t* data, std::size_t size, std::uint16_t& version) {
    Reader r(data, size);
    const std::uint8_t channel = r.u8();
    const std::uint8_t type = r.u8();
    const std::uint16_t v = r.u16();
    if (r.truncated() || channel != static_cast<std::uint8_t>(Channel::Control) ||
        type != static_cast<std::uint8_t>(MsgType::Hello)) {
        return false;
    }
    version = v;
    return true;
}

const char* reject_reason_name(std::uint8_t reason) {
    switch (static_cast<RejectReason>(reason)) {
        case RejectReason::Protocol: return "version du protocole différente";
        case RejectReason::ModVersion: return "version du mod différente";
        case RejectReason::GameFingerprint: return "empreinte du jeu différente";
        case RejectReason::SessionFull: return "session pleine";
        case RejectReason::BadHello: return "Hello invalide";
    }
    return "inconnue";
}

const char* end_reason_name(std::uint8_t reason) {
    switch (static_cast<EndReason>(reason)) {
        case EndReason::PlayerLeft: return "départ du joueur";
        case EndReason::WorldEnded: return "fin de monde";
        case EndReason::HostDied: return "mort de l'hôte";
        case EndReason::InternalError: return "erreur interne";
    }
    return "inconnue";
}

const char* decode_error_name(DecodeError error) {
    switch (error) {
        case DecodeError::None: return "aucune";
        case DecodeError::Truncated: return "message tronqué";
        case DecodeError::UnknownChannel: return "canal inconnu";
        case DecodeError::UnknownType: return "type de message inconnu";
        case DecodeError::UnsupportedVersion: return "version du protocole non prise en charge";
        case DecodeError::BadLength: return "longueur de texte hors bornes";
        case DecodeError::TrailingBytes: return "octets en trop";
        case DecodeError::BadValue: return "champ hors domaine";
    }
    return "inconnue";
}

}  // namespace mp::protocol

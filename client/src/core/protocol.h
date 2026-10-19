// Codec du contrat binaire hôte ⇄ invité, version 4 (protocol/PROTOCOL.md fait foi).
// Logique pure, sans socket ni appel au jeu : testable sous Linux.
//
// Cadrage :
// - snapshot (datagramme UDP complet, v4)        = [u8 canal=1][u32 tick][u8 nombre][entités] ;
// - message de contrôle (datagramme UDP complet) = [u8 canal=2][u8 MsgType][charge] ;
// - message fiable (unité confiée à KCP)        = [u8 MsgType][charge], sans octet de canal.
// Tout est little-endian, sérialisé champ par champ. Un texte `str8` = u8 longueur + octets UTF-8.
//
// Le décodage ne lit jamais hors du tampon et ne lève pas d'exception de son fait :
// toute entrée invalide donne une `DecodeError` typée.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include "core/version.h"

namespace mp::protocol {

inline constexpr std::uint16_t kProtocolVersion = 4;
inline constexpr std::size_t kMaxDatagram = 1200;      // octets, canal compris
inline constexpr std::size_t kMaxModVersion = 255;     // str8 : 0 à 255 octets
inline constexpr std::size_t kMinNickname = 1;
inline constexpr std::size_t kMaxNickname = 32;
inline constexpr std::size_t kMaxEquipment = 255;
inline constexpr std::size_t kFingerprintModules = 4;  // Game, Engine, Script, SharedBase

// Octet 0 de chaque datagramme UDP.
enum class Channel : std::uint8_t {
    Reliable = 0,  // segments KCP
    Snapshot = 1,  // non fiable, 20 Hz (v4, AD-7)
    Control = 2,   // hors KCP : Hello, Welcome, Reject
};

// 18 et 22 à 25 sont réservés (AniName, HitRequest, HitOnGuest, Death, Respawn) et ne sont
// pas encore définis : ils se décodent en `UnknownType`.
enum class MsgType : std::uint8_t {
    Hello = 1,
    Welcome = 2,
    Reject = 3,
    Heartbeat = 16,
    SessionEnd = 17,
    Motion = 19,   // v2 (story 2.1)
    Spawn = 20,    // v3 (story 2.2)
    Despawn = 21,  // v3 (story 2.2)
};

enum class RejectReason : std::uint8_t {
    Protocol = 1,         // version du protocole différente
    ModVersion = 2,       // version du mod différente
    GameFingerprint = 3,  // empreinte du jeu différente
    SessionFull = 4,      // un invité est déjà en session
    BadHello = 5,         // Hello illisible ou invalide
};

enum class EndReason : std::uint8_t {
    PlayerLeft = 1,     // départ du joueur
    WorldEnded = 2,     // fin de monde (rechargement, menu…)
    HostDied = 3,       // mort de l'hôte
    InternalError = 4,  // erreur interne
};

// Index dans Hello::fingerprint.
enum FingerprintModule : std::size_t { kGame = 0, kEngine = 1, kScript = 2, kSharedBase = 3 };

struct Hello {
    std::uint16_t protocol_version = kProtocolVersion;
    std::string mod_version;  // 0 à 255 octets
    std::array<ModuleFingerprint, kFingerprintModules> fingerprint{};  // Game, Engine, Script, SharedBase
    std::string nickname;     // 1 à 32 octets
    std::string equipment;    // 0 à 255 octets
};

struct Welcome {
    std::uint32_t conv = 0;  // identifiant de session KCP, tiré par l'hôte
    float x = 0.0f, y = 0.0f, z = 0.0f;  // position de l'avatar hôte
    float yaw = 0.0f;                    // radians, atan2(m[8], m[10]) (AD-7)
};

// Les raisons sont gardées brutes : un code inconnu se décode quand même,
// l'appelant le journalise (voir reject_reason_name / end_reason_name).
struct Reject {
    std::uint8_t reason = 0;
};

struct Heartbeat {};

struct SessionEnd {
    std::uint8_t reason = 0;
};

inline constexpr std::uint8_t kMotionTypeCount = 8;      // eEMotionType 0 à 7
inline constexpr std::uint8_t kMotionFlagPlay = 0x01;    // argument bool de PlayMotion
inline constexpr std::uint8_t kMotionFlagDesc = 0x02;    // descripteur présent (sinon nul)

// Un appel à eCAnimation_PS::PlayMotion (AD-8). Les 7 scalaires sont les seuls octets du
// descripteur eSMotionPlayBackDesc lus par le moteur, nommés par leur offset
// (docs/re/spike-j1.md). Sans kMotionFlagDesc, ils valent 0 et le descripteur est nul.
struct Motion {
    std::uint32_t net_id = 0;
    std::uint32_t ms = 0;     // millisecondes depuis l'entrée en InSession de l'émetteur
    std::uint8_t type = 0;    // 0 à 7
    std::uint16_t ani_id = 0; // nom interné (AniName, story 2.4)
    std::uint8_t flags = 0;   // kMotionFlagPlay | kMotionFlagDesc, autres bits à 0
    float d00 = 0.0f;
    float d04 = 0.0f;
    std::uint32_t d08 = 0;
    float d0c = 0.0f;
    std::uint32_t d10 = 0;
    float d14 = 0.0f;
    std::uint32_t d18 = 0;
};

// Identifiants d'entité attribués par l'hôte (AD-6).
inline constexpr std::uint32_t kHostNetId = 1;   // avatar de l'hôte
inline constexpr std::uint32_t kGuestNetId = 2;  // avatar de l'invité
inline constexpr std::uint8_t kSpawnTypeHero = 0;  // Spawn::type : héros d'un joueur

// Apparition d'une entité dont l'émetteur a l'autorité. `type` est gardé brut : un type
// inconnu se décode, l'appelant l'ignore.
struct Spawn {
    std::uint32_t net_id = 0;
    std::uint8_t type = kSpawnTypeHero;
    float x = 0.0f, y = 0.0f, z = 0.0f;  // position (unités du monde)
    float yaw = 0.0f;                    // radians, atan2(m[8], m[10]) (AD-7)
};

// Retrait d'une entité annoncée par Spawn.
struct Despawn {
    std::uint32_t net_id = 0;
};

// Snapshot (AD-7, v4) : état complet des entités dont l'émetteur a l'autorité.
inline constexpr std::uint32_t kSnapshotTickMs = 50;      // 20 Hz
inline constexpr std::size_t kSnapshotEntitySize = 22;    // octets par entité
inline constexpr std::size_t kMaxSnapshotEntities = 54;   // (1200 − 6) / 22

struct SnapshotEntity {
    std::uint32_t net_id = 0;
    float x = 0.0f, y = 0.0f, z = 0.0f;
    float yaw = 0.0f;  // radians, atan2(m[8], m[10])
    std::uint16_t hp = 0;
};

struct Snapshot {
    std::uint32_t tick = 0;  // floor((QPC − t0) / 50 ms), t0 = entrée en InSession de l'émetteur
    std::vector<SnapshotEntity> entities;
};

using ControlMessage = std::variant<Hello, Welcome, Reject>;
using ReliableMessage = std::variant<Heartbeat, SessionEnd, Motion, Spawn, Despawn>;

enum class DecodeError : std::uint8_t {
    None = 0,
    Truncated,           // tampon plus court que le message
    UnknownChannel,      // octet de canal autre que Control (decode_control)
    UnknownType,         // MsgType inconnu, réservé ou hors de ce canal
    UnsupportedVersion,  // Hello d'une autre version du protocole (lire peek_hello_version)
    BadLength,           // longueur de texte hors bornes
    TrailingBytes,       // octets après la fin du message
    BadValue,            // champ hors de son domaine (Motion : type, bits de flags, scalaires sans descripteur)
};

template <typename Message>
struct Decoded {
    DecodeError error = DecodeError::None;
    Message message{};

    bool ok() const { return error == DecodeError::None; }
};

// Encodage : `out` est remplacé par le datagramme (contrôle) ou par le message
// (fiable). Retourne false, sans rien écrire dans `out`, si une borne n'est pas
// respectée (texte trop long ou pseudo vide, version ≠ kProtocolVersion, champ hors domaine,
// > 1200 octets).
bool encode_control(const ControlMessage& message, std::vector<std::uint8_t>& out);
bool encode_reliable(const ReliableMessage& message, std::vector<std::uint8_t>& out);

// Snapshot : datagramme complet (octet de canal 1 compris). L'encodeur refuse plus de
// kMaxSnapshotEntities entités ; le décodeur rend Truncated, UnknownChannel, TrailingBytes,
// ou BadValue pour un `nombre` > 54.
bool encode_snapshot(const Snapshot& snapshot, std::vector<std::uint8_t>& out);
Decoded<Snapshot> decode_snapshot(const std::uint8_t* data, std::size_t size);

// Décodage d'un datagramme de contrôle complet (octet de canal compris).
Decoded<ControlMessage> decode_control(const std::uint8_t* data, std::size_t size);
// Décodage d'un message fiable (sans octet de canal).
Decoded<ReliableMessage> decode_reliable(const std::uint8_t* data, std::size_t size);

// Lit la version du protocole d'un Hello de n'importe quelle version : seuls
// [canal=2][MsgType=1][u16 version] sont figés pour toujours. Retourne false si le
// datagramme n'est pas un Hello ou est trop court pour porter une version.
bool peek_hello_version(const std::uint8_t* data, std::size_t size, std::uint16_t& version);

// Noms lisibles pour le log (UTF-8). Code inconnu : « inconnue ».
const char* reject_reason_name(std::uint8_t reason);
const char* end_reason_name(std::uint8_t reason);
const char* decode_error_name(DecodeError error);

}  // namespace mp::protocol

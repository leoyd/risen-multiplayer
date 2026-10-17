// Descripteur eSMotionPlayBackDesc (0x60 octets) de eCAnimation_PS::PlayMotion (AD-8).
//
// Fait établi par désassemblage (Steam x64, janvier 2023) :
// eCWrapper_emfx3ActorInstance::PlayMotion ne lit que les octets 0x00 à 0x1B du descripteur,
// soit 7 scalaires : f32 +0x00, f32 +0x04, u32 +0x08 (seuls 0 et 1 sont retenus), f32 +0x0C,
// u32 +0x10, f32 +0x14, u32 +0x18 (comparé à 2). Le reste (pointeurs compris) n'est pas lu
// sur ce chemin. Voir docs/re/spike-j1.md.
//
// Code pur, sans appel au jeu ni dépendance Windows : testé sous Linux.
#pragma once

#include <cstddef>
#include <cstdint>

#include "core/protocol.h"

namespace mp::game {

constexpr std::size_t kPlayDescSize = 0x60;
constexpr std::size_t kPlayDescReadSize = 0x1C;  // octets lus par le moteur

// Recopie les 7 scalaires du descripteur dans d00…d18 de `m` (les autres champs sont intacts).
void read_play_desc(const std::uint8_t (&desc)[kPlayDescSize], protocol::Motion& m);

// Descripteur reconstruit : 0x60 octets à zéro, puis les 7 scalaires de `m` à leur offset.
void build_play_desc(const protocol::Motion& m, std::uint8_t (&out)[kPlayDescSize]);

// Un PlayMotion capturé → Motion : type, flags (argument `flag`, descripteur présent) et,
// si `desc` n'est pas nul, ses 7 scalaires (sinon ils restent à zéro). net_id et ani_id à 0.
protocol::Motion motion_from_call(int type, bool flag, const std::uint8_t (*desc)[kPlayDescSize],
                                  std::uint32_t ms);

// Arguments de rejeu d'un Motion : descripteur reconstruit dans `buf` (rendu) si
// kMotionFlagDesc, sinon nul (lecture par défaut du moteur) ; `flag` = kMotionFlagPlay.
const void* replay_desc(const protocol::Motion& m, std::uint8_t (&buf)[kPlayDescSize]);
bool replay_flag(const protocol::Motion& m);

}  // namespace mp::game

// Captures du héros local → entrées de core/session (story 2.4). Code portable, testé sous
// Linux : c'est ici que se fait le routage par nature (PlayMotion ou ragdoll) des captures de
// l'AnimFilter.
#pragma once

#include <optional>
#include <string>

#include "core/session.h"
#include "sync/anim_filter.h"

namespace mp::sync {

// Nom de ressource utilisable : non vide, et pas un marqueur d'erreur « (…) ».
bool usable_motion_name(const std::string& name);

// PlayMotion → LocalMotion (si le nom est utilisable et le type dans 0..7). Un ragdoll ne
// donne rien : la mort part sur Entity::IsDead (décision du 2026-10-19). `name` : titre de la
// ressource de la capture.
std::optional<core::session::Input> local_input(const Capture& capture, const std::string& name);

}  // namespace mp::sync

#include "core/overlay_text.h"
#include "test.h"
#include <cmath>
#include <string>

using mp::Role;
using mp::core::overlay_color;
using mp::core::overlay_text;
using State = mp::core::session::State;

TEST("overlay : hôte en attente, en or") {
    CHECK_EQ(overlay_text(Role::Host, "Leoyd", State::Hosting, "", 27960),
             std::string("HÔTE · Leoyd · en attente d’un invité"));
    CHECK_EQ(overlay_color(Role::Host), 0xFFFFC040u);
}

TEST("overlay : invité en connexion, en cyan") {
    CHECK_EQ(overlay_text(Role::Guest, "Invite", State::Connecting, "127.0.0.1", 27960),
             std::string("INVITÉ · Invite · connexion à 127.0.0.1:27960…"));
    CHECK_EQ(overlay_color(Role::Guest), 0xFF40D0FFu);
}

TEST("overlay : autres états") {
    CHECK_EQ(overlay_text(Role::Host, "Leoyd", State::Inactive, "", 27960), std::string("HÔTE · Leoyd · hors partie"));
    CHECK_EQ(overlay_text(Role::Guest, "Invite", State::Inactive, "1.2.3.4", 27960),
             std::string("INVITÉ · Invite · hors partie"));
    CHECK_EQ(overlay_text(Role::Guest, "Invite", State::InSession, "1.2.3.4", 4000),
             std::string("INVITÉ · Invite · en session"));
    CHECK_EQ(overlay_text(Role::Host, "Leoyd", State::Ended, "", 27960),
             std::string("HÔTE · Leoyd · session terminée"));
}

TEST("overlay 4.5 : alerte du champ de force — 0 sous 80 % du rayon, 1 au rayon, texte en mètres") {
    using mp::core::leash_warning_level;
    using mp::core::leash_warning_text;
    CHECK(leash_warning_level(2700.0f, 3500.0f) == 0.0f);
    CHECK(leash_warning_level(2800.0f, 3500.0f) == 0.0f);
    CHECK(std::fabs(leash_warning_level(3150.0f, 3500.0f) - 0.5f) < 1e-4f);
    CHECK(leash_warning_level(3500.0f, 3500.0f) == 1.0f);
    CHECK(leash_warning_level(9000.0f, 3500.0f) == 1.0f);
    CHECK(leash_warning_level(-1.0f, 3500.0f) == 0.0f);   // distance inconnue
    CHECK(leash_warning_level(3400.0f, 0.0f) == 0.0f);    // champ désactivé
    CHECK(leash_warning_text(2000.0f, 3500.0f).empty());
    CHECK_EQ(leash_warning_text(3180.0f, 3500.0f), std::string("Distance de l’hôte : 32 m / 35 m"));
}

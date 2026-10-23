#include "core/overlay_text.h"
#include "test.h"

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

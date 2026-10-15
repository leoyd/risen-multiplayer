#include "core/config.h"
#include "test.h"

using mp::parse_config;
using mp::Role;

TEST("config : nominal hôte") {
    const auto r = parse_config("role=host\nport=27960\nnickname=Leo\n");
    CHECK(r.ok);
    CHECK(r.config.role == Role::Host);
    CHECK_EQ(r.config.port, 27960);
    CHECK_EQ(r.config.nickname, std::string("Leo"));
    CHECK(r.warnings.empty());
}

TEST("config : nominal invité, commentaires, section, CRLF, BOM, casse") {
    const auto r = parse_config(
        "\xEF\xBB\xBF; commentaire\r\n"
        "[multiplayer]\r\n"
        "# autre commentaire\r\n"
        "  Role =  Guest \r\n"
        "HOST_IP = 192.168.1.20\r\n"
        "port = 4000\r\n"
        "nickname = Max le Rouge\r\n"
        "guest_equipment = It_Sword_01, It_Armor_02\r\n");
    CHECK(r.ok);
    CHECK(r.config.role == Role::Guest);
    CHECK_EQ(r.config.host_ip, std::string("192.168.1.20"));
    CHECK_EQ(r.config.port, 4000);
    CHECK_EQ(r.config.nickname, std::string("Max le Rouge"));
    CHECK_EQ(r.config.guest_equipment, std::string("It_Sword_01, It_Armor_02"));
}

TEST("config : port par défaut") {
    const auto r = parse_config("role=host");
    CHECK(r.ok);
    CHECK_EQ(r.config.port, mp::kDefaultPort);
}

TEST("config : texte vide -> role manquant") {
    const auto r = parse_config("");
    CHECK(!r.ok);
    CHECK_EQ(r.error_field, std::string("role"));
}

TEST("config : role=chef invalide") {
    const auto r = parse_config("role=chef\n");
    CHECK(!r.ok);
    CHECK_EQ(r.error_field, std::string("role"));
    CHECK_EQ(r.error_value, std::string("chef"));
}

TEST("config : port=99999 invalide") {
    const auto r = parse_config("role=host\nport=99999\n");
    CHECK(!r.ok);
    CHECK_EQ(r.error_field, std::string("port"));
    CHECK_EQ(r.error_value, std::string("99999"));
}

TEST("config : port non numérique, zéro, hors bornes") {
    for (const char* v : {"abc", "0", "-1", "123456", "80a"}) {
        const auto r = parse_config(std::string("role=host\nport=") + v + "\n");
        CHECK(!r.ok);
        CHECK_EQ(r.error_field, std::string("port"));
    }
    const auto edge = parse_config("role=host\nport=65535\n");
    CHECK(edge.ok);
    CHECK_EQ(edge.config.port, 65535);
}

TEST("config : ip vide en invité") {
    const auto r = parse_config("role=guest\nhost_ip=\n");
    CHECK(!r.ok);
    CHECK_EQ(r.error_field, std::string("host_ip"));
    CHECK_EQ(r.error_value, std::string(""));
    const auto absent = parse_config("role=guest\n");
    CHECK(!absent.ok);
    CHECK_EQ(absent.error_field, std::string("host_ip"));
}

TEST("config : ip mal formée") {
    const auto r = parse_config("role=guest\nhost_ip=1.2.3.4 ; oups\n");
    CHECK(!r.ok);
    CHECK_EQ(r.error_field, std::string("host_ip"));
}

TEST("config : ip vide acceptée en hôte") {
    const auto r = parse_config("role=host\nhost_ip=\n");
    CHECK(r.ok);
}

TEST("config : champ inconnu -> avertissement, pas d'erreur") {
    const auto r = parse_config("role=host\ncouleur=bleu\n");
    CHECK(r.ok);
    CHECK_EQ(r.warnings.size(), std::size_t{1});
    CHECK(r.warnings[0].find("couleur") != std::string::npos);
}

TEST("config : doublon -> avertissement, dernière valeur") {
    const auto r = parse_config("role=host\nport=1000\nport=2000\n");
    CHECK(r.ok);
    CHECK_EQ(r.config.port, 2000);
    CHECK_EQ(r.warnings.size(), std::size_t{1});
}

TEST("config : ligne sans '=' -> erreur nommant la ligne") {
    const auto r = parse_config("role=host\nn'importe quoi\n");
    CHECK(!r.ok);
    CHECK_EQ(r.error_field, std::string("ligne 2"));
}

TEST("config : pseudo vide ou trop long") {
    CHECK_EQ(parse_config("role=host\nnickname=\n").error_field, std::string("nickname"));
    CHECK_EQ(parse_config("role=host\nnickname=" + std::string(33, 'a') + "\n").error_field,
             std::string("nickname"));
    CHECK(parse_config("role=host\nnickname=" + std::string(32, 'a') + "\n").ok);
}

TEST("config : host_ip invalide ignoré en hôte") {
    const auto r = parse_config("role=host\nhost_ip=n'importe quoi\n");
    CHECK(r.ok);
}

TEST("config : port vide -> valeur par défaut") {
    const auto r = parse_config("role=host\nport =\n");
    CHECK(r.ok);
    CHECK_EQ(r.config.port, mp::kDefaultPort);
}

TEST("config : host_ip mal formé rejeté en invité") {
    for (const char* v : {"...", "-", ".a", "a.", "a..b", "-a.b", "a-.b", "a_b"}) {
        const auto r = parse_config(std::string("role=guest\nhost_ip=") + v + "\n");
        CHECK(!r.ok);
        CHECK_EQ(r.error_field, std::string("host_ip"));
    }
}

TEST("config : host_ip avec port -> message orientant vers « port »") {
    const auto r = parse_config("role=guest\nhost_ip=1.2.3.4:27960\n");
    CHECK(!r.ok);
    CHECK_EQ(r.error_field, std::string("host_ip"));
    CHECK(r.error_message.find("port") != std::string::npos);
}

TEST("config : host_ip valides en invité") {
    for (const char* v : {"192.168.1.20", "localhost", "mon-pc.lan", "::1", "fe80::1", "::ffff:1.2.3.4"}) {
        const auto r = parse_config(std::string("role=guest\nhost_ip=") + v + "\n");
        CHECK(r.ok);
    }
}

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

TEST("config : probe absent -> 0 par défaut") {
    const auto r = parse_config("role=host\n");
    CHECK(r.ok);
    CHECK(!r.config.probe);
}

TEST("config : probe = 0, 1, vide") {
    const auto zero = parse_config("role=host\nprobe = 0\n");
    CHECK(zero.ok);
    CHECK(!zero.config.probe);
    const auto one = parse_config("role=host\nPROBE = 1\n");
    CHECK(one.ok);
    CHECK(one.config.probe);
    const auto empty = parse_config("role=host\nprobe =\n");
    CHECK(empty.ok);
    CHECK(!empty.config.probe);
    CHECK(empty.warnings.empty());
}

TEST("config : probe invalide -> erreur nommant le champ") {
    for (const char* v : {"2", "oui", "true", "01", "-1"}) {
        const auto r = parse_config(std::string("role=host\nprobe=") + v + "\n");
        CHECK(!r.ok);
        CHECK_EQ(r.error_field, std::string("probe"));
        CHECK_EQ(r.error_value, std::string(v));
    }
}

TEST("config : log_level absent -> info par défaut") {
    const auto r = parse_config("role=host\n");
    CHECK(r.ok);
    CHECK(r.config.log_level == mp::LogLevel::Info);
    CHECK_EQ(std::string(mp::log_level_name(r.config.log_level)), std::string("info"));
}

TEST("config : log_level = debug, INFO, Debug, vide") {
    const auto debug = parse_config("role=host\nlog_level = debug\n");
    CHECK(debug.ok);
    CHECK(debug.config.log_level == mp::LogLevel::Debug);
    CHECK_EQ(std::string(mp::log_level_name(debug.config.log_level)), std::string("debug"));
    const auto info = parse_config("role=host\nLOG_LEVEL = INFO\n");
    CHECK(info.ok);
    CHECK(info.config.log_level == mp::LogLevel::Info);
    const auto mixed = parse_config("role=host\nlog_level = Debug\n");
    CHECK(mixed.ok);
    CHECK(mixed.config.log_level == mp::LogLevel::Debug);
    const auto empty = parse_config("role=host\nlog_level =\n");
    CHECK(empty.ok);
    CHECK(empty.config.log_level == mp::LogLevel::Info);
    CHECK(empty.warnings.empty());
}

TEST("config : log_level invalide -> erreur nommant le champ") {
    for (const char* v : {"trace", "warn", "1", "debug info"}) {
        const auto r = parse_config(std::string("role=host\nlog_level=") + v + "\n");
        CHECK(!r.ok);
        CHECK_EQ(r.error_field, std::string("log_level"));
        CHECK_EQ(r.error_value, std::string(v));
    }
}

#include "config.h"

#include <algorithm>
#include <cctype>
#include <set>

namespace mp {

namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

std::string to_lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

ConfigResult fail(ConfigResult r, std::string field, std::string_view value, std::string message) {
    r.ok = false;
    r.error_field = std::move(field);
    r.error_value = std::string(value);
    r.error_message = std::move(message);
    return r;
}

bool parse_port(std::string_view s, std::uint16_t& out) {
    if (s.empty() || s.size() > 5) return false;
    unsigned value = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        value = value * 10 + static_cast<unsigned>(c - '0');
    }
    if (value < 1 || value > 65535) return false;
    out = static_cast<std::uint16_t>(value);
    return true;
}

// IPv4, IPv6 ou nom d'hôte, sans validation DNS. Retourne un message d'erreur,
// ou une chaîne vide si l'adresse est acceptable.
std::string host_error(std::string_view s) {
    if (s.empty()) return "obligatoire pour un invité";
    if (s.size() > 253) return "adresse IP ou nom d'hôte attendu";
    const auto colons = std::count(s.begin(), s.end(), ':');
    if (colons == 1) return "adresse sans port attendue (le port se règle dans le champ « port »)";
    if (colons >= 2) {  // IPv6
        for (char c : s) {
            if (!std::isxdigit(static_cast<unsigned char>(c)) && c != ':' && c != '.') {
                return "adresse IP ou nom d'hôte attendu";
            }
        }
        return {};
    }
    // IPv4 ou nom d'hôte : étiquettes non vides séparées par '.', sans '-' en bordure.
    std::size_t start = 0;
    for (;;) {
        const std::size_t dot = s.find('.', start);
        const std::string_view label = s.substr(start, dot == std::string_view::npos ? s.npos : dot - start);
        if (label.empty() || label.size() > 63 || label.front() == '-' || label.back() == '-') {
            return "adresse IP ou nom d'hôte attendu";
        }
        for (char c : label) {
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-') return "adresse IP ou nom d'hôte attendu";
        }
        if (dot == std::string_view::npos) return {};
        start = dot + 1;
    }
}

bool valid_nickname(std::string_view s) {
    if (s.empty() || s.size() > kMaxNicknameLength) return false;
    for (char c : s) {
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) return false;
    }
    return true;
}

}  // namespace

const char* role_name(Role role) { return role == Role::Host ? "hôte" : "invité"; }

ConfigResult parse_config(std::string_view text) {
    ConfigResult r;
    if (text.size() >= 3 && text.substr(0, 3) == "\xEF\xBB\xBF") text.remove_prefix(3);

    bool has_role = false;
    std::set<std::string> seen;
    int line_no = 0;

    while (!text.empty()) {
        const std::size_t eol = text.find('\n');
        std::string_view line = text.substr(0, eol);
        text = (eol == std::string_view::npos) ? std::string_view{} : text.substr(eol + 1);
        ++line_no;

        line = trim(line);
        if (line.empty() || line.front() == ';' || line.front() == '#') continue;
        if (line.front() == '[' && line.back() == ']') continue;

        const std::size_t eq = line.find('=');
        if (eq == std::string_view::npos) {
            return fail(std::move(r), "ligne " + std::to_string(line_no), line,
                        "attendu « cle = valeur »");
        }
        const std::string key = to_lower(trim(line.substr(0, eq)));
        const std::string_view value = trim(line.substr(eq + 1));

        if (key.empty()) {
            return fail(std::move(r), "ligne " + std::to_string(line_no), line, "nom de champ vide");
        }
        if (!seen.insert(key).second) {
            r.warnings.push_back("champ « " + key + " » défini plusieurs fois, la dernière valeur l'emporte");
        }

        if (key == "role") {
            const std::string v = to_lower(value);
            if (v == "host") {
                r.config.role = Role::Host;
            } else if (v == "guest") {
                r.config.role = Role::Guest;
            } else {
                return fail(std::move(r), key, value, "valeurs possibles : host, guest");
            }
            has_role = true;
        } else if (key == "host_ip") {
            r.config.host_ip = std::string(value);
        } else if (key == "port") {
            if (value.empty()) {
                r.config.port = kDefaultPort;  // `port =` : valeur par défaut
            } else if (!parse_port(value, r.config.port)) {
                return fail(std::move(r), key, value, "entier attendu entre 1 et 65535");
            }
        } else if (key == "nickname") {
            if (!valid_nickname(value)) {
                return fail(std::move(r), key, value,
                            "1 à " + std::to_string(kMaxNicknameLength) + " octets (UTF-8), sans caractère de contrôle");
            }
            r.config.nickname = std::string(value);
        } else if (key == "guest_equipment") {
            r.config.guest_equipment = std::string(value);
        } else if (key == "probe") {
            if (value.empty() || value == "0") {
                r.config.probe = false;  // `probe =` : valeur par défaut
            } else if (value == "1") {
                r.config.probe = true;
            } else {
                return fail(std::move(r), key, value, "valeurs possibles : 0, 1");
            }
        } else {
            r.warnings.push_back("champ inconnu « " + key + " » ignoré");
        }
    }

    if (!has_role) return fail(std::move(r), "role", "", "champ obligatoire (host ou guest)");

    // host_ip n'est utilisé (donc validé) qu'en invité ; l'hôte l'ignore.
    if (r.config.role == Role::Guest) {
        std::string message = host_error(r.config.host_ip);
        if (!message.empty()) {
            const std::string v = r.config.host_ip;
            return fail(std::move(r), "host_ip", v, std::move(message));
        }
    }

    r.ok = true;
    return r;
}

}  // namespace mp

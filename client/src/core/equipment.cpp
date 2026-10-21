#include "core/equipment.h"

#include <cctype>

namespace mp::core {

namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

bool name_char(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
}

bool fail(std::vector<std::string>& names, std::string& error, std::string message) {
    names.clear();
    error = std::move(message);
    return false;
}

}  // namespace

bool parse_equipment(std::string_view text, std::vector<std::string>& names, std::string& error) {
    names.clear();
    error.clear();
    text = trim(text);
    if (text.empty()) return true;
    std::size_t canonical = 0;
    for (;;) {
        const std::size_t comma = text.find(',');
        const std::string_view name = trim(text.substr(0, comma));
        const std::size_t index = names.size() + 1;
        if (name.empty()) {
            return fail(names, error, "nom " + std::to_string(index) + " vide (virgule en trop ?)");
        }
        if (name.size() > kMaxEquipmentNameLength) {
            return fail(names, error, "nom " + std::to_string(index) + " trop long (" + std::to_string(name.size()) +
                                          " octets, " + std::to_string(kMaxEquipmentNameLength) + " au plus)");
        }
        for (char c : name) {
            if (!name_char(c)) {
                return fail(names, error, "nom « " + std::string(name) +
                                              " » : caractères autorisés A-Z, a-z, 0-9 et _");
            }
        }
        for (const std::string& seen : names) {
            if (seen == name) return fail(names, error, "nom « " + std::string(name) + " » en double");
        }
        if (names.size() == kMaxEquipmentNames) {
            return fail(names, error, std::to_string(kMaxEquipmentNames) + " noms au plus");
        }
        canonical += name.size() + (names.empty() ? 0 : 1);
        if (canonical > kMaxEquipmentCanonical) {
            return fail(names, error, "liste trop longue (" + std::to_string(kMaxEquipmentCanonical) +
                                          " octets au plus, sans les espaces)");
        }
        names.emplace_back(name);
        if (comma == std::string_view::npos) return true;
        text.remove_prefix(comma + 1);
    }
}

std::string join_equipment(const std::vector<std::string>& names) {
    std::string out;
    for (const std::string& n : names) {
        if (!out.empty()) out += ',';
        out += n;
    }
    return out;
}

}  // namespace mp::core

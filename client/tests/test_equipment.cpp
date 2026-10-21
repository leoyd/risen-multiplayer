#include "core/equipment.h"
#include "test.h"

using mp::core::join_equipment;
using mp::core::parse_equipment;

namespace {

std::vector<std::string> ok_names(const std::string& text) {
    std::vector<std::string> names{"reste"};
    std::string error = "reste";
    const bool ok = parse_equipment(text, names, error);
    CHECK(ok);
    CHECK(error.empty());
    return names;
}

bool rejected(const std::string& text) {
    std::vector<std::string> names{"reste"};
    std::string error;
    const bool ok = parse_equipment(text, names, error);
    if (!ok) {
        CHECK(names.empty());
        CHECK(!error.empty());
    }
    return !ok;
}

std::string name_n(int i) {
    std::string n = "Item_" + std::to_string(i);
    return n;
}

}  // namespace

TEST("équipement : valeur valide, espaces tolérés, forme canonique") {
    const auto names = ok_names("It_Armor_Don_Fighter , It_1H_Sword_Rusty");
    CHECK_EQ(names.size(), std::size_t{2});
    CHECK_EQ(names[0], std::string("It_Armor_Don_Fighter"));
    CHECK_EQ(names[1], std::string("It_1H_Sword_Rusty"));
    CHECK_EQ(join_equipment(names), std::string("It_Armor_Don_Fighter,It_1H_Sword_Rusty"));
    CHECK_EQ(join_equipment(ok_names("  \tA\t,B  ")), std::string("A,B"));
}

TEST("équipement : vide ou espaces -> aucun équipement") {
    CHECK(ok_names("").empty());
    CHECK(ok_names("   \t ").empty());
    CHECK_EQ(join_equipment({}), std::string());
}

TEST("équipement : caractères, noms vides, doublons refusés") {
    CHECK(rejected("It-Sword"));
    CHECK(rejected("A,,B"));
    CHECK(rejected(",A"));
    CHECK(rejected("A,"));
    CHECK(rejected("A, ,B"));
    CHECK(rejected("A B"));
    CHECK(rejected("Épée"));
    CHECK(rejected("A\xC2\xA0"));  // espace insécable : seuls les blancs ASCII sont retirés
    CHECK(rejected("A,B,A"));
    CHECK(!rejected("A,a"));  // la casse distingue deux noms
}

TEST("équipement : longueur d'un nom (64 octets au plus)") {
    CHECK_EQ(ok_names(std::string(64, 'x')).size(), std::size_t{1});
    CHECK(rejected(std::string(65, 'x')));
}

TEST("équipement : 16 noms au plus") {
    std::string sixteen;
    for (int i = 0; i < 16; ++i) sixteen += (i ? "," : "") + name_n(i);
    CHECK_EQ(ok_names(sixteen).size(), std::size_t{16});
    CHECK(rejected(sixteen + "," + name_n(16)));
}

TEST("équipement : forme canonique de 255 octets au plus") {
    // 3 noms de 64 + 1 nom de 61 + 3 virgules = 256 ; 60 → 255.
    const std::string a(64, 'a'), b(64, 'b'), c(64, 'c');
    const std::string at_limit = a + " , " + b + " , " + c + " , " + std::string(60, 'd');
    const auto names = ok_names(at_limit);
    CHECK_EQ(join_equipment(names).size(), std::size_t{255});
    CHECK(rejected(a + "," + b + "," + c + "," + std::string(61, 'd')));
}

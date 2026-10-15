// Mini-framework de test, sans dépendance externe.
#pragma once

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace mptest {

struct Case {
    const char* name;
    std::function<void()> fn;
};

inline std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

inline int& failures() {
    static int n = 0;
    return n;
}

// Raison du saut du test courant (vide = test exécuté).
inline std::string& skip_reason() {
    static std::string reason;
    return reason;
}

struct Registrar {
    Registrar(const char* name, std::function<void()> fn) { registry().push_back({name, std::move(fn)}); }
};

inline int run_all() {
    int failed_cases = 0;
    int skipped_cases = 0;
    for (const auto& c : registry()) {
        const int before = failures();
        skip_reason().clear();
        c.fn();
        const bool ok = failures() == before;
        if (!ok) {
            ++failed_cases;
            std::printf("[FAIL] %s\n", c.name);
        } else if (!skip_reason().empty()) {
            ++skipped_cases;
            std::printf("[SKIP] %s (%s)\n", c.name, skip_reason().c_str());
        } else {
            std::printf("[ OK ] %s\n", c.name);
        }
    }
    std::printf("%zu tests, %d en échec, %d ignorés\n", registry().size(), failed_cases, skipped_cases);
    return failed_cases == 0 ? 0 : 1;
}

}  // namespace mptest

#define MP_CAT2(a, b) a##b
#define MP_CAT(a, b) MP_CAT2(a, b)
#define TEST(name)                                                                   \
    static void MP_CAT(test_fn_, __LINE__)();                                        \
    static ::mptest::Registrar MP_CAT(test_reg_, __LINE__)(name, MP_CAT(test_fn_, __LINE__)); \
    static void MP_CAT(test_fn_, __LINE__)()

// Marque le test courant comme ignoré (non compté comme réussi) et en sort.
#define SKIP(reason)                       \
    do {                                   \
        ::mptest::skip_reason() = (reason); \
        return;                            \
    } while (0)

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("  %s:%d: CHECK(%s) a échoué\n", __FILE__, __LINE__, #cond); \
            ++::mptest::failures();                                              \
        }                                                                        \
    } while (0)

#define CHECK_EQ(a, b)                                                                     \
    do {                                                                                   \
        if (!((a) == (b))) {                                                               \
            std::printf("  %s:%d: CHECK_EQ(%s, %s) a échoué\n", __FILE__, __LINE__, #a, #b); \
            ++::mptest::failures();                                                        \
        }                                                                                  \
    } while (0)

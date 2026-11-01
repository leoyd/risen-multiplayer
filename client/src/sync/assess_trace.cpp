#include "sync/assess_trace.h"

#include "common/log.h"
#include "game/combat.h"
#include "game/dialogue.h"
#include "game/entity.h"

#include <chrono>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <tuple>

namespace mp::sync::assess_trace {

namespace {

constexpr std::size_t kRing = 8192;
constexpr auto kRepeat = std::chrono::seconds(3);

struct Call {
    std::uint16_t script = 0;
    std::uint16_t depth = 0;
    void* self = nullptr;
    void* other = nullptr;
    long long arg = 0;
    long long result = 0;
    bool done = false;
};

std::mutex g_mutex;
Call g_ring[kRing];          // sous g_mutex
std::size_t g_count = 0;     // appels de la frame
unsigned g_dropped = 0;
std::uint32_t g_generation = 0;  // jetons de la frame : (génération << 16) | index
thread_local std::uint16_t t_depth = 0;

// Thread principal.
std::string g_names[kMaxScripts];
struct Seen {
    std::chrono::steady_clock::time_point last{};
    unsigned repeats = 0;
};
std::map<std::tuple<std::uint16_t, void*, void*, long long>, Seen> g_seen;

std::string short_name(const std::string& decorated) {
    const std::size_t at = decorated.find("@@");
    return decorated.size() > 1 && at != std::string::npos ? decorated.substr(1, at - 1) : decorated;
}

std::string who(void* e, void* hero, void* avatar) {
    if (e == nullptr) return "nul";
    std::string name;
    if (!game::get_entity_name(game::Entity{e}, name) || name.empty()) name = "?";
    char buf[24];
    std::snprintf(buf, sizeof buf, "0x%llx", static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(e)));
    const char* role = e == hero ? " [héros local]" : e == avatar ? " [avatar]" : "";
    return name + " (" + buf + ")" + role;
}

}  // namespace

void set_script_name(std::size_t index, const char* name) {
    if (index < kMaxScripts && name != nullptr) g_names[index] = name;
}

std::uint32_t before(std::size_t index, void* spu, void* self, void* other, long long arg) noexcept {
    Call c;
    c.script = static_cast<std::uint16_t>(index);
    c.depth = t_depth++;
    c.arg = arg;
    if (self != nullptr ? !game::read_entity_ptr(self, c.self) : !game::spu_self_entity(spu, c.self)) c.self = nullptr;
    if (other != nullptr ? !game::read_entity_ptr(other, c.other) : !game::spu_other_entity(spu, c.other)) {
        c.other = nullptr;
    }
    try {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_count >= kRing) {
            ++g_dropped;
            return 0xffffffffu;
        }
        g_ring[g_count] = c;
        return (g_generation << 16) | static_cast<std::uint32_t>(g_count++);
    } catch (...) {
        return 0xffffffffu;
    }
}

void after(std::uint32_t token, long long result) noexcept {
    if (t_depth > 0) --t_depth;
    if (token == 0xffffffffu) return;
    try {
        std::lock_guard<std::mutex> lock(g_mutex);
        const std::size_t i = token & 0xffffu;
        if ((token >> 16) != (g_generation & 0xffffu) || i >= g_count) return;  // frame déjà vidée
        g_ring[i].result = result;
        g_ring[i].done = true;
    } catch (...) {
    }
}

void frame(void* hero, void* avatar) {
    static Call batch[kRing];
    std::size_t n = 0;
    unsigned dropped = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        n = g_count;
        for (std::size_t i = 0; i < n; ++i) batch[i] = g_ring[i];
        g_count = 0;
        dropped = g_dropped;
        g_dropped = 0;
        g_generation = (g_generation + 1) & 0xffffu;
    }
    if (dropped != 0) log::warn("[trace] Assess : " + std::to_string(dropped) + " appels perdus (anneau plein)");
    const auto now = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < n; ++i) {
        const Call& c = batch[i];
        const bool mine = (hero != nullptr && (c.self == hero || c.other == hero)) ||
                          (avatar != nullptr && (c.self == avatar || c.other == avatar));
        if (!mine) continue;
        const long long result = c.done ? c.result : -999;
        // Scans périodiques de perception (retour 0) : sans intérêt, sauf les scripts de dégâts.
        const std::string& full = c.script < kMaxScripts ? g_names[c.script] : std::string();
        const bool damage = full.find("Damage") != std::string::npos;
        if (result == 0 && !damage) continue;
        Seen& s = g_seen[std::make_tuple(c.script, c.self, c.other, result)];
        if (now - s.last < kRepeat) {
            ++s.repeats;
            continue;
        }
        const unsigned repeats = s.repeats;
        s.last = now;
        s.repeats = 0;
        const std::string name = c.script < kMaxScripts ? short_name(g_names[c.script]) : "?";
        log::info("[trace] " + std::string(c.depth * 2u, ' ') + name + "(self=" + who(c.self, hero, avatar) +
                  ", other=" + who(c.other, hero, avatar) + ", arg=" + std::to_string(c.arg) + ") -> " +
                  (c.done ? std::to_string(c.result) : std::string("?")) +
                  (repeats != 0 ? " (+" + std::to_string(repeats) + " identiques en 3 s)" : std::string()));
    }
    if (g_seen.size() > 4096) g_seen.clear();
}

}  // namespace mp::sync::assess_trace

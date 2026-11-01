#include "game/dialogue.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstring>
#include <string>

namespace mp::game {

namespace {

// Game.dll
using SpuGetSelfFn = void* (*)(void* spu);
using SStartOutputFn = bool (*)(void* entity, const void* sample, int type);
using SIsOutputPlayingFn = bool (*)(void* entity);
using HudGetSystemFn = void* (*)();
using HudShowSubtitleFn = void (*)(void* hud, const void* ustring);
// Script.dll : statiques de la façade InfoSystem (sret dans RCX), méthodes d'InfComSay (sret dans RDX).
using InfoIsRunningFn = bool (*)();
using InfoGetNpcFn = Entity* (*)(Entity* sret);
using InfoCommandTypeFn = int (*)();
using InfoCommandSayFn = void* (*)();  // InfComSay& (donnée statique s_InfComSay)
using SayTextIdFn = void* (*)(void* say, void* bcstring_sret);
using SaySpeakerFn = Entity* (*)(void* say, Entity* sret);
using StartFacialAniFn = void (*)(const Entity* self, void* bcstring_by_value);
using StartSayFn = void (*)(const Entity* self, const Entity* listener, void* text_by_value, bool show_text, bool flag);
using StartTalkFn = bool (*)(void* gui, Entity* speaker_by_value, void* text_by_value);
using StopTalkFn = void (*)(void* gui);
using EntityCopyCtorFn = Entity* (*)(Entity* self, const Entity* other);
// Engine.dll
using LocGetInstanceFn = void* (*)();
using LocGetStringFn = void* (*)(const void* admin, void* ustring_sret, const void* id);
// SharedBase.dll
using BCStringCtorFn = void* (*)(void* self, const char* text);
using BCStringDtorFn = void (*)(void* self);
using BCStringGetTextFn = const char* (*)(const void* self);
using UStringCtorFn = void* (*)(void* self, const wchar_t* text);
using UStringDtorFn = void (*)(void* self);
using UStringGetTextFn = const wchar_t* (*)(const void* self);

constexpr const char* kStartOutput = "?StartOutput@gCScriptProcessingUnit@@QEAA_NAEBVbCString@@W4gEOutputType@@@Z";
constexpr const char* kGetSelfEntity = "?GetSelfEntity@gCScriptProcessingUnit@@QEAAPEAVeCGeometryEntity@@XZ";
// Script_Game.dll, facultatif (story 4.2) : demande de dialogue reçue par le PNJ.
constexpr const char* kAssessTalk = "?AssessTalk@@YA_JPEAVgCScriptProcessingUnit@@PEAX1_J@Z";
constexpr const char* kSStartOutput =
    "?sStartOutput@gCScriptProcessingUnit@@KA_NPEAVeCGeometryEntity@@AEBVbCString@@W4gEOutputType@@@Z";
constexpr const char* kSIsOutputPlaying = "?sIsOutputPlaying@gCScriptProcessingUnit@@KA_NPEAVeCGeometryEntity@@@Z";
// Story 4.1 (test en jeu du 2026-10-24) : la voix d'une réplique n'est jouée que par
// UpdateChannel (canal audio + eCSoundResource2::Play3D), appelée depuis ProcessScript, donc depuis
// gCScriptRoutine_PS::OnProcess, que le registre saute pour les répliques de PNJ (IA figée).
constexpr const char* kSpuIsOutputPlaying = "?IsOutputPlaying@gCScriptProcessingUnit@@QEBA_NXZ";
constexpr const char* kSpuUpdateChannel = "?UpdateChannel@gCScriptProcessingUnit@@IEAAXXZ";
constexpr std::size_t kRoutineSpuOffset = 0x118;  // gCScriptProcessingUnit dans gCScriptRoutine_PS
constexpr const char* kHudGetSystem = "?GetSystem@gCHudModule2@@SAPEAVgCHudSystem2@@XZ";
constexpr const char* kHudShowSubtitle = "?ShowSubtitle@gCHudSystem2@@QEAAXAEBVbCUnicodeString@@@Z";
constexpr const char* kInfoIsRunning = "?IsRunning@InfoSystem@@SA_NXZ";
constexpr const char* kInfoGetNpc = "?GetNPC@InfoSystem@@SA?AVEntity@@XZ";
constexpr const char* kInfoCommandType = "?GetCurrentCommandType@InfoSystem@@SA?AW4gEInfoCommand@@XZ";
constexpr const char* kInfoGetCommandSay = "?GetCurrentCommandSay@InfoSystem@@SAAEAVInfComSay@@XZ";
constexpr const char* kSayTextId = "?GetTextID@InfComSay@@QEAA?AVbCString@@XZ";
constexpr const char* kSaySpeaker = "?GetSpeaker@InfComSay@@QEAA?AVEntity@@XZ";
constexpr const char* kStartFacialAni = "?StartFacialAni@Entity@@QEAAXVbCString@@@Z";
constexpr const char* kStartSay = "?StartSay@Entity@@QEAAXAEAV1@VbCString@@_N2@Z";
constexpr const char* kStartTalk = "?StartTalk@GUI2@@QEAA_NVEntity@@VbCString@@@Z";
constexpr const char* kStopTalk = "?StopTalk@GUI2@@QEAAXXZ";
// Story 4.2 (H1, diagnostic seulement, hors du bloc en tout ou rien) : `this` = Entity (8 octets),
// l'entité est lue à [this] puis GetPropertySet(0x37) (gCDialog_PS) : désassemblage, 0x180220290.
constexpr const char* kBeingTalkedTo = "?IsBeingTalkedTo@PSDialog@@QEBA_NXZ";
constexpr const char* kDialogCameraActive = "?IsDialogCameraActive@Entity@@SA_NXZ";  // facultatif (story 4.2)
// Story 4.3, facultatifs (Script.dll) : nom affiché au survol (`this` = Entity de 8 octets, l'entité est
// lue à [this] puis GetPropertySet(0x28), gCInteraction_PS : désassemblage, 0x180222750 ; retour par sret)
// et message du journal en jeu (`this` = gui2 ; type 1 comme PrintEquipMissingSkill de Script_Game.dll).
constexpr const char* kFocusName = "?GetFocusName@PSInteraction@@QEBA?AVbCUnicodeString@@XZ";
constexpr const char* kPrintGameLog = "?PrintGameLog@GUI2@@QEAAXAEBVbCUnicodeString@@W4gELogMessageType@@_NPEAD@Z";
constexpr const char* kEntityCopyCtor = "??0Entity@@QEAA@AEBV0@@Z";
constexpr const char* kGui2 = "gui2";
constexpr const char* kLocGetInstance = "?GetInstance@eCLocAdmin2@@SAAEAV1@XZ";
constexpr const char* kLocGetString = "?GetString@eCLocAdmin2@@QEBA?AVbCUnicodeString@@AEBVbCString@@@Z";
constexpr const char* kBCStringCtor = "??0bCString@@QEAA@PEBD@Z";
constexpr const char* kBCStringDtor = "??1bCString@@QEAA@XZ";
constexpr const char* kBCStringGetText = "?GetText@bCString@@QEBAPEBDXZ";
constexpr const char* kUStringCtor = "??0bCUnicodeString@@QEAA@PEB_W@Z";
constexpr const char* kUStringDtor = "??1bCUnicodeString@@QEAA@XZ";
constexpr const char* kUStringGetText = "?GetText@bCUnicodeString@@QEBAPEB_WXZ";

struct DialogueAbi {
    void* start_output = nullptr;  // cible du détour
    void* assess_talk = nullptr;   // cible du détour, facultative (story 4.2)
    SpuGetSelfFn spu_get_self = nullptr;
    SStartOutputFn s_start_output = nullptr;
    SIsOutputPlayingFn s_is_output_playing = nullptr;
    bool (*spu_is_output_playing)(const void* spu) = nullptr;
    void (*spu_update_channel)(void* spu) = nullptr;
    HudGetSystemFn hud_get_system = nullptr;
    HudShowSubtitleFn hud_show_subtitle = nullptr;
    InfoIsRunningFn info_is_running = nullptr;
    InfoGetNpcFn info_get_npc = nullptr;
    InfoCommandTypeFn info_command_type = nullptr;
    InfoCommandSayFn info_command_say = nullptr;
    SayTextIdFn say_text_id = nullptr;
    SaySpeakerFn say_speaker = nullptr;
    StartFacialAniFn start_facial_ani = nullptr;
    StartSayFn start_say = nullptr;
    StartTalkFn start_talk = nullptr;
    StopTalkFn stop_talk = nullptr;
    bool (*being_talked_to)(const Entity* self) = nullptr;  // facultatif (story 4.2)
    bool (*dialog_camera_active)() = nullptr;                // facultatif (story 4.2)
    void* (*focus_name)(const Entity* self, void* ustring_sret) = nullptr;                    // facultatif (4.3)
    void (*print_game_log)(void* gui, const void* ustring, int type, bool flag, char* extra) = nullptr;  // idem
    EntityCopyCtorFn entity_copy_ctor = nullptr;
    void* gui2 = nullptr;
    LocGetInstanceFn loc_get_instance = nullptr;
    LocGetStringFn loc_get_string = nullptr;
    BCStringCtorFn bcstring_ctor = nullptr;
    BCStringDtorFn bcstring_dtor = nullptr;
    BCStringGetTextFn bcstring_get_text = nullptr;
    UStringCtorFn ustring_ctor = nullptr;
    UStringDtorFn ustring_dtor = nullptr;
    UStringGetTextFn ustring_get_text = nullptr;
};

DialogueAbi g_abi;
bool g_resolved = false;

template <typename Fn>
bool lookup(HMODULE module, const char* name, Fn& out, const char*& missing) {
    out = reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(module, name)));
    if (out == nullptr) missing = name;
    return out != nullptr;
}

constexpr std::size_t kTextCap = 255;
constexpr std::size_t kWideCap = 511;

__declspec(noinline) std::size_t raw_copy_cstr(const char* src, char* dst, std::size_t cap) {
    std::size_t n = 0;
    if (src != nullptr) {
        while (n < cap && src[n] != '\0') {
            dst[n] = src[n];
            ++n;
        }
    }
    return n;
}

__declspec(noinline) std::size_t raw_copy_wstr(const wchar_t* src, wchar_t* dst, std::size_t cap) {
    std::size_t n = 0;
    if (src != nullptr) {
        while (n < cap && src[n] != L'\0') {
            dst[n] = src[n];
            ++n;
        }
    }
    return n;
}

// Aucun objet C++ à détruire dans un cadre __try : les chaînes du jeu sont des `void*` locaux.

bool text_id_into(char* buf, std::size_t cap, std::size_t& len) {
    len = 0;
    void* text = nullptr;  // bCString rendu par valeur (sret), détruit ici
    bool ok = false;
    __try {
        void* say = g_abi.info_command_say();
        if (say != nullptr) {
            g_abi.say_text_id(say, &text);
            len = raw_copy_cstr(g_abi.bcstring_get_text(&text), buf, cap);
            ok = true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    __try {
        if (text != nullptr) g_abi.bcstring_dtor(&text);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return ok;
}

bool loc_into(const char* text_id, wchar_t* buf, std::size_t cap, std::size_t& len) {
    len = 0;
    void* id = nullptr;    // bCString construit ici, passé par référence, détruit ici
    void* text = nullptr;  // bCUnicodeString rendu par valeur (sret), détruit ici
    bool ok = false;
    __try {
        g_abi.bcstring_ctor(&id, text_id);
        void* admin = g_abi.loc_get_instance();
        if (admin != nullptr) {
            g_abi.loc_get_string(admin, &text, &id);
            len = raw_copy_wstr(g_abi.ustring_get_text(&text), buf, cap);
            ok = true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    __try {
        if (text != nullptr) g_abi.ustring_dtor(&text);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    __try {
        if (id != nullptr) g_abi.bcstring_dtor(&id);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return ok;
}

}  // namespace

bool resolve_dialogue(const char*& missing) {
    HMODULE game = GetModuleHandleW(L"Game.dll");
    HMODULE script = GetModuleHandleW(L"Script.dll");
    HMODULE engine = GetModuleHandleW(L"Engine.dll");
    HMODULE shared_base = GetModuleHandleW(L"SharedBase.dll");
    if (game == nullptr || script == nullptr || engine == nullptr || shared_base == nullptr) {
        missing = game == nullptr     ? "Game.dll"
                  : script == nullptr ? "Script.dll"
                  : engine == nullptr ? "Engine.dll"
                                      : "SharedBase.dll";
        return false;
    }
    DialogueAbi a;
    const bool ok = lookup(game, kStartOutput, a.start_output, missing) &&
                    lookup(game, kGetSelfEntity, a.spu_get_self, missing) &&
                    lookup(game, kSStartOutput, a.s_start_output, missing) &&
                    lookup(game, kSIsOutputPlaying, a.s_is_output_playing, missing) &&
                    lookup(game, kSpuIsOutputPlaying, a.spu_is_output_playing, missing) &&
                    lookup(game, kSpuUpdateChannel, a.spu_update_channel, missing) &&
                    lookup(game, kHudGetSystem, a.hud_get_system, missing) &&
                    lookup(game, kHudShowSubtitle, a.hud_show_subtitle, missing) &&
                    lookup(script, kInfoIsRunning, a.info_is_running, missing) &&
                    lookup(script, kInfoGetNpc, a.info_get_npc, missing) &&
                    lookup(script, kInfoCommandType, a.info_command_type, missing) &&
                    lookup(script, kInfoGetCommandSay, a.info_command_say, missing) &&
                    lookup(script, kSayTextId, a.say_text_id, missing) &&
                    lookup(script, kSaySpeaker, a.say_speaker, missing) &&
                    lookup(script, kStartFacialAni, a.start_facial_ani, missing) &&
                    lookup(script, kStartSay, a.start_say, missing) &&
                    lookup(script, kStartTalk, a.start_talk, missing) &&
                    lookup(script, kStopTalk, a.stop_talk, missing) &&
                    lookup(script, kEntityCopyCtor, a.entity_copy_ctor, missing) &&
                    lookup(script, kGui2, a.gui2, missing) &&
                    lookup(engine, kLocGetInstance, a.loc_get_instance, missing) &&
                    lookup(engine, kLocGetString, a.loc_get_string, missing) &&
                    lookup(shared_base, kBCStringCtor, a.bcstring_ctor, missing) &&
                    lookup(shared_base, kBCStringDtor, a.bcstring_dtor, missing) &&
                    lookup(shared_base, kBCStringGetText, a.bcstring_get_text, missing) &&
                    lookup(shared_base, kUStringCtor, a.ustring_ctor, missing) &&
                    lookup(shared_base, kUStringDtor, a.ustring_dtor, missing) &&
                    lookup(shared_base, kUStringGetText, a.ustring_get_text, missing);
    if (!ok) return false;
    const char* optional = nullptr;
    lookup(script, kBeingTalkedTo, a.being_talked_to, optional);  // absent : dialog_being_talked_to rend false
    lookup(script, kDialogCameraActive, a.dialog_camera_active, optional);
    lookup(script, kFocusName, a.focus_name, optional);         // absent : nom générique
    lookup(script, kPrintGameLog, a.print_game_log, optional);  // absent : refus au journal seulement
    if (HMODULE script_game = GetModuleHandleW(L"Script_Game.dll"); script_game != nullptr) {
        lookup(script_game, kAssessTalk, a.assess_talk, optional);  // absent : pas de libération à la demande
    }
    g_abi = a;
    g_resolved = true;
    return true;
}

bool dialogue_resolved() { return g_resolved; }

void* start_output_address() { return g_resolved ? g_abi.start_output : nullptr; }

void* assess_talk_address() { return g_resolved ? g_abi.assess_talk : nullptr; }

// --- Détour --------------------------------------------------------------------------------

bool spu_self_entity(void* spu, void*& out) {
    out = nullptr;
    if (!g_resolved || spu == nullptr) return false;
    __try {
        out = g_abi.spu_get_self(spu);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out = nullptr;
        return false;
    }
}


bool read_bcstring(const void* bcstring, char* buf, std::size_t cap, std::size_t& len) {
    len = 0;
    if (!g_resolved || bcstring == nullptr || buf == nullptr) return false;
    __try {
        len = raw_copy_cstr(g_abi.bcstring_get_text(bcstring), buf, cap);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        len = 0;
        return false;
    }
}

// --- InfoSystem ----------------------------------------------------------------------------

bool dialog_being_talked_to(const Entity& e, bool& out) {
    out = false;
    if (!g_resolved || g_abi.being_talked_to == nullptr || e.ptr == nullptr) return false;
    __try {
        out = g_abi.being_talked_to(&e);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out = false;
        return false;
    }
}

bool dialog_camera_active(bool& out) {
    out = false;
    if (!g_resolved || g_abi.dialog_camera_active == nullptr) return false;
    __try {
        out = g_abi.dialog_camera_active();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out = false;
        return false;
    }
}

bool focus_name(const Entity& e, wchar_t* buf, std::size_t cap, std::size_t& len) {
    len = 0;
    if (!g_resolved || g_abi.focus_name == nullptr || e.ptr == nullptr || buf == nullptr) return false;
    void* text = nullptr;  // bCUnicodeString (un pointeur), rempli par l'appelé
    bool ok = false;
    __try {
        g_abi.focus_name(&e, &text);
        len = raw_copy_wstr(g_abi.ustring_get_text(&text), buf, cap);
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        len = 0;
        ok = false;
    }
    __try {
        if (text != nullptr) g_abi.ustring_dtor(&text);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return ok;
}

bool print_game_log(const wchar_t* message) {
    if (!g_resolved || g_abi.print_game_log == nullptr || g_abi.gui2 == nullptr || message == nullptr) return false;
    void* text = nullptr;
    bool ok = false;
    __try {
        g_abi.ustring_ctor(&text, message);
        g_abi.print_game_log(g_abi.gui2, &text, 1, false, nullptr);
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    __try {
        if (text != nullptr) g_abi.ustring_dtor(&text);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return ok;
}

bool info_is_running(bool& out) {
    out = false;
    if (!g_resolved) return false;
    __try {
        out = g_abi.info_is_running();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out = false;
        return false;
    }
}

bool info_dialog_npc(Entity& out) {
    out.ptr = nullptr;
    if (!g_resolved) return false;
    __try {
        g_abi.info_get_npc(&out);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out.ptr = nullptr;
        return false;
    }
}

bool info_command_type(int& out) {
    out = kInfoCommandNone;
    if (!g_resolved) return false;
    __try {
        out = g_abi.info_command_type();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out = kInfoCommandNone;
        return false;
    }
}

bool info_say_text_id(std::string& out) {
    out.clear();
    int type = kInfoCommandNone;
    if (!info_command_type(type) || type != kInfoCommandSay) return false;
    char buf[kTextCap];
    std::size_t len = 0;
    if (!text_id_into(buf, sizeof buf, len)) return false;
    out.assign(buf, len);
    return true;
}

bool info_say_speaker(Entity& out) {
    out.ptr = nullptr;
    int type = kInfoCommandNone;
    if (!info_command_type(type) || type != kInfoCommandSay) return false;
    __try {
        void* say = g_abi.info_command_say();
        if (say == nullptr) return false;
        g_abi.say_speaker(say, &out);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out.ptr = nullptr;
        return false;
    }
}

// --- Parole --------------------------------------------------------------------------------

bool pump_routine_output(void* routine_ps) {
    if (!g_resolved || routine_ps == nullptr) return false;
    __try {
        void* spu = static_cast<char*>(routine_ps) + kRoutineSpuOffset;
        if (g_abi.spu_is_output_playing(spu)) g_abi.spu_update_channel(spu);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool output_playing(const Entity& e, bool& out) {
    out = false;
    if (!g_resolved || e.ptr == nullptr) return false;
    __try {
        out = g_abi.s_is_output_playing(e.ptr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out = false;
        return false;
    }
}

bool start_output(const Entity& e, const char* sample, int type, bool& out) {
    out = false;
    if (!g_resolved || e.ptr == nullptr || sample == nullptr) return false;
    void* text = nullptr;  // bCString passé par référence : construit et détruit ici
    bool ok = false;
    __try {
        g_abi.bcstring_ctor(&text, sample);
        out = g_abi.s_start_output(e.ptr, &text, type);
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out = false;
        ok = false;
    }
    __try {
        if (text != nullptr) g_abi.bcstring_dtor(&text);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return ok;
}

bool start_facial_ani(const Entity& e, const char* sample) {
    if (!g_resolved || e.ptr == nullptr || sample == nullptr) return false;
    void* text = nullptr;  // bCString passé par valeur : l'appelé le détruit
    __try {
        g_abi.bcstring_ctor(&text, sample);
        g_abi.start_facial_ani(&e, &text);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;  // la copie fuit peut-être (rare, sonde seulement)
    }
}

bool start_say(const Entity& speaker, const Entity& listener, const char* text_id, bool show_text, bool flag) {
    if (!g_resolved || speaker.ptr == nullptr || listener.ptr == nullptr || text_id == nullptr) return false;
    void* text = nullptr;  // bCString passé par valeur : StartSay le détruit
    __try {
        g_abi.bcstring_ctor(&text, text_id);
        g_abi.start_say(&speaker, &listener, &text, show_text, flag);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// --- Sous-titres ---------------------------------------------------------------------------

bool localized_string(const char* text_id, std::wstring& out) {
    out.clear();
    if (!g_resolved || text_id == nullptr) return false;
    wchar_t buf[kWideCap];
    std::size_t len = 0;
    if (!loc_into(text_id, buf, kWideCap, len)) return false;
    out.assign(buf, len);
    return true;
}

bool show_subtitle(const wchar_t* text, void*& hud) {
    hud = nullptr;
    if (!g_resolved || text == nullptr) return false;
    void* ustring = nullptr;  // bCUnicodeString passé par référence : construit et détruit ici
    bool ok = false;
    __try {
        hud = g_abi.hud_get_system();
        if (hud != nullptr) {
            g_abi.ustring_ctor(&ustring, text);
            g_abi.hud_show_subtitle(hud, &ustring);
        }
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    __try {
        if (ustring != nullptr) g_abi.ustring_dtor(&ustring);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return ok;
}

bool start_talk(const Entity& speaker, const char* text_id, bool& out) {
    out = false;
    if (!g_resolved || speaker.ptr == nullptr || text_id == nullptr) return false;
    void* text = nullptr;  // bCString et Entity passés par valeur : copies laissées à l'appelé
    __try {
        Entity copy;
        g_abi.entity_copy_ctor(&copy, &speaker);
        g_abi.bcstring_ctor(&text, text_id);
        out = g_abi.start_talk(g_abi.gui2, &copy, &text);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out = false;
        return false;
    }
}

bool stop_talk() {
    if (!g_resolved) return false;
    __try {
        g_abi.stop_talk(g_abi.gui2);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}  // namespace mp::game

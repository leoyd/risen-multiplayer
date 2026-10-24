// Adaptateur ABI des dialogues (spike J3, story 4.0, `probe = 1` seulement). Plan et résultats :
// docs/re/spike-j3.md.
//
// Faits établis par désassemblage (Steam x64, janvier 2023) :
// - gCScriptProcessingUnit (SPU, Game.dll) vit à `+0x118` du gCScriptRoutine_PS (PS 0x24) de
//   l'entité : `sStartOutput(entité, échantillon, type)` et `sIsOutputPlaying(entité)` font
//   GetPropertySet(0x24) puis appellent la méthode de l'unité (rendent false sans ce PS).
// - `StartOutput(this, bCString const& échantillon, gEOutputType)` : StopOutput, puis
//   `eCResource2::GetSound(échantillon)` et `RequestData` ; range la ressource à `+0x188` et le
//   type à `+0x190`, rend true. Elle ne joue rien elle-même : la lecture vient plus tard du
//   traitement de l'unité. `IsOutputPlaying` = ressource à `+0x188` non nulle.
// - `GetSelfEntity` = eCEntityProxy::GetEntity sur `this + 0x140` (accesseur sans effet de bord).
// - `Entity::StartSay(auditeur&, bCString texte, bool, bool)` : échantillon = GetSampleTitle(texte),
//   puis `StartSayEx(auditeur, échantillon, texte, bool, bool)`. StartSayEx : type 0, 1 si l'auditeur
//   est le joueur, 2 si le locuteur est le joueur, 3 si le 2ᵉ bool est vrai ; StartOutput et
//   StartFacialAni avec `échantillon + ".wav"` ; si le 1ᵉʳ bool est vrai, « nom : texte localisé »
//   (eCLocString(texte).GetString) dans le journal à l'écran (gCHudSystem2::PrintGameLog).
// - `GUI2::ShowSubtitle` (Script.dll) est un simple `ret` : sans effet. Le sous-titre passe par
//   `gCHudSystem2::ShowSubtitle(bCUnicodeString const&)` (Game.dll, instance par
//   gCHudModule2::GetSystem), qui relaie au contrôle à `+0xe0` du système.
// - `GUI2::StartTalk(Entity, bCString texte)` ouvre la page 0xc du HUD avec le nom du locuteur et
//   eCLocString(texte).GetString ; `StopTalk` la ferme.
// - Façade `InfoSystem` (Script.dll, statiques) : IsRunning = gCInfoSystem::IsRunning,
//   GetNPC = GetDialogPartnerNPC (Entity rendue par sret dans RCX), GetCurrentCommandType = type de
//   la commande courante (0x3d sans commande ; 0x1b = gCInfoCommandSay), GetCurrentCommandSay =
//   enveloppe InfComSay de la commande courante, dont GetTextID / GetSpeaker rendent un bCString /
//   une Entity par sret après `this`. À n'appeler que si le type courant est 0x1b.
// - bCString et bCUnicodeString : un seul pointeur (8 octets) ; passés par valeur, ce sont des
//   copies construites par l'appelant et détruites par l'appelé (AD-3).
//
// Toutes les fonctions sont protégées par SEH et rendent false en cas d'échec. Thread principal
// (hook OnProcess, AD-2), sauf `spu_self_entity` et `read_bcstring` : accesseurs sans effet de
// bord appelés depuis le détour de StartOutput (même thread que le script qui parle).
#pragma once

#include "game/entity.h"

#include <cstddef>
#include <string>

namespace mp::game {

constexpr int kInfoCommandSay = 0x1b;   // gCInfoCommandSay::GetCommandType
constexpr int kInfoCommandNone = 0x3d;  // InfoSystem::GetCurrentCommandType sans commande courante

// gCScriptProcessingUnit::StartOutput(bCString const&, gEOutputType) : cible du détour.
using StartOutputFn = bool (*)(void* spu, const void* sample, int type);

// Bloc facultatif en tout ou rien (Game.dll, Script.dll, Engine.dll, SharedBase.dll). False et
// `missing` au premier échec : les expériences J3 restent inactives, J1 et J2 intactes.
bool resolve_dialogue(const char*& missing);
bool dialogue_resolved();
void* start_output_address();  // après resolve_dialogue (sinon nul)

// --- Depuis le détour (accesseurs) ---------------------------------------------------------
bool spu_self_entity(void* spu, void*& out);  // gCScriptProcessingUnit::GetSelfEntity
// bCString::GetText, copié dans `buf` (`cap` octets au plus, sans zéro final). `len` = octets copiés.
bool read_bcstring(const void* bcstring, char* buf, std::size_t cap, std::size_t& len);

// --- Système de dialogue (façade InfoSystem) ----------------------------------------------
bool info_is_running(bool& out);
bool info_dialog_npc(Entity& out);   // nul : aucun partenaire
bool info_command_type(int& out);    // kInfoCommandNone : aucune commande
// Commande courante de type Say (vérifié ici) : identifiant de texte et locuteur.
bool info_say_text_id(std::string& out);
bool info_say_speaker(Entity& out);

// --- Parole --------------------------------------------------------------------------------
// gCScriptProcessingUnit::sIsOutputPlaying(entité) : ressource de sortie en attente ou en cours.
bool output_playing(const Entity& e, bool& out);
// gCScriptProcessingUnit::sStartOutput(entité, échantillon, type) : passe par le détour.
bool start_output(const Entity& e, const char* sample, int type, bool& out);
// Entity::StartFacialAni(bCString) : animation des lèvres (nom = échantillon, comme StartSayEx).
bool start_facial_ani(const Entity& e, const char* sample);
// Entity::StartSay(auditeur, texte, afficher le texte, 2ᵉ bool) : voix, lèvres et texte du jeu.
bool start_say(const Entity& speaker, const Entity& listener, const char* text_id, bool show_text, bool flag);

// --- Sous-titres ---------------------------------------------------------------------------
// eCLocAdmin2::GetInstance().GetString(identifiant) : texte localisé (vide si inconnu).
bool localized_string(const char* text_id, std::wstring& out);
// gCHudModule2::GetSystem puis gCHudSystem2::ShowSubtitle. `hud` = instance (nulle : rien fait).
bool show_subtitle(const wchar_t* text, void*& hud);
// GUI2::StartTalk(Entity, bCString) / StopTalk : page 0xc du HUD.
bool start_talk(const Entity& speaker, const char* text_id, bool& out);
bool stop_talk();

}  // namespace mp::game

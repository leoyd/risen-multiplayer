#!/usr/bin/env bash
# Scénarios de la matrice I/O de J0, exécutés sous Wine avec les vraies DLL du jeu.
# Usage : run_scenarios.sh <dossier du build Windows> <dossier bin de Risen>
set -euo pipefail

win_build=$1
risen_bin=$2
work=$(mktemp -d)
trap 'chmod -R u+w "$work"; rm -rf "$work"' EXIT

export WINEPREFIX=${MP_WINEPREFIX:-$win_build/wineprefix}
export WINEDEBUG=-all
failures=0

# Prépare un dossier « scripts » isolé avec la DLL et, éventuellement, une config.
setup() {
    local dir=$work/$1
    mkdir -p "$dir"
    cp "$win_build/Script_Multiplayer.dll" "$dir/"
    if [[ $# -ge 2 ]]; then printf '%s\n' "$2" > "$dir/multiplayer.ini"; fi
    echo "$dir"
}

# run <nom> <dossier scripts> [dossier bin du jeu] [expect-empty]
run() {
    local name=$1 dir=$2 bin=${3:-$risen_bin} rc=0
    wine "$win_build/mp_harness.exe" "$(winepath -w "$bin")" \
        "$(winepath -w "$dir/Script_Multiplayer.dll")" ${4:+"$4"} >/dev/null || rc=$?
    if [[ $rc -ne 0 ]]; then
        echo "[ÉCHEC] $name : descripteur non conforme (code $rc)"; failures=$((failures + 1))
    fi
}

expect_log() {
    local name=$1 dir=$2 pattern=$3
    if grep -q -- "$pattern" "$dir/Script_Multiplayer.log"; then
        echo "[ OK ] $name"
    else
        echo "[ÉCHEC] $name : « $pattern » absent du log"; failures=$((failures + 1))
    fi
}

expect_no_log() {
    local name=$1 dir=$2 pattern=$3
    if [[ ! -f $dir/Script_Multiplayer.log ]]; then
        echo "[ÉCHEC] $name : pas de log"; failures=$((failures + 1))
    elif grep -q -- "$pattern" "$dir/Script_Multiplayer.log"; then
        echo "[ÉCHEC] $name : « $pattern » présent dans le log"; failures=$((failures + 1))
    else
        echo "[ OK ] $name"
    fi
}

d=$(setup nominal $'role = guest\nhost_ip = 192.168.1.10\ncouleur = bleu')
run nominal "$d"
expect_log "nominal : mod actif" "$d" "mod actif"
expect_log "champ inconnu : avertissement" "$d" "couleur"
expect_log "nominal : log_level=info par défaut" "$d" "config OK (.*log_level=info)"

expect_log "nominal : sonde désactivée par défaut" "$d" "sonde désactivée"
# Story 1.4 : sans sonde, la session pose ses hooks (OnProcess ; PlayMotion et SetRagDollEnabled
# pour les avatars depuis la story 2.2). Le banc n'appelle jamais OnProcess : aucun tick.
expect_log "nominal : hooks de session installés" "$d" "hooks de session installés (OnProcess, PlayMotion, SetRagDollEnabled, locomotion EMotionFX, gCScriptRoutine_PS::OnProcess)"
expect_no_log "nominal : pas de hooks de la sonde" "$d" "hooks installés (OnProcess, PlayMotion"
expect_log "nominal : session prête" "$d" "\[sync\] session prête (rôle invité)"
# Story 3.2 (AD-3) : symboles d'inventaire résolus en tout ou rien à l'installation de la session.
expect_log "nominal : inventaire de session résolu" "$d" "\[hooks\] inventaire de session résolu (arme de l'avatar)"
expect_no_log "nominal : aucun symbole d'inventaire manquant" "$d" "symbole d'inventaire introuvable"
# Story 3.3 (AD-3) : symboles des répliques de PNJ résolus en tout ou rien ; hook de routine posé.
expect_log "nominal : PNJ de session résolus" "$d" "\[hooks\] PNJ de session résolus (répliques)"
expect_no_log "nominal : aucun symbole des PNJ manquant" "$d" "symbole des PNJ introuvable"
expect_no_log "nominal : log_level=info, aucune ligne DEBUG" "$d" "DEBUG"
# Story 3.5 (Q8) : point d'accroche des dégâts posé par défaut (damage_hook = 1), symboles résolus.
expect_log "nominal : point d'accroche des dégâts posé" "$d" "\[hooks\] point d'accroche des dégâts posé (gCDamage_PS::DoDamage)"
expect_no_log "nominal : dégâts actifs" "$d" "dégâts inactifs"
# Story 3.8 : attaques natives des répliques exigeant aussi les détours DownOrDeadBy* ; le banc ne
# charge pas Script_Game.dll : Warn nommé, session installée quand même (symboles vérifiés plus bas).
expect_log "nominal : attaques natives inactives sans DownOrDeadBy*" "$d" "\[hooks\] attaques natives des répliques inactives : détours DownOrDeadBy\* absents"
expect_no_log "nominal : attaques natives non résolues" "$d" "attaques natives des répliques résolues"
# Story 3.7 : détours DownOrDeadBy* de session facultatifs ; le banc ne charge jamais Script_Game.dll :
# Warn nommé, session installée quand même.
expect_log "nominal : DownOrDeadBy* inactifs, cause nommée" "$d" \
    "\[hooks\] DownOrDeadBy\* inactifs : Script_Game.dll introuvable"
# Story 4.1 : dialogues par défaut (dialogues = 1) : bloc game/dialogue résolu sur les vraies DLL,
# détour de StartOutput posé en session.
expect_log "nominal : dialogues, hook StartOutput posé" "$d" \
    "\[hooks\] dialogues : hook StartOutput posé (répliques de l'hôte vues par l'invité)"
expect_no_log "nominal : dialogues actifs" "$d" "dialogues inactifs"

# Spike J1 : sonde explicitement désactivée (hooks de session), puis activée (hooks posés sur les vraies
# DLL du jeu ; le banc n'appelle jamais OnProcess, donc aucun tick).
d=$(setup sonde_off $'role = host\nprobe = 0\nlog_level = DEBUG')
run "sonde off" "$d"
expect_log "log_level = DEBUG lu" "$d" "config OK (.*log_level=debug)"
expect_log "probe = 0 : sonde désactivée" "$d" "sonde désactivée"
expect_log "probe = 0 : hooks de session installés" "$d" "hooks de session installés (OnProcess, PlayMotion, SetRagDollEnabled, locomotion EMotionFX, gCScriptRoutine_PS::OnProcess)"
expect_no_log "probe = 0 : pas de hooks de la sonde" "$d" "hooks installés (OnProcess, PlayMotion"
expect_log "probe = 0 : session prête" "$d" "\[sync\] session prête (rôle hôte)"
expect_no_log "probe = 0 : aucun hook J2" "$d" "\[hooks\] J2"
expect_no_log "probe = 0 : aucun hook J3" "$d" "\[hooks\] J3"
expect_log "log_level = DEBUG : lignes DEBUG écrites" "$d" "DEBUG .*\[sync\] empreinte locale SharedBase.dll"

# Story 3.5 : damage_hook = 0 : pas de détour des dégâts, la session est installée quand même.
d=$(setup sans_degats $'role = host\ndamage_hook = 0')
run "damage_hook = 0" "$d"
expect_log "damage_hook = 0 : détour non posé" "$d" "\[hooks\] point d'accroche des dégâts désactivé (damage_hook = 0)"
expect_no_log "damage_hook = 0 : pas de détour" "$d" "point d'accroche des dégâts posé"
expect_log "damage_hook = 0 : hooks de session installés" "$d" "hooks de session installés (OnProcess, PlayMotion, SetRagDollEnabled, locomotion EMotionFX, gCScriptRoutine_PS::OnProcess)"
# Story 3.8 : sans détour des dégâts, pas d'attaques natives (le coup d'une réplique blesserait l'invité).
expect_log "damage_hook = 0 : attaques natives inactives" "$d" "\[hooks\] attaques natives des répliques inactives : pas de point d'accroche des dégâts"

# Story 3.8 : native_attacks = 0 : bloc non résolu, ligne Info, session installée quand même.
d2=$(setup sans_attaques $'role = guest\nhost_ip = 192.168.1.10\nnative_attacks = 0')
run "native_attacks = 0" "$d2"
expect_log "native_attacks = 0 : désactivées" "$d2" "\[hooks\] attaques natives des répliques désactivées (native_attacks = 0)"
expect_no_log "native_attacks = 0 : non résolues" "$d2" "attaques natives des répliques résolues"
expect_log "native_attacks = 0 : session prête" "$d2" "\[sync\] session prête (rôle invité)"

# Story 4.1 : dialogues = 0 : ni bloc ni détour, ligne Info, session installée quand même.
d3=$(setup sans_dialogues $'role = host\ndialogues = 0')
run "dialogues = 0" "$d3"
expect_log "dialogues = 0 : désactivés" "$d3" "\[hooks\] dialogues désactivés (dialogues = 0)"
expect_no_log "dialogues = 0 : pas de détour" "$d3" "hook StartOutput posé"
expect_log "dialogues = 0 : session prête" "$d3" "\[sync\] session prête (rôle hôte)"

# Overlay à l'écran : posé par défaut (device factice sur le d3d9 du préfixe Wine) ; sans GPU, Warn
# nommé et session intacte. overlay = 0 : aucun détour, ligne Info.
expect_log "overlay par défaut : posé ou inactif nommé" "$d" "\[overlay\] \(détours posés (IDirect3DDevice9::Present, Reset)\|inactif : \)"
expect_no_log "overlay par défaut : d3dx9_39.dll chargée" "$d" "\[overlay\] inactif : d3dx9_39.dll non chargée"
expect_no_log "overlay par défaut : D3DXCreateFontW résolue" "$d" "\[overlay\] inactif : D3DXCreateFontW introuvable"
d=$(setup sans_overlay $'role = host\noverlay = 0')
run "overlay = 0" "$d"
expect_log "overlay = 0 : désactivé" "$d" "\[overlay\] désactivé (overlay = 0)"
expect_no_log "overlay = 0 : pas de détour" "$d" "\[overlay\] détours posés"
expect_log "overlay = 0 : session prête" "$d" "\[sync\] session prête (rôle hôte)"

d=$(setup sonde_on $'role = host\nprobe = 1')
run "sonde on" "$d"
expect_log "probe = 1 : hooks installés" "$d" "hooks installés (OnProcess, PlayMotion, SetRagDollEnabled)"
expect_log "probe = 1 : mod actif" "$d" "mod actif"
expect_no_log "probe = 1 : inventaire de session non résolu" "$d" "inventaire de session résolu"
expect_no_log "probe = 1 : PNJ de session non résolus" "$d" "PNJ de session résolus"
expect_no_log "probe = 1 : pas de hook de session" "$d" "hooks de session installés"
expect_no_log "probe = 1 : aucune session" "$d" "\[sync\]"
expect_no_log "probe = 1 : pas d'overlay" "$d" "\[overlay\]"
# Spike J2 (story 3.0) : hooks facultatifs. Le banc ne charge jamais Script_Game.dll (et le mod
# ne la charge pas lui-même) : hook Routine posé, expérience Q7 nommée inactive, sonde J1 intacte.
expect_log "probe = 1 : J2 : hook Routine posé" "$d" "\[hooks\] J2 : hook Routine posé (Num4)"
expect_no_log "probe = 1 : J2 : ABI du combat résolue" "$d" "expériences Num1 à Num6 inactives"
expect_log "probe = 1 : J2 : Q7 inactive, cause nommée" "$d" \
    "\[hooks\] J2 : expérience Q7 (Num6, mort interceptée) inactive : Script_Game.dll introuvable"
# Spike J3 (story 4.0) : ABI des dialogues résolue en tout ou rien sur les vraies DLL, détour de
# gCScriptProcessingUnit::StartOutput posé ; jamais en session (vérifié par les scénarios probe = 0).
expect_log "probe = 1 : J3 : hook StartOutput posé" "$d" \
    "\[hooks\] J3 : hook StartOutput posé (journal des répliques, Num7 à Num9)"
expect_no_log "probe = 1 : J3 : ABI des dialogues résolue" "$d" "\[hooks\] J3 : journal des répliques et expériences"
# Noms décorés de Kill, Defeat et DownOrDeadBy* (sonde J2, session depuis la story 3.7) présents dans
# la Script_Game.dll du jeu (résolus en jeu seulement).
for sym in '?Kill@@YA_JPEAVgCScriptProcessingUnit@@PEAX1_J@Z' '?Defeat@@YA_JPEAVgCScriptProcessingUnit@@PEAX1_J@Z' \
    '?DownOrDeadByMeleeWeapon@@YA_JPEAVgCScriptProcessingUnit@@PEAX1_J@Z' \
    '?DownOrDeadByProjectile@@YA_JPEAVgCScriptProcessingUnit@@PEAX1_J@Z' \
    '?DownOrDeadByInferno@@YA_JPEAVgCScriptProcessingUnit@@PEAX1_J@Z'; do
    if grep -qaF -- "$sym" "$risen_bin/scripts/Script_Game.dll" 2>/dev/null; then
        echo "[ OK ] Script_Game.dll exporte $sym"
    else
        echo "[ÉCHEC] Script_Game.dll : $sym introuvable"; failures=$((failures + 1))
    fi
done

# Stories 3.2 à 3.2c : symboles d'inventaire de la session présents dans la Script.dll du jeu.
for sym in '?GetRightHoldStack@PSInventory@@QEBAHXZ' '?GetRightHoldItem@PSInventory@@QEBA?AVEntity@@XZ' \
    '?CreateItems@PSInventory@@QEAAHAEBVTemplate@@HW4gEStackType@@@Z' '?HoldRightStack@PSInventory@@QEAA_NH@Z' \
    '?EquipStack@PSInventory@@QEAA_NH@Z' '?GetName@Entity@@QEBA?AVbCString@@XZ' '??0Template@@QEAA@PEBD@Z' \
    '?IsValid@Template@@QEBA_NXZ' '?FindStackIndex@PSInventory@@QEBAHAEBVTemplate@@H_N@Z' \
    '?IsStackEquiped@PSInventory@@QEBA_NH@Z' '?FindStackIndex@PSInventory@@QEBAHW4gEEquipSlot@@@Z' \
    '?GetTemplateItem@PSInventory@@QEBA?AVEntity@@H@Z' '?UnEquipStack@PSInventory@@QEAA_NH@Z'; do
    if grep -qaF -- "$sym" "$risen_bin/Script.dll"; then
        echo "[ OK ] Script.dll exporte $sym"
    else
        echo "[ÉCHEC] Script.dll : $sym introuvable"; failures=$((failures + 1))
    fi
done

# Story 3.3 : symboles de resolve_npc présents dans les DLL du jeu (Script, Engine, Game) ;
# story 3.4b : PSRoutine::PropertyAIMode (lecture et operator=) ; story 3.5 : resolve_damage ;
# story 3.5 (correctif) : PSNpc::Get/SetCurrentTarget (resolve_damage) ; story 3.7 : PSSkills::GetHitPointsMax (resolve_npc) ;
# story 4.0 : resolve_dialogue (sonde J3 ; en session depuis la story 4.1).
for entry in 'Script.dll|?ms_arrNPCs@Entity@@2V?$bTObjArray@VEntity@@@@A' 'Script.dll|?UpdateEntityLists@Entity@@KAXXZ' \
    'Script.dll|?IsPlayer@Entity@@QEBA_NXZ' 'Script.dll|?GetHitPoints@PSSkills@@QEBAHXZ' \
    'Script.dll|?GetHitPointsMax@PSSkills@@QEBAHXZ' \
    'Script.dll|?SetHitPoints@PSSkills@@QEAAXHW4gESkillModifier@@@Z' 'Script.dll|?IsDead@Entity@@QEBA_NXZ' \
    'Script.dll|?GetPose@Entity@@QEBA?AVbCMatrix@@XZ' 'Script.dll|?MoveTo@Entity@@QEAAXAEBVbCMatrix@@@Z' \
    'Engine.dll|?GetID@eCDynamicEntity@@UEBAAEBVbCPropertyID@@XZ' \
    'Game.dll|?GetEntity@gCRuntimeInfo@@QEAAPEAVeCEntity@@AEBVbCPropertyID@@@Z' \
    'Game.dll|?OnProcess@gCScriptRoutine_PS@@UEAAXXZ' \
    'Script.dll|??BPropertyAIMode@PSRoutine@@QEBA?BW4gEAIMode@@XZ' \
    'Script.dll|??4PropertyAIMode@PSRoutine@@QEAAAEAV01@AEBW4gEAIMode@@@Z' \
    'Game.dll|?DoDamage@gCDamage_PS@@QEAA_NPEAVeCEntity@@@Z' \
    'Game.dll|?GetAgressor@gCDamage_PS@@QEBAAEBVeCEntityProxy@@XZ' \
    'Engine.dll|?GetEntity@eCEntityProxy@@QEAAPEAVeCEntity@@XZ' \
    'Script.dll|?DoDamage@PSDamage@@QEAA_NAEAVEntity@@0@Z' \
    'Script.dll|?GetCurrentTarget@PSNpc@@QEBA?AVEntity@@XZ' \
    'Script.dll|?SetCurrentTarget@PSNpc@@QEAAXAEBVEntity@@@Z' \
    'Script.dll|?GetPartyName@PSParty@@QEBA?AVbCString@@XZ' \
    'Script.dll|?StartStumble@PSCombatSystem@@QEBA_NW4gECombatMove@@VEntity@@@Z' 'Script.dll|??0Entity@@QEAA@AEBV0@@Z' \
    'Script.dll|?GetLastAttackMove@PSCombatSystem@@QEAA?AW4gECombatMove@@XZ' \
    'Script.dll|?GetCurrentMove@PSCombatSystem@@QEBA?AW4gECombatMove@@XZ' \
    'Script.dll|?GetCurrentTarget@PSCombatSystem@@QEBA?AVEntity@@XZ' \
    'Script.dll|?SetCurrentTarget@PSCombatSystem@@QEAA_NAEBVEntity@@@Z' \
    'Script.dll|?StartMove@PSCombatSystem@@QEBA_NW4gECombatMove@@@Z' \
    'Script.dll|?StopCurrentMove@PSCombatSystem@@QEAA_NXZ' \
    'Script.dll|?GetRightFistItem@PSCombatSystem@@QEBA?AVTemplate@@XZ' \
    'Game.dll|?StartOutput@gCScriptProcessingUnit@@QEAA_NAEBVbCString@@W4gEOutputType@@@Z' \
    'Game.dll|?GetSelfEntity@gCScriptProcessingUnit@@QEAAPEAVeCGeometryEntity@@XZ' \
    'Game.dll|?sStartOutput@gCScriptProcessingUnit@@KA_NPEAVeCGeometryEntity@@AEBVbCString@@W4gEOutputType@@@Z' \
    'Game.dll|?sIsOutputPlaying@gCScriptProcessingUnit@@KA_NPEAVeCGeometryEntity@@@Z' \
    'Game.dll|?IsOutputPlaying@gCScriptProcessingUnit@@QEBA_NXZ' 'Game.dll|?UpdateChannel@gCScriptProcessingUnit@@IEAAXXZ' \
    'Game.dll|?GetSystem@gCHudModule2@@SAPEAVgCHudSystem2@@XZ' \
    'Game.dll|?ShowSubtitle@gCHudSystem2@@QEAAXAEBVbCUnicodeString@@@Z' \
    'Script.dll|?IsRunning@InfoSystem@@SA_NXZ' 'Script.dll|?GetNPC@InfoSystem@@SA?AVEntity@@XZ' \
    'Script.dll|?GetCurrentCommandType@InfoSystem@@SA?AW4gEInfoCommand@@XZ' \
    'Script.dll|?GetCurrentCommandSay@InfoSystem@@SAAEAVInfComSay@@XZ' \
    'Script.dll|?GetTextID@InfComSay@@QEAA?AVbCString@@XZ' 'Script.dll|?GetSpeaker@InfComSay@@QEAA?AVEntity@@XZ' \
    'Script.dll|?StartFacialAni@Entity@@QEAAXVbCString@@@Z' 'Script.dll|?StartSay@Entity@@QEAAXAEAV1@VbCString@@_N2@Z' \
    'Script.dll|?StartTalk@GUI2@@QEAA_NVEntity@@VbCString@@@Z' 'Script.dll|?StopTalk@GUI2@@QEAAXXZ' \
    'Engine.dll|?GetInstance@eCLocAdmin2@@SAAEAV1@XZ' \
    'Engine.dll|?GetString@eCLocAdmin2@@QEBA?AVbCUnicodeString@@AEBVbCString@@@Z' \
    'SharedBase.dll|?GetText@bCString@@QEBAPEBDXZ' 'SharedBase.dll|??0bCUnicodeString@@QEAA@PEB_W@Z' \
    'SharedBase.dll|??1bCUnicodeString@@QEAA@XZ' 'SharedBase.dll|?GetText@bCUnicodeString@@QEBAPEB_WXZ'; do
    dll=${entry%%|*}
    sym=${entry#*|}
    if grep -qaF -- "$sym" "$risen_bin/$dll"; then
        echo "[ OK ] $dll exporte $sym"
    else
        echo "[ÉCHEC] $dll : $sym introuvable"; failures=$((failures + 1))
    fi
done

# Hook impossible : jeu sans Script.dll (Game.dll n'en dépend pas). Attendu : erreur
# explicite, sonde inactive, mod toujours actif et descripteur intact.
no_script_bin=$work/no_script_bin
mkdir -p "$no_script_bin"
for f in "$risen_bin"/*; do
    [[ $(basename "$f") == Script.dll ]] || ln -s "$f" "$no_script_bin/"
done
d=$(setup sonde_sans_script $'role = host\nprobe = 1')
run "sonde sans Script.dll" "$d" "$no_script_bin"
expect_log "hook impossible : erreur explicite" "$d" "sonde inactive : symbole introuvable : Script.dll"
expect_no_log "hook impossible : aucun hook" "$d" "hooks installés"
expect_no_log "hook impossible : aucun hook J2" "$d" "\[hooks\] J2"
expect_no_log "hook impossible : aucun hook J3" "$d" "\[hooks\] J3"
expect_log "hook impossible : mod actif" "$d" "mod actif"

# Même jeu sans Script.dll, sans sonde : la session ne peut pas poser son hook. Attendu :
# erreur explicite, jeu en solo, mod toujours actif.
d=$(setup session_sans_script $'role = host\nprobe = 0')
run "session sans Script.dll" "$d" "$no_script_bin"
expect_log "session impossible : symbole nommé" "$d" "session inactive : symbole introuvable : Script.dll"
expect_log "session impossible : retour au solo" "$d" "session impossible"
expect_no_log "session impossible : aucun hook" "$d" "hooks installés"
expect_no_log "session impossible : pas de session prête" "$d" "session prête"
expect_no_log "session impossible : inventaire non résolu" "$d" "inventaire de session résolu"
expect_no_log "session impossible : PNJ non résolus" "$d" "PNJ de session résolus"
expect_log "session impossible : mod actif" "$d" "mod actif"

d=$(setup absente)
run absente "$d"
expect_log "config absente" "$d" "config absente"
expect_log "config absente : mod inactif" "$d" "mod inactif"

d=$(setup invalide 'role = chef')
run invalide "$d"
expect_log "champ invalide nommé" "$d" "champ « role »"
expect_log "champ invalide : mod inactif" "$d" "mod inactif"

# Version inconnue : copie de Game.dll avec un TimeDateStamp modifié, autres fichiers
# du jeu liés symboliquement. Attendu : tables vides, « version non supportée ».
fake_bin=$work/fake_bin
mkdir -p "$fake_bin"
for f in "$risen_bin"/*; do
    [[ $(basename "$f") == Game.dll ]] || ln -s "$f" "$fake_bin/"
done
cp "$risen_bin/Game.dll" "$fake_bin/Game.dll"
python3 -I - "$fake_bin/Game.dll" <<'PY'
import struct, sys
with open(sys.argv[1], "r+b") as f:
    f.seek(0x3c)
    pe = struct.unpack("<I", f.read(4))[0]
    f.seek(pe + 8)  # signature PE (4) + Machine (2) + NumberOfSections (2)
    f.write(struct.pack("<I", 0x12345678))
PY
d=$(setup version_inconnue 'role = host')
run "version inconnue" "$d" "$fake_bin" expect-empty
expect_log "version inconnue : version non supportée" "$d" "version non supportée"
expect_log "version inconnue : mod inactif" "$d" "mod inactif"

d=$(setup lecture_seule 'role = host')
chmod a-w "$d"
run "log impossible" "$d"
if [[ ! -e $d/Script_Multiplayer.log ]]; then
    echo "[ OK ] log impossible : descripteur rendu, pas de log"
else
    echo "[ÉCHEC] log impossible : un log a été écrit"; failures=$((failures + 1))
fi

echo "$failures scénario(s) en échec"
exit $(( failures > 0 ))

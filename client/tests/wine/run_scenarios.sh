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
expect_log "log_level = DEBUG : lignes DEBUG écrites" "$d" "DEBUG .*\[sync\] empreinte locale SharedBase.dll"

d=$(setup sonde_on $'role = host\nprobe = 1')
run "sonde on" "$d"
expect_log "probe = 1 : hooks installés" "$d" "hooks installés (OnProcess, PlayMotion, SetRagDollEnabled)"
expect_log "probe = 1 : mod actif" "$d" "mod actif"
expect_no_log "probe = 1 : inventaire de session non résolu" "$d" "inventaire de session résolu"
expect_no_log "probe = 1 : PNJ de session non résolus" "$d" "PNJ de session résolus"
expect_no_log "probe = 1 : pas de hook de session" "$d" "hooks de session installés"
expect_no_log "probe = 1 : aucune session" "$d" "\[sync\]"
# Spike J2 (story 3.0) : hooks facultatifs. Le banc ne charge jamais Script_Game.dll (et le mod
# ne la charge pas lui-même) : hook Routine posé, expérience Q7 nommée inactive, sonde J1 intacte.
expect_log "probe = 1 : J2 : hook Routine posé" "$d" "\[hooks\] J2 : hook Routine posé (Num4)"
expect_no_log "probe = 1 : J2 : ABI du combat résolue" "$d" "expériences Num1 à Num6 inactives"
expect_log "probe = 1 : J2 : Q7 inactive, cause nommée" "$d" \
    "\[hooks\] J2 : expérience Q7 (Num6, mort interceptée) inactive : Script_Game.dll introuvable"
# Noms décorés de Kill et Defeat présents dans la Script_Game.dll du jeu (résolus en jeu seulement).
for sym in '?Kill@@YA_JPEAVgCScriptProcessingUnit@@PEAX1_J@Z' '?Defeat@@YA_JPEAVgCScriptProcessingUnit@@PEAX1_J@Z'; do
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

# Story 3.3 : symboles de resolve_npc présents dans les DLL du jeu (Script, Engine, Game).
for entry in 'Script.dll|?ms_arrNPCs@Entity@@2V?$bTObjArray@VEntity@@@@A' 'Script.dll|?UpdateEntityLists@Entity@@KAXXZ' \
    'Script.dll|?IsPlayer@Entity@@QEBA_NXZ' 'Script.dll|?GetHitPoints@PSSkills@@QEBAHXZ' \
    'Script.dll|?SetHitPoints@PSSkills@@QEAAXHW4gESkillModifier@@@Z' 'Script.dll|?IsDead@Entity@@QEBA_NXZ' \
    'Script.dll|?GetPose@Entity@@QEBA?AVbCMatrix@@XZ' 'Script.dll|?MoveTo@Entity@@QEAAXAEBVbCMatrix@@@Z' \
    'Engine.dll|?GetID@eCDynamicEntity@@UEBAAEBVbCPropertyID@@XZ' \
    'Game.dll|?GetEntity@gCRuntimeInfo@@QEAAPEAVeCEntity@@AEBVbCPropertyID@@@Z' \
    'Game.dll|?OnProcess@gCScriptRoutine_PS@@UEAAXXZ'; do
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

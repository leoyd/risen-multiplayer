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
# Story 1.4 : sans sonde, la session pose le seul hook OnProcess. Le banc n'appelle jamais
# OnProcess : aucun tick, aucune transition.
expect_log "nominal : hook de session installé" "$d" "hooks installés (OnProcess)"
expect_no_log "nominal : pas de PlayMotion" "$d" "PlayMotion"
expect_log "nominal : session prête" "$d" "\[sync\] session prête (rôle invité)"
expect_no_log "nominal : log_level=info, aucune ligne DEBUG" "$d" "DEBUG"

# Spike J1 : sonde explicitement désactivée (hook de session seul), puis activée (hooks posés sur les vraies
# DLL du jeu ; le banc n'appelle jamais OnProcess, donc aucun tick).
d=$(setup sonde_off $'role = host\nprobe = 0\nlog_level = DEBUG')
run "sonde off" "$d"
expect_log "log_level = DEBUG lu" "$d" "config OK (.*log_level=debug)"
expect_log "probe = 0 : sonde désactivée" "$d" "sonde désactivée"
expect_log "probe = 0 : hooks installés (OnProcess)" "$d" "hooks installés (OnProcess)"
expect_no_log "probe = 0 : pas de hook PlayMotion" "$d" "PlayMotion"
expect_log "probe = 0 : session prête" "$d" "\[sync\] session prête (rôle hôte)"
expect_log "log_level = DEBUG : lignes DEBUG écrites" "$d" "DEBUG .*\[sync\] empreinte locale SharedBase.dll"

d=$(setup sonde_on $'role = host\nprobe = 1')
run "sonde on" "$d"
expect_log "probe = 1 : hooks installés" "$d" "hooks installés (OnProcess, PlayMotion, SetRagDollEnabled)"
expect_log "probe = 1 : mod actif" "$d" "mod actif"
expect_no_log "probe = 1 : pas de hook de session" "$d" "hooks installés (OnProcess)"
expect_no_log "probe = 1 : aucune session" "$d" "\[sync\]"

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
expect_log "hook impossible : mod actif" "$d" "mod actif"

# Même jeu sans Script.dll, sans sonde : la session ne peut pas poser son hook. Attendu :
# erreur explicite, jeu en solo, mod toujours actif.
d=$(setup session_sans_script $'role = host\nprobe = 0')
run "session sans Script.dll" "$d" "$no_script_bin"
expect_log "session impossible : symbole nommé" "$d" "session inactive : symbole introuvable : Script.dll"
expect_log "session impossible : retour au solo" "$d" "session impossible"
expect_no_log "session impossible : aucun hook" "$d" "hooks installés"
expect_no_log "session impossible : pas de session prête" "$d" "session prête"
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

#!/usr/bin/env bash
# Hôte de session Linux (session_host : core/session + net::Transport) face au faux invité Go
# (tools/fake-guest), stories 1.4, 1.5, 2.2, 2.3, 2.4, 3.2, 3.2b, 3.2c, 3.3, 3.4, 3.4a, 3.5, 3.6, 3.7, 3.8 et 3.8b : preuve du comportement de l'hôte sans le jeu.
# Usage : session_fake_guest.sh <session_host> <dossier tools/fake-guest>
set -uo pipefail

host_bin=$1
guest_src=$2
work=$(mktemp -d)
trap 'kill $(jobs -p) 2>/dev/null; rm -rf "$work"' EXIT

(cd "$guest_src" && go build -o "$work/fake-guest" .) || { echo "[ÉCHEC] build du faux invité"; exit 1; }
guest=$work/fake-guest
failures=0
base_port=$((38000 + RANDOM % 2000))

ok() { echo "[ OK ] $1"; }
ko() { echo "[ÉCHEC] $1"; failures=$((failures + 1)); }

expect_code() {  # <nom> <attendu> <obtenu> <fichier log>
    if [[ $2 -eq $3 ]]; then ok "$1 : code $3"; else ko "$1 : code $3, attendu $2"; sed 's/^/    /' "$4"; fi
}
expect_log() {  # <nom> <fichier> <motif>
    if grep -q -- "$3" "$2"; then ok "$1"; else ko "$1 : « $3 » absent"; sed 's/^/    /' "$2"; fi
}
expect_count() {  # <nom> <fichier> <motif> <nombre>
    local n
    n=$(grep -c -- "$3" "$2")
    if [[ $n -eq $4 ]]; then ok "$1"; else ko "$1 : « $3 » $n fois, attendu $4"; sed 's/^/    /' "$2"; fi
}

# start_host <nom> <port> <max-ms> [fins] [options de session_host…] : lance l'hôte en
# arrière-plan, attend que le transport écoute. Échec : log de l'hôte affiché, hôte arrêté, code
# non nul (scénario sauté).
start_host() {
    "$host_bin" --port "$2" --max-ms "$3" --ends "${4:-1}" "${@:5}" > "$work/$1.host.log" 2>&1 &
    host_pid=$!
    for _ in $(seq 100); do
        grep -q "transport démarré" "$work/$1.host.log" && return 0
        sleep 0.05
    done
    ko "$1 : l'hôte n'a pas démarré (scénario sauté)"
    sed 's/^/    /' "$work/$1.host.log"
    kill "$host_pid" 2>/dev/null; wait "$host_pid" 2>/dev/null
    return 1
}

# --- Nominal : session de 3 s puis SessionEnd{1} -------------------------------------
p=$((base_port + 1))
if start_host nominal $p 20000; then
    "$guest" --host 127.0.0.1 --port $p --duration 3s > "$work/nominal.guest.log" 2>&1; rc=$?
    wait $host_pid
    expect_code "nominal : invité" 0 $rc "$work/nominal.guest.log"
    h=$work/nominal.host.log
    expect_log "nominal : Hosting" "$h" "Inactive → Hosting"
    expect_log "nominal : Hello journalisé" "$h" "Hello reçu de 127.0.0.1"
    expect_log "nominal : InSession" "$h" "Hosting → InSession"
    expect_log "nominal : fin par l'invité" "$h" "InSession → Ended : fin de session par le pair : départ du joueur (1)"
    expect_log "nominal : maintien puis Inactive" "$h" "Ended → Inactive : transport arrêté ; réarmement immédiat"
    expect_log "nominal : réarmé (même monde)" "$h" "état final : Hosting"
    expect_log "nominal : heartbeats reçus par l'invité" "$work/nominal.guest.log" "Heartbeat envoyés=1 reçus=[1-9]"
    # Story 3.2b : sans --equipment, pas d'EquipAvatar.
    expect_count "nominal : pas d'EquipAvatar sans --equipment" "$h" "EquipAvatar" 0
fi

# --- --dup-hello : même Welcome, Hello en session ignoré ------------------------------
p=$((base_port + 2))
if start_host dup $p 20000; then
    "$guest" --host 127.0.0.1 --port $p --duration 3s --dup-hello > "$work/dup.guest.log" 2>&1; rc=$?
    wait $host_pid
    expect_code "dup-hello : invité" 0 $rc "$work/dup.guest.log"
    h=$work/dup.host.log
    expect_count "dup-hello : une seule entrée en session" "$h" "→ InSession" 1
    expect_log "dup-hello : même Welcome renvoyé" "$h" "même Welcome renvoyé"
    expect_log "dup-hello : Hello après KCP ignoré" "$h" "ignoré (KCP déjà établi)"
    grep -o "Welcome conv=0x[0-9a-f]*" "$work/dup.guest.log" | sort -u > "$work/dup.convs"
    expect_count "dup-hello : un seul conv côté invité" "$work/dup.convs" "Welcome" 1
    expect_log "dup-hello : fin par l'invité" "$h" "départ du joueur (1)"
fi

# --- --bad-version : Reject{1}, l'hôte reste en attente -------------------------------
p=$((base_port + 3))
if start_host bad $p 3000; then
    "$guest" --host 127.0.0.1 --port $p --bad-version > "$work/bad.guest.log" 2>&1; rc=$?
    wait $host_pid
    expect_code "bad-version : invité rejeté" 2 $rc "$work/bad.guest.log"
    h=$work/bad.host.log
    expect_log "bad-version : Reject{1} journalisé" "$h" "Reject envoyé à 127.0.0.1:[0-9]* : version du protocole différente (1)"
    expect_log "bad-version : l'hôte reste Hosting" "$h" "état final : Hosting"
    expect_log "bad-version : raison côté invité" "$work/bad.guest.log" "version du protocole différente"
fi

# --- Deux invités : le second reçoit Reject{4}, la session du premier continue --------
p=$((base_port + 4))
if start_host two $p 20000; then
    "$guest" --host 127.0.0.1 --port $p --duration 4s > "$work/two.a.log" 2>&1 &
    a_pid=$!
    sleep 1
    "$guest" --host 127.0.0.1 --port $p --nickname Second > "$work/two.b.log" 2>&1; rc_b=$?
    wait $a_pid; rc_a=$?
    wait $host_pid
    expect_code "deux invités : second rejeté" 2 $rc_b "$work/two.b.log"
    expect_code "deux invités : premier intact" 0 $rc_a "$work/two.a.log"
    h=$work/two.host.log
    expect_log "deux invités : Reject{4} journalisé" "$h" "Reject envoyé à .* : session pleine (4)"
    expect_log "deux invités : raison côté invité" "$work/two.b.log" "session pleine"
    expect_count "deux invités : une seule session" "$h" "→ InSession" 1
    expect_log "deux invités : fin par le premier" "$h" "départ du joueur (1)"
fi

# --- --drop 20 : pertes sortantes de l'invité, la session tient ------------------------
p=$((base_port + 5))
if start_host drop $p 30000; then
    "$guest" --host 127.0.0.1 --port $p --duration 8s --drop 20 --seed 3 > "$work/drop.guest.log" 2>&1; rc=$?
    wait $host_pid
    expect_code "drop 20 : invité" 0 $rc "$work/drop.guest.log"
    h=$work/drop.host.log
    expect_log "drop 20 : InSession" "$h" "Hosting → InSession"
    expect_log "drop 20 : fin par l'invité, pas de timeout" "$h" "départ du joueur (1)"
fi

# --- --kill : l'invité disparaît, l'hôte constate le timeout ---------------------------
p=$((base_port + 6))
if start_host kill $p 20000; then
    "$guest" --host 127.0.0.1 --port $p --duration 1s --kill > "$work/kill.guest.log" 2>&1; rc=$?
    wait $host_pid
    expect_code "kill : invité" 0 $rc "$work/kill.guest.log"
    expect_log "kill : timeout côté hôte" "$work/kill.host.log" "InSession → Ended : timeout"
fi

# wait_count <fichier> <motif> <nombre> : attend (≤ 15 s) que le motif apparaisse N fois.
wait_count() {
    for _ in $(seq 300); do
        [[ $(grep -c -- "$2" "$1") -ge $3 ]] && return 0
        sleep 0.05
    done
    return 1
}

# abort_scenario <nom> <pid invité> : échec d'une attente ; l'invité (sans --duration) et
# l'hôte sont tués pour ne pas attendre --max-ms.
abort_scenario() {
    ko "$1"
    kill "$2" "$host_pid" 2>/dev/null
    wait "$2" 2>/dev/null
    wait "$host_pid" 2>/dev/null
}

# --- Story 1.5 : trois cycles « rejoindre, fin de monde, réarmer » ----------------------
p=$((base_port + 7))
if start_host reload $p 60000 3; then
    h=$work/reload.host.log
    reload_ok=1
    for i in 1 2 3; do
        "$guest" --host 127.0.0.1 --port $p > "$work/reload.guest$i.log" 2>&1 &
        g_pid=$!
        if ! wait_count "$h" "→ InSession" $i; then
            abort_scenario "rechargement $i : pas de session" $g_pid; reload_ok=0; break
        fi
        kill -USR1 $host_pid  # rechargement simulé
        wait $g_pid; rc=$?
        expect_code "rechargement $i : invité sorti sur SessionEnd" 0 $rc "$work/reload.guest$i.log"
        expect_log "rechargement $i : SessionEnd{2} reçu par l'invité" "$work/reload.guest$i.log" "<- SessionEnd raison=2"
        expect_log "rechargement $i : Despawn de l'hôte reçu" "$work/reload.guest$i.log" "<- Despawn netid=1"
        if ! wait_count "$h" "Ended → Inactive" $i; then
            ko "rechargement $i : pas de retour à Inactive"
            kill "$host_pid" 2>/dev/null; wait "$host_pid" 2>/dev/null; reload_ok=0; break
        fi
    done
    [[ $reload_ok -eq 1 ]] && wait $host_pid
    expect_count "rechargements : 3 sessions" "$h" "→ InSession" 3
    expect_count "rechargements : 3 fins de monde" "$h" "InSession → Ended : fin de monde" 3
    expect_count "rechargements : 3 SessionEnd{2} envoyés" "$h" "SessionEnd envoyé : fin de monde (2)" 3
    expect_count "rechargements : 3 retours à Inactive" "$h" "Ended → Inactive" 3
    grep -o "conv=0x[0-9a-f]*" "$h" | sort -u > "$work/reload.convs"
    expect_count "rechargements : 3 conv distincts" "$work/reload.convs" "conv=" 3
fi

# --- Story 1.5 : réarmement après --quit, un second invité rejoint le même monde --------
p=$((base_port + 8))
if start_host rearm $p 30000 2; then
    h=$work/rearm.host.log
    "$guest" --host 127.0.0.1 --port $p --duration 1s --quit > "$work/rearm.a.log" 2>&1; rc_a=$?
    "$guest" --host 127.0.0.1 --port $p --duration 1s --quit > "$work/rearm.b.log" 2>&1; rc_b=$?
    wait $host_pid
    expect_code "réarmement : premier invité" 0 $rc_a "$work/rearm.a.log"
    expect_code "réarmement : second invité" 0 $rc_b "$work/rearm.b.log"
    expect_count "réarmement : 2 sessions" "$h" "→ InSession" 2
    expect_count "réarmement : réarmement immédiat" "$h" "réarmement immédiat" 2
fi

# --- Story 2.2 : « rejoindre, --kill, rejoindre, quitter » : jamais deux avatars ---------
p=$((base_port + 10))
if start_host avatars $p 30000 2; then
    h=$work/avatars.host.log
    "$guest" --host 127.0.0.1 --port $p --duration 1s --kill > "$work/avatars.a.log" 2>&1; rc_a=$?
    # L'hôte constate le timeout (5 s) et se réarme avant le second invité.
    if wait_count "$h" "Ended → Inactive" 1; then
        "$guest" --host 127.0.0.1 --port $p --duration 1s --quit > "$work/avatars.b.log" 2>&1; rc_b=$?
        wait $host_pid
    else
        ko "avatars : pas de réarmement après --kill"; kill "$host_pid" 2>/dev/null; wait "$host_pid" 2>/dev/null; rc_b=-1
    fi
    expect_code "avatars : premier invité (--kill)" 0 $rc_a "$work/avatars.a.log"
    expect_code "avatars : second invité (--quit)" 0 $rc_b "$work/avatars.b.log"
    expect_count "avatars : 2 sessions" "$h" "→ InSession" 2
    expect_count "avatars : 2 Spawn de l'invité reçus" "$h" "Spawn reçu (NetId 2 (invité), type 0" 2
    expect_count "avatars : 2 créations" "$h" "SpawnAvatar NetId 2 : avatars présents=1" 2
    expect_count "avatars : 2 retraits" "$h" "DespawnAvatar NetId 2 : avatars présents=0" 2
    expect_count "avatars : jamais deux avatars" "$h" "avatars présents=2" 0
    expect_log "avatars : retrait au timeout" "$h" "avatar 2 (invité) retiré : fin de session"
    expect_log "avatars : retrait sur Despawn" "$h" "avatar 2 (invité) retiré : Despawn reçu"
    expect_count "avatars : Spawn de l'hôte reçu à chaque session" "$work/avatars.a.log" "<- Spawn netid=1 type=0" 1
    expect_count "avatars : Spawn de l'hôte reçu (second)" "$work/avatars.b.log" "<- Spawn netid=1 type=0" 1
    l_despawn=$(grep -n -- "-> Despawn netid=2" "$work/avatars.b.log" | head -1 | cut -d: -f1)
    l_end=$(grep -n -- "-> SessionEnd raison=1" "$work/avatars.b.log" | head -1 | cut -d: -f1)
    if [[ -n $l_despawn && -n $l_end && $l_despawn -lt $l_end ]]; then
        ok "avatars : Despawn avant SessionEnd"
    else
        ko "avatars : Despawn avant SessionEnd (lignes ${l_despawn:-?} et ${l_end:-?})"; sed 's/^/    /' "$work/avatars.b.log"
    fi
fi

# --- Story 2.3 : cercle avec 100 ms ± 20 ms de latence et 10 % de pertes : pas de saut ----
p=$((base_port + 11))
if start_host fluid $p 30000; then
    h=$work/fluid.host.log
    "$guest" --host 127.0.0.1 --port $p --duration 6s --quit --scenario "$guest_src/scenarios/cercle.txt" \
        --delay 100ms --jitter 20ms --drop 10 > "$work/fluid.guest.log" 2>&1; rc=$?
    wait $host_pid
    expect_code "fluidité : invité" 0 $rc "$work/fluid.guest.log"
    expect_log "fluidité : snapshots de l'hôte reçus par l'invité" "$work/fluid.guest.log" "snapshots envoyés=[0-9]* reçus=[1-9]"
    line=$(grep -- "\[test\] avatar (retrait)" "$h" | tail -1)
    jump=$(sed -E 's/.*saut max ([0-9.]+) cm.*/\1/' <<< "$line")
    travelled=$(sed -E 's/.*parcouru ([0-9.]+) cm.*/\1/' <<< "$line")
    if [[ -n $line ]] && awk -v j="$jump" -v t="$travelled" 'BEGIN { exit !(j < 10 && t > 800) }'; then
        ok "fluidité : avatar déplacé sans saut ($line)"
    else
        ko "fluidité : saut max ${jump:-?} cm (< 10 attendu), parcouru ${travelled:-?} cm (> 800 attendu)"
        sed 's/^/    /' "$h" | grep -E "avatar|snapshots|Spawn" | head -20
    fi
fi

# --- Story 2.4 : gestes horodatés et mort du scénario, rejoués par l'hôte dans l'ordre -----
p=$((base_port + 12))
cat > "$work/gestes.txt" <<'EOS'
pause 3
anim 0.2 0 Hero_Run
anim 0.6 0 Hero_Jump
param 0.7 0 poids 1 0.25
anim 1.0 2 Hero_Attack 0.1 0 0 1 -1 1 1
mort 2.0
EOS
if start_host gestes $p 30000; then
    h=$work/gestes.host.log
    "$guest" --host 127.0.0.1 --port $p --duration 3s --quit --scenario "$work/gestes.txt" --delay 50ms \
        > "$work/gestes.guest.log" 2>&1; rc=$?
    wait $host_pid
    expect_code "gestes : invité" 0 $rc "$work/gestes.guest.log"
    expect_count "gestes : course rejouée" "$h" "PlayAvatarMotion NetId 2 « Hero_Run » type 0" 1
    expect_count "gestes : saut rejoué" "$h" "PlayAvatarMotion NetId 2 « Hero_Jump » type 0" 1
    expect_count "gestes : attaque rejouée" "$h" "PlayAvatarMotion NetId 2 « Hero_Attack » type 2" 1
    order=$(grep -E "PlayAvatar(Motion|Param)" "$h" | sed -E 's/.*« (Hero_[A-Za-z]+) ».*/\1/; s/.*PlayAvatarParam.*/Param/' | tr '\n' ,)
    if [[ $order == "Hero_Run,Hero_Jump,Param,Hero_Attack," ]]; then ok "gestes : dans l'ordre (réglage compris)"; else ko "gestes : ordre $order"; fi
    expect_log "gestes : réglage de locomotion rejoué" "$h" "PlayAvatarParam NetId 2 type 0 nature 0 valeur 1.000000"
    # Story 3.7 (v14) : Death{2} = invité à terre ; plus de ragdoll, l'hôte répond Respawn.
    expect_count "gestes : pas de ragdoll de l'avatar" "$h" "AvatarDeath NetId 2" 0
    expect_log "gestes : Death reçu (à terre)" "$h" "Death reçu : invité à terre, Respawn à envoyer"
    expect_log "gestes : Respawn envoyé" "$h" "Respawn envoyé : invité replacé"
    expect_count "gestes : rien d'ignoré" "$h" "Motion d'AniId" 0
fi

# --- Story 2.4 : --echo, l'invité renvoie les snapshots de l'hôte comme les siens -----------
p=$((base_port + 13))
if start_host echo $p 30000; then
    h=$work/echo.host.log
    "$guest" --host 127.0.0.1 --port $p --duration 3s --quit --echo > "$work/echo.guest.log" 2>&1; rc=$?
    wait $host_pid
    expect_code "écho : invité" 0 $rc "$work/echo.guest.log"
    expect_log "écho : snapshots renvoyés" "$work/echo.guest.log" "snapshots envoyés=[1-9][0-9]* reçus=[1-9]"
    expect_log "écho : avatar déplacé" "$h" "\[test\] avatar (retrait) : [1-9][0-9]* déplacements"
fi

# --- Story 3.2 : arme — Weapon dans les deux sens, après le Spawn, puis rangée ------------
p=$((base_port + 14))
cat > "$work/arme.txt" <<'EOS'
pause 3
weapon 1.0 -
weapon 1.5 It_2H_Axe
EOS
start_host_weapon() {  # comme start_host, avec --weapon
    "$host_bin" --port "$2" --max-ms "$3" --weapon "$4" > "$work/$1.host.log" 2>&1 &
    host_pid=$!
    for _ in $(seq 100); do
        grep -q "transport démarré" "$work/$1.host.log" && return 0
        sleep 0.05
    done
    ko "$1 : l'hôte n'a pas démarré (scénario sauté)"; sed 's/^/    /' "$work/$1.host.log"
    kill "$host_pid" 2>/dev/null; wait "$host_pid" 2>/dev/null
    return 1
}
if start_host_weapon arme $p 30000 Host_Sword; then
    h=$work/arme.host.log
    "$guest" --host 127.0.0.1 --port $p --duration 2500ms --quit --weapon It_1H_Sword --scenario "$work/arme.txt" \
        > "$work/arme.guest.log" 2>&1 &
    g_pid=$!
    if wait_count "$work/arme.guest.log" '<- Weapon netid=1 nom="Host_Sword"' 1; then
        kill -HUP $host_pid  # l'hôte range son arme
    else
        ko "arme : Weapon de l'hôte non reçu"
    fi
    wait $g_pid; rc=$?
    wait $host_pid
    g=$work/arme.guest.log
    expect_code "arme : invité" 0 $rc "$g"
    expect_log "arme : Weapon de l'hôte envoyé" "$h" "Weapon envoyé : NetId 1 (hôte), « Host_Sword »"
    expect_log "arme : Weapon rangé de l'hôte reçu" "$g" '<- Weapon netid=1 nom=""'
    l_spawn=$(grep -n -- "<- Spawn netid=1" "$g" | head -1 | cut -d: -f1)
    l_weapon=$(grep -n -- "<- Weapon netid=1" "$g" | head -1 | cut -d: -f1)
    if [[ -n $l_spawn && -n $l_weapon && $l_spawn -lt $l_weapon ]]; then
        ok "arme : Weapon de l'hôte après son Spawn"
    else
        ko "arme : Weapon de l'hôte après son Spawn (lignes ${l_spawn:-?} et ${l_weapon:-?})"; sed 's/^/    /' "$g"
    fi
    expect_count "arme : épée de l'invité appliquée" "$h" "AvatarWeapon NetId 2 « It_1H_Sword »" 1
    expect_count "arme : arme de l'invité rangée" "$h" "AvatarWeapon NetId 2 «  »" 1
    expect_count "arme : hache de l'invité" "$h" "AvatarWeapon NetId 2 « It_2H_Axe »" 1
    order=$(grep -o "AvatarWeapon NetId 2 « [^»]* »" "$h" | sed -E 's/.*« (.*) »/\1/' | tr '\n' ,)
    if [[ $order == "It_1H_Sword,,It_2H_Axe," ]]; then ok "arme : dans l'ordre"; else ko "arme : ordre $order"; fi
    l_sa=$(grep -n -- "SpawnAvatar NetId 2" "$h" | head -1 | cut -d: -f1)
    l_aw=$(grep -n -- "AvatarWeapon NetId 2" "$h" | head -1 | cut -d: -f1)
    if [[ -n $l_sa && -n $l_aw && $l_sa -lt $l_aw ]]; then ok "arme : avatar armé après son apparition"; else ko "arme : SpawnAvatar puis AvatarWeapon (lignes ${l_sa:-?} et ${l_aw:-?})"; fi
    # Story 3.2c : sans --armor, jamais d'AvatarArmor.
    expect_count "armure : pas d'AvatarArmor sans --armor (arme)" "$h" "AvatarArmor" 0
fi

# --- Story 3.2b : équipement de l'invité — Hello.equipment, EquipAvatar après SpawnAvatar ----
p=$((base_port + 15))
if start_host equipement $p 20000; then
    "$guest" --host 127.0.0.1 --port $p --duration 2s --equipment " It_Armor_Don_Fighter , It_1H_Sword " \
        > "$work/equipement.guest.log" 2>&1; rc=$?
    wait $host_pid
    h=$work/equipement.host.log
    expect_code "équipement : invité" 0 $rc "$work/equipement.guest.log"
    expect_log "équipement : reçu dans Hello" "$h" "équipement de l'invité : It_Armor_Don_Fighter,It_1H_Sword"
    expect_count "équipement : EquipAvatar" "$h" "\[test\] EquipAvatar NetId 2 « It_Armor_Don_Fighter,It_1H_Sword »" 1
    l_sa=$(grep -n -- "SpawnAvatar NetId 2" "$h" | head -1 | cut -d: -f1)
    l_ea=$(grep -n -- "\[test\] EquipAvatar NetId 2" "$h" | head -1 | cut -d: -f1)
    if [[ -n $l_sa && -n $l_ea && $l_sa -lt $l_ea ]]; then ok "équipement : après SpawnAvatar"; else ko "équipement : SpawnAvatar puis EquipAvatar (lignes ${l_sa:-?} et ${l_ea:-?})"; fi
fi

# --- Story 3.2c : armure — Armor dans les deux sens, après le Spawn et le Weapon, puis retirée --
p=$((base_port + 16))
cat > "$work/armure.txt" <<'EOS'
pause 3
armor 1.0 -
armor 1.5 It_Armor_Don_Fighter
EOS
start_host_armor() {  # comme start_host, avec --weapon et --armor
    "$host_bin" --port "$2" --max-ms "$3" --weapon "$4" --armor "$5" > "$work/$1.host.log" 2>&1 &
    host_pid=$!
    for _ in $(seq 100); do
        grep -q "transport démarré" "$work/$1.host.log" && return 0
        sleep 0.05
    done
    ko "$1 : l'hôte n'a pas démarré (scénario sauté)"; sed 's/^/    /' "$work/$1.host.log"
    kill "$host_pid" 2>/dev/null; wait "$host_pid" 2>/dev/null
    return 1
}
if start_host_armor armure $p 30000 Host_Sword It_Armor_Inquisitor; then
    h=$work/armure.host.log
    "$guest" --host 127.0.0.1 --port $p --duration 2500ms --quit --armor It_Armor_Inquisitor --scenario "$work/armure.txt" \
        > "$work/armure.guest.log" 2>&1 &
    g_pid=$!
    if wait_count "$work/armure.guest.log" '<- Armor netid=1 nom="It_Armor_Inquisitor"' 1; then
        kill -HUP $host_pid  # l'hôte range son arme et retire son armure
    else
        ko "armure : Armor de l'hôte non reçu"
    fi
    wait $g_pid; rc=$?
    wait $host_pid
    g=$work/armure.guest.log
    expect_code "armure : invité" 0 $rc "$g"
    expect_log "armure : Armor de l'hôte envoyé" "$h" "Armor envoyé : NetId 1 (hôte), « It_Armor_Inquisitor »"
    expect_log "armure : Armor retiré de l'hôte reçu" "$g" '<- Armor netid=1 nom=""'
    l_weapon=$(grep -n -- "<- Weapon netid=1" "$g" | head -1 | cut -d: -f1)
    l_armor=$(grep -n -- "<- Armor netid=1" "$g" | head -1 | cut -d: -f1)
    if [[ -n $l_weapon && -n $l_armor && $l_weapon -lt $l_armor ]]; then
        ok "armure : Armor de l'hôte après son Spawn et son Weapon"
    else
        ko "armure : Armor de l'hôte après son Weapon (lignes ${l_weapon:-?} et ${l_armor:-?})"; sed 's/^/    /' "$g"
    fi
    expect_count "armure : armure de l'invité appliquée" "$h" "AvatarArmor NetId 2 « It_Armor_Inquisitor »" 1
    expect_count "armure : armure de l'invité retirée" "$h" "AvatarArmor NetId 2 «  »" 1
    expect_count "armure : nouvelle armure de l'invité" "$h" "AvatarArmor NetId 2 « It_Armor_Don_Fighter »" 1
    order=$(grep -o "AvatarArmor NetId 2 « [^»]* »" "$h" | sed -E 's/.*« (.*) »/\1/' | tr '\n' ,)
    if [[ $order == "It_Armor_Inquisitor,,It_Armor_Don_Fighter," ]]; then ok "armure : dans l'ordre"; else ko "armure : ordre $order"; fi
    l_sa=$(grep -n -- "SpawnAvatar NetId 2" "$h" | head -1 | cut -d: -f1)
    l_aa=$(grep -n -- "AvatarArmor NetId 2" "$h" | head -1 | cut -d: -f1)
    if [[ -n $l_sa && -n $l_aa && $l_sa -lt $l_aa ]]; then ok "armure : avatar vêtu après son apparition"; else ko "armure : SpawnAvatar puis AvatarArmor (lignes ${l_sa:-?} et ${l_aa:-?})"; fi
fi

# --- Story 3.3 : 80 PNJ simulés — Spawn{1}, Despawn, PV, au plus 54 entités par snapshot ----
# (53 PNJ annoncés, les autres attendent ; les impairs sortent du rayon puis reviennent ; le 0 meurt
# à 2 s, Death et non Despawn, puis « revit » à 6 s sans être réannoncé : story 3.4b.)
p=$((base_port + 17))
start_host_npcs() {  # comme start_host, avec --npcs
    "$host_bin" --port "$2" --max-ms "$3" --npcs "$4" > "$work/$1.host.log" 2>&1 &
    host_pid=$!
    for _ in $(seq 100); do
        grep -q "transport démarré" "$work/$1.host.log" && return 0
        sleep 0.05
    done
    ko "$1 : l'hôte n'a pas démarré (scénario sauté)"; sed 's/^/    /' "$work/$1.host.log"
    kill "$host_pid" 2>/dev/null; wait "$host_pid" 2>/dev/null
    return 1
}
if start_host_npcs pnj $p 30000 80; then
    h=$work/pnj.host.log
    "$guest" --host 127.0.0.1 --port $p --duration 8s --quit > "$work/pnj.guest.log" 2>&1; rc=$?
    wait $host_pid
    g=$work/pnj.guest.log
    expect_code "PNJ : invité" 0 $rc "$g"
    expect_log "PNJ : premier NetId 1000 annoncé par l'hôte" "$h" "PNJ 1000 annoncé (Spawn) : GUID "
    expect_log "PNJ : sortie du rayon" "$h" "retiré (Despawn) : hors du rayon"
    # Story 3.4b : mort → Death (jamais Despawn), une fois, et jamais réannoncé (même « revenu à la vie »).
    expect_log "PNJ : mort → Death" "$h" "PNJ 1000 mort (Death)"
    expect_count "PNJ : un seul Death" "$h" "mort (Death)" 1
    expect_count "PNJ : plus de Despawn de mort" "$h" "retiré (Despawn) : mort" 0
    expect_log "PNJ : Death reçu par l'invité" "$g" "<- Death PNJ netid=1000 (poses reçues="
    expect_count "PNJ : aucun Despawn de 1000" "$g" "<- Despawn PNJ netid=1000 " 0
    l_death=$(grep -n "PNJ 1000 mort (Death)" "$h" | head -1 | cut -d: -f1)
    n_after=$(awk -v f="${l_death:-999999999}" 'NR > f && /PNJ 1000 annoncé \(Spawn\)/' "$h" | wc -l)
    if [[ -n $l_death && $n_after -eq 0 ]]; then ok "PNJ : 1000 jamais réannoncé après sa mort"; else ko "PNJ : 1000 réannoncé après sa mort ($n_after)"; fi
    expect_log "PNJ : bilan des morts de l'invité" "$g" "Bilan PNJ : .* Death=1 "
    first=$(grep -m1 -n "annoncé (Spawn)" "$h" | cut -d: -f1)
    n53=$(awk -v f="$first" 'NR >= f && /annoncé \(Spawn\)/ { n++ } NR >= f && !/annoncé \(Spawn\)/ && n > 0 { print n; exit }' "$h")
    if [[ $n53 -eq 53 ]]; then ok "PNJ : 53 PNJ au plus au premier snapshot"; else ko "PNJ : ${n53:-?} annonces au premier snapshot (53 attendu)"; fi
    expect_log "PNJ : PNJ en attente annoncé quand une place se libère" "$h" "PNJ 1053 annoncé (Spawn)"
    expect_log "PNJ : évincé au retour de plus proches" "$h" "retiré (Despawn) : évincé par plus proche"
    expect_log "PNJ : Spawn{1} reçu par l'invité" "$g" "<- Spawn PNJ netid=1000 guid="
    expect_log "PNJ : Despawn reçu par l'invité" "$g" "<- Despawn PNJ netid=10[0-9][0-9] (poses reçues="
    expect_log "PNJ : PV suivis par l'invité" "$g" "PNJ netid=10[0-9][0-9] PV [0-9]* -> [0-9]*"
    expect_log "PNJ : réannonce (même NetId)" "$g" "(annonce 2)"
    expect_log "PNJ : bilan du faux invité" "$g" "Bilan PNJ : [1-9][0-9]* vus"
    expect_count "PNJ : rien d'ignoré par l'invité" "$g" ": ignoré (" 0
    max=$(sed -nE 's/.*au plus ([0-9]+) entités par snapshot.*/\1/p' "$g" | tail -1)
    if [[ -n $max && $max -le 54 && $max -ge 40 ]]; then
        ok "PNJ : au plus $max entités par snapshot (≤ 54)"
    else
        ko "PNJ : ${max:-?} entités par snapshot au plus (40 à 54 attendu)"; grep "Bilan PNJ" "$g"
    fi
    # Un NetId n'est jamais lié à deux GUID : chaque NetId du bilan apparaît une fois.
    dup=$(grep -oE "^.*  PNJ netid=[0-9]+" "$g" | grep -oE "netid=[0-9]+" | sort | uniq -d | wc -l)
    if [[ $dup -eq 0 ]]; then ok "PNJ : un NetId par GUID"; else ko "PNJ : NetId en double dans le bilan"; fi
    # Story 3.4 : les 8 plus proches sont animés ; Motion et MotionParam par NetId de PNJ.
    expect_count "animations PNJ : jamais plus de 8 PNJ capturés par l'hôte" "$h" "ERROR.*CaptureNpc" 0
    expect_log "animations PNJ : PNJ capturés par l'hôte" "$h" "CaptureNpc NetId 1000 : PNJ animés="
    expect_log "animations PNJ : PNJ sorti du groupe" "$h" "UncaptureNpc NetId 10[0-9][0-9]"
    expect_log "animations PNJ : premier geste d'un PNJ reçu" "$g" "<- Motion PNJ netid=10[0-9][0-9] premier geste ani=[0-9]* nom=Npc_Walk"
    animated=$(sed -nE 's/.*au plus ([0-9]+) PNJ animés en 1 s.*/\1/p' "$g" | tail -1)
    if [[ -n $animated && $animated -ge 8 && $animated -le 16 ]]; then
        ok "animations PNJ : $animated PNJ animés en 1 s au plus (8 à la fois, relèves comprises)"
    else
        ko "animations PNJ : ${animated:-?} PNJ animés en 1 s au plus (8 à 16 attendu)"; grep "Bilan animations" "$g"
    fi
    with_params=$(grep -cE "^.*  PNJ netid=[0-9]+ .* Motion=[1-9][0-9]* MotionParam=[1-9]" "$g")
    if [[ $with_params -ge 8 ]]; then ok "animations PNJ : $with_params PNJ avec Motion et MotionParam au bilan"; else ko "animations PNJ : ${with_params:-0} PNJ animés au bilan (8 au moins attendus)"; fi
fi

# --- Story 3.5 : HitRequest du faux invité (v12) → ApplyHit chez l'hôte ; NetId inconnu refusé ------
# 3 PNJ simulés : 1000 (le plus proche) reçoit les coups jusqu'à sa mort (2 s), puis le suivant.
p=$((base_port + 19))
if start_host_npcs coups $p 30000 3; then
    h=$work/coups.host.log
    g=$work/coups.guest.log
    "$guest" --host 127.0.0.1 --port $p --duration 4s --quit --hit-interval 500ms > "$g" 2>&1; rc=$?
    wait $host_pid
    expect_code "coups : invité" 0 $rc "$g"
    expect_log "coups : HitRequest envoyés par l'invité" "$g" "-> HitRequest PNJ netid=1000"
    expect_log "coups : ApplyHit chez l'hôte" "$h" "\[test\] ApplyHit NetId 1000 "
    # Un HitRequest croisé avec le Death de 1000 peut être refusé « PNJ mort » : seul « NetId » est exclu.
    expect_count "coups : aucun HitRequest d'un NetId inconnu" "$h" "HitRequest PNJ [0-9]* ignoré : NetId" 0
    # Un coup = 10 PV d'un snapshot à l'autre (l'usure simulée ne fait que 4 PV/s).
    drop=$(sed -nE 's/.*PNJ netid=1000 PV ([0-9]+) -> ([0-9]+).*/\1 \2/p' "$g" | awk '$1 - $2 >= 10 { n++ } END { print n + 0 }')
    if [[ $drop -ge 1 ]]; then ok "coups : PV de 1000 en baisse de 10 dans les snapshots ($drop fois)"; else ko "coups : aucune baisse de 10 PV de 1000 vue par l'invité"; grep "netid=1000 PV" "$g"; fi
    expect_log "coups : bilan de l'invité" "$g" "Bilan coups : HitRequest envoyés=[1-9]"
fi
p=$((base_port + 20))
if start_host_npcs coups_inconnu $p 30000 3; then
    h=$work/coups_inconnu.host.log
    g=$work/coups_inconnu.guest.log
    "$guest" --host 127.0.0.1 --port $p --duration 2s --quit --hit-interval 500ms --hit-target 1500 > "$g" 2>&1; rc=$?
    wait $host_pid
    expect_code "coups NetId inconnu : invité" 0 $rc "$g"
    expect_log "coups NetId inconnu : HitRequest envoyé" "$g" "-> HitRequest PNJ netid=1500"
    expect_log "coups NetId inconnu : refusé par l'hôte" "$h" "HitRequest PNJ 1500 ignoré : NetId inconnu"
    expect_count "coups NetId inconnu : aucune commande" "$h" "ApplyHit" 0
fi

# --- Story 3.6 : coups d'un monstre sur l'avatar (simulés par --hit-guest-ms) → HitOnGuest (v13) ------
# Port base + 21 : base + 20 est déjà pris par le scénario « coups NetId inconnu ».
p=$((base_port + 21))
if start_host coups_subis $p 30000 1 --npcs 1 --hit-guest-ms 500; then
    h=$work/coups_subis.host.log
    g=$work/coups_subis.guest.log
    "$guest" --host 127.0.0.1 --port $p --duration 3s --quit > "$g" 2>&1; rc=$?
    wait $host_pid
    expect_code "coups subis : invité" 0 $rc "$g"
    expect_log "coups subis : HitOnGuest envoyé par l'hôte" "$h" "HitOnGuest envoyé, dégâts bruts 18"
    expect_log "coups subis : HitOnGuest reçu par l'invité" "$g" "<- HitOnGuest source=1000 dégâts=18"
    # Un HitOnGuest reçu par coup injecté (au plus un en vol au départ de l'invité).
    sent=$(grep -c "HitOnGuest envoyé, dégâts bruts 18" "$h")
    received=$(grep -c "<- HitOnGuest source=1000 dégâts=18" "$g")
    if [[ $received -ge 4 && $received -le $sent && $((sent - received)) -le 1 ]]; then
        ok "coups subis : $received HitOnGuest reçus pour $sent envoyés"
    else
        ko "coups subis : $received HitOnGuest reçus pour $sent envoyés (4 au moins, écart ≤ 1 attendu)"
    fi
    expect_log "coups subis : bilan de l'invité" "$g" "HitOnGuest reçus=[1-9]"
    # Story 3.8 : début d'attaque du PNJ 1000 avant chaque coup, tant qu'il vit (NpcAttack, v15).
    expect_log "attaques natives : NpcAttack envoyé par l'hôte" "$h" "attaque du PNJ 1000 (mouvement 11, cible 2) : NpcAttack envoyé"
    expect_log "attaques natives : NpcAttack reçu par l'invité" "$g" "<- NpcAttack netid=1000 move=11 cible=2"
    expect_log "attaques natives : bilan de l'invité" "$g" "NpcAttack reçus=[1-9]"
fi

# --- Story 3.8b : début d'attaque du héros invité (NpcAttack{2}) → ApplyAvatarAttack chez l'hôte ------
p=$((base_port + 23))
if start_host_npcs attaques_avatar $p 30000 3; then
    h=$work/attaques_avatar.host.log
    g=$work/attaques_avatar.guest.log
    "$guest" --host 127.0.0.1 --port $p --duration 3s --quit --hit-interval 500ms --attack-move 4 > "$g" 2>&1; rc=$?
    wait $host_pid
    expect_code "attaques de l'avatar : invité" 0 $rc "$g"
    expect_log "attaques de l'avatar : NpcAttack{2} envoyé" "$g" "-> NpcAttack netid=2 move=4 cible=1000"
    expect_log "attaques de l'avatar : ApplyAvatarAttack chez l'hôte" "$h" "\[test\] ApplyAvatarAttack mouvement 4 cible 1000"
    expect_count "attaques de l'avatar : aucun refus" "$h" "NpcAttack .* ignoré" 0
    expect_log "attaques de l'avatar : bilan de l'invité" "$g" "NpcAttack reçus=0 envoyés=[1-9]"
fi
p=$((base_port + 24))
if start_host_npcs attaques_netid $p 30000 3; then
    h=$work/attaques_netid.host.log
    g=$work/attaques_netid.guest.log
    "$guest" --host 127.0.0.1 --port $p --duration 2s --quit --hit-interval 500ms --attack-move 4 --attack-netid 1001 > "$g" 2>&1; rc=$?
    wait $host_pid
    expect_code "attaques NetId ≠ 2 : invité" 0 $rc "$g"
    expect_log "attaques NetId ≠ 2 : refusé par l'hôte" "$h" "NpcAttack PNJ 1001 (mouvement 4, cible 1000) ignoré : l'hôte fait autorité"
    expect_count "attaques NetId ≠ 2 : aucune commande" "$h" "ApplyAvatarAttack" 0
fi

# --- Story 4.1 : répliques de dialogue de l'hôte (héros et PNJ 1002) → DialogLine chez l'invité ---
p=$((base_port + 25))
if start_host dialogues $p 30000 1 --npcs 3 --dialog-ms 400; then
    h=$work/dialogues.host.log
    g=$work/dialogues.guest.log
    "$guest" --host 127.0.0.1 --port $p --duration 3s --quit > "$g" 2>&1; rc=$?
    wait $host_pid
    expect_code "dialogues : invité" 0 $rc "$g"
    expect_log "dialogues : réplique du héros envoyée" "$h" "réplique de dialogue du héros (type 3, « INFO_Test_1_PC_Hero_DE_%_PC_Hero.wav ») : DialogLine envoyé"
    expect_log "dialogues : réplique du PNJ envoyée" "$h" "réplique de dialogue du PNJ 1002 (type 0, « INFO_Test_2_Sara_DE_%_Sara.wav ») : DialogLine envoyé"
    expect_log "dialogues : héros reçu par l'invité" "$g" "<- DialogLine locuteur=1 type=3 échantillon=INFO_Test_1_PC_Hero_DE_%_PC_Hero.wav"
    expect_log "dialogues : PNJ reçu par l'invité" "$g" "<- DialogLine locuteur=1002 type=0 échantillon=INFO_Test_2_Sara_DE_%_Sara.wav"
    expect_log "dialogues : bilan de l'invité" "$g" "Bilan dialogues : DialogLine reçus=[1-9]"
fi

# --- Story 4.2 : dialogue de l'invité (DialogState, DialogLine{2} et du PNJ) → tenue et rejeu chez l'hôte ---
p=$((base_port + 26))
if start_host_npcs dialogues_invite $p 30000 3; then
    h=$work/dialogues_invite.host.log
    g=$work/dialogues_invite.guest.log
    "$guest" --host 127.0.0.1 --port $p --duration 3s --quit --dialog-every 800ms > "$g" 2>&1; rc=$?
    wait $host_pid
    expect_code "dialogues de l'invité : invité" 0 $rc "$g"
    expect_log "dialogues de l'invité : DialogState envoyé" "$g" "-> DialogState pnj=1000 actif=1"
    expect_log "dialogues de l'invité : PNJ tenu" "$h" "\[test\] HoldNpc NetId 1000"
    expect_log "dialogues de l'invité : tenue journalisée" "$h" "PNJ 1000 tenu (routine figée) pendant le dialogue de l'invité"
    expect_log "dialogues de l'invité : réplique du héros rejouée" "$h" "\[test\] ApplyDialogLine locuteur 2 type 3 « INFO_Sara_1_PC_Hero_DE_%_PC_Hero.wav »"
    expect_log "dialogues de l'invité : réplique du PNJ rejouée" "$h" "\[test\] ApplyDialogLine locuteur 1000 type 0 « INFO_Sara_2_Sara_DE_%_Sara.wav »"
    expect_log "dialogues de l'invité : fin, PNJ rendu" "$h" "\[test\] UnholdNpc NetId 1000"
    expect_log "dialogues de l'invité : bilan de l'invité" "$g" "Bilan dialogues : DialogLine reçus=0 envoyés=[1-9]"
fi
p=$((base_port + 27))
if start_host_npcs dialogues_netid $p 30000 3; then
    h=$work/dialogues_netid.host.log
    g=$work/dialogues_netid.guest.log
    "$guest" --host 127.0.0.1 --port $p --duration 2s --quit --dialog-every 500ms --dialog-npc 4242 > "$g" 2>&1; rc=$?
    wait $host_pid
    expect_code "dialogues NetId inconnu : invité" 0 $rc "$g"
    expect_log "dialogues NetId inconnu : refusé par l'hôte" "$h" "DialogState (PNJ 4242, début) ignoré (PNJ non annoncé)"
    expect_count "dialogues NetId inconnu : aucun PNJ tenu" "$h" "HoldNpc" 0
    expect_count "dialogues NetId inconnu : réplique du PNJ non rejouée" "$h" "ApplyDialogLine locuteur 4242" 0
fi

# --- Story 3.7 : invité à terre toutes les secondes : Respawn de l'hôte, accusé de l'invité -----
p=$((base_port + 22))
if start_host respawn $p 30000; then
    h=$work/respawn.host.log
    g=$work/respawn.guest.log
    "$guest" --host 127.0.0.1 --port $p --die-every 1s --duration 4s --quit > "$g" 2>&1; rc=$?
    wait $host_pid
    expect_code "réapparition : invité" 0 $rc "$g"
    expect_log "réapparition : Death reçu par l'hôte" "$h" "Death reçu : invité à terre, Respawn à envoyer"
    expect_log "réapparition : Respawn envoyé" "$h" "Respawn envoyé : invité replacé"
    expect_log "réapparition : confirmée par l'invité" "$h" "réapparition confirmée par l'invité (tick [1-9]"
    expect_log "réapparition : Respawn reçu par l'invité" "$g" "<- Respawn netid=2 pose="
    expect_log "réapparition : accusé envoyé" "$g" "-> Respawn accusé tick="
    expect_count "réapparition : jamais de ragdoll" "$h" "AvatarDeath" 0
    # À chaque mort : un Respawn ; une confirmation par Respawn, sauf peut-être le dernier (Death{2}
    # envoyé juste avant le départ de l'invité à 4 s : Respawn sans accusé).
    deaths=$(grep -c "Death reçu : invité à terre" "$h")
    sent=$(grep -c "Respawn envoyé : invité replacé" "$h")
    confirmed=$(grep -c "réapparition confirmée par l'invité" "$h")
    if [[ $deaths -ge 2 && $sent -eq $deaths && ( $confirmed -eq $sent || $confirmed -eq $((sent - 1)) ) ]]; then
        ok "réapparition : $deaths morts, $sent Respawn, $confirmed confirmations"
    else
        ko "réapparition : $deaths morts, $sent Respawn, $confirmed confirmations (≥ 2, Respawn = morts, confirmations = Respawn ou Respawn − 1 attendus)"
        sed 's/^/    /' "$h"
    fi
fi

# --- Story 3.4a : jeu de l'hôte en pause 7 s (cinématique, chargement) : pas de timeout ------
# Le faux invité --strict-timeout ne compte que les messages de l'hôte : seuls les heartbeats de
# secours du thread réseau de l'hôte maintiennent la session pendant la pause.
p=$((base_port + 18))
if start_host pause $p 30000 1 --pause-at 1000 --pause-ms 7000; then
    h=$work/pause.host.log
    g=$work/pause.guest.log
    "$guest" --host 127.0.0.1 --port $p --duration 10s --quit --strict-timeout > "$g" 2>&1; rc=$?
    wait $host_pid
    expect_code "pause : invité" 0 $rc "$g"
    expect_log "pause : pause simulée par l'hôte" "$h" "\[test\] pause du jeu simulée : 7000 ms"
    expect_count "pause : pas de timeout chez l'hôte" "$h" "timeout" 0
    expect_count "pause : pas de timeout chez l'invité" "$g" "timeout" 0
    expect_log "pause : fin par l'invité" "$h" "InSession → Ended : fin de session par le pair : départ du joueur (1)"
    n=$(sed -nE 's/.*\[sync\] jeu en pause : ([0-9]+) heartbeat\(s\) envoyé\(s\) par le thread réseau.*/\1/p' "$h" | head -1)
    if [[ -n $n && $n -ge 4 ]]; then
        ok "pause : $n heartbeat(s) de secours journalisés à la reprise (≥ 4)"
    else
        ko "pause : ${n:-aucun} heartbeat de secours journalisé (≥ 4 attendus)"; sed 's/^/    /' "$h"
    fi
    expect_count "pause : borne de 60 s non atteinte" "$h" "jeu en pause plus de" 0
fi

# --- Story 1.5 : mort de l'hôte en session → SessionEnd{3} ----------------------------
p=$((base_port + 9))
if start_host death $p 20000; then
    h=$work/death.host.log
    "$guest" --host 127.0.0.1 --port $p > "$work/death.guest.log" 2>&1 &
    g_pid=$!
    if wait_count "$h" "→ InSession" 1; then
        kill -USR2 $host_pid
        wait $g_pid; rc=$?
        wait $host_pid
    else
        abort_scenario "mort : pas de session" $g_pid; rc=-1
    fi
    expect_code "mort : invité sorti sur SessionEnd" 0 $rc "$work/death.guest.log"
    expect_log "mort : SessionEnd{3} reçu par l'invité" "$work/death.guest.log" "<- SessionEnd raison=3"
    expect_log "mort : Ended côté hôte" "$h" "InSession → Ended : mort de l'hôte"
    expect_log "mort : pas de réarmement dans le même monde" "$h" "nouvelle session au prochain monde chargé"
fi

echo "$failures vérification(s) en échec"
exit $(( failures > 0 ))

#!/usr/bin/env bash
# Hôte de session Linux (session_host : core/session + net::Transport) face au faux invité Go
# (tools/fake-guest), stories 1.4, 1.5, 2.2 et 2.3 : preuve du comportement de l'hôte sans le jeu.
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

# start_host <nom> <port> <max-ms> [fins] : lance l'hôte en arrière-plan, attend que le
# transport écoute. Échec : log de l'hôte affiché, hôte arrêté, code non nul (scénario sauté).
start_host() {
    "$host_bin" --port "$2" --max-ms "$3" --ends "${4:-1}" > "$work/$1.host.log" 2>&1 &
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

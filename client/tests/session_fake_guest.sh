#!/usr/bin/env bash
# Hôte de session Linux (session_host : core/session + net::Transport) face au faux invité Go
# (tools/fake-guest), story 1.4 : preuve du comportement de l'hôte sans le jeu.
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

# start_host <nom> <port> <max-ms> : lance l'hôte en arrière-plan, attend que le transport
# écoute. Échec : log de l'hôte affiché, hôte arrêté, code non nul (scénario sauté).
start_host() {
    "$host_bin" --port "$2" --max-ms "$3" > "$work/$1.host.log" 2>&1 &
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
    expect_log "nominal : état final" "$h" "état final : Ended"
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

echo "$failures vérification(s) en échec"
exit $(( failures > 0 ))

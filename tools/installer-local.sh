#!/usr/bin/env bash
# Installe la DLL compilée et les configs de test dans les deux copies locales de Risen :
# ~/Games/Risen-host (hôte) et ~/Games/Risen-invite (invité sur 127.0.0.1).
# Lancement ensuite : ~/Games/lancer-risen-coop.sh (ou lancer-risen-host.sh / lancer-risen-invite.sh).
# Les logs de la session précédente sont gardés en Script_Multiplayer.log.prev.
# PROBE=1 tools/installer-local.sh : sonde activée (probe = 1) dans la copie hôte, à lancer seule
# (~/Games/lancer-risen-host.sh) ; l'invité reste en session normale.
set -euo pipefail

RACINE=$(cd "$(dirname "$0")/.." && pwd)
DLL="$RACINE/client/build/Script_Multiplayer.dll"
[ -f "$DLL" ] || { echo "DLL absente : cmake --build client/build" >&2; exit 1; }

installer() {  # $1 = dossier scripts du jeu, stdin = multiplayer.ini
    local dir=$1
    [ -d "$dir" ] || { echo "dossier absent : $dir" >&2; exit 1; }
    [ -f "$dir/Script_Multiplayer.log" ] && mv -f "$dir/Script_Multiplayer.log" "$dir/Script_Multiplayer.log.prev"
    cp "$DLL" "$dir/"
    cat >"$dir/multiplayer.ini"
}

installer ~/Games/Risen-host/bin/scripts <<EOF
role = host
port = 27960
nickname = Leoyd
guest_equipment =
probe = ${PROBE:-0}
damage_hook = 1
overlay = 1
log_level = debug
EOF

installer ~/Games/Risen-invite/bin/scripts <<'EOF'
role = guest
host_ip = 127.0.0.1
port = 27960
nickname = Invite
guest_equipment = It_1H_Sword
probe = 0
damage_hook = 1
overlay = 1
log_level = debug
EOF

echo "Installé ($(git -C "$RACINE" rev-parse --short HEAD), $(git -C "$RACINE" branch --show-current))"
sha256sum ~/Games/Risen-host/bin/scripts/Script_Multiplayer.dll ~/Games/Risen-invite/bin/scripts/Script_Multiplayer.dll

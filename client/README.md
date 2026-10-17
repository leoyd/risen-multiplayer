# Client — `Script_Multiplayer.dll`

DLL chargée nativement par Risen (Steam, x64) depuis `bin/scripts/`, comme le module
officiel `Script_Game.dll`. Aucun injecteur, aucune option de lancement : le chargeur
de scripts du jeu (`gCScriptAdmin::LoadScriptDLL`) charge toutes les `*.dll` de ce
dossier et appelle leur export `ScriptInit`.

État story 1.4 : le mod écrit un log, vérifie la version du jeu et lit sa config. Il
n'expose qu'un script inoffensif (`MP_Probe`) et hooke `eCApplication::OnProcess` pour la
session : au chargement d'une partie, l'hôte se met à écouter et l'invité se connecte
(voir « Tester une session »).

## Prérequis (Fedora)

```bash
sudo dnf install clang lld llvm cmake ninja-build
# CRT + SDK Windows (licence Microsoft à accepter), extraits localement, jamais commités :
xwin --accept-license --arch x86_64 splat --output ~/.xwin
```

`xwin` : binaire publié sur <https://github.com/Jake-Shadle/xwin/releases>.
Autre emplacement : `-DXWIN_DIR=/chemin` ou variable d'environnement `XWIN_DIR`.

## Build

Depuis la racine du dépôt :

```bash
# Tests unitaires (natif Linux, sans le jeu)
cmake -S client -B client/build-tests && cmake --build client/build-tests && ctest --test-dir client/build-tests

# DLL Windows x64 (clang-cl + lld-link + xwin)
cmake -S client -B client/build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/clang-cl-xwin.cmake
cmake --build client/build
objdump -p client/build/Script_Multiplayer.dll | grep ScriptInit   # export présent
```

### Scénarios d'intégration sous Wine (`wine_scenarios`)

`tests/wine/run_scenarios.sh` rejoue la matrice I/O de J0 (nominal, config absente,
champ invalide, version inconnue, dossier en lecture seule) avec `mp_harness.exe`, qui
charge les vraies DLL du jeu de `risen_ori/bin/` puis appelle `ScriptInit`. Le test
CTest `wine_scenarios` n'est enregistré que si `wine`, `risen_ori/bin/Game.dll`,
`client/build/mp_harness.exe` et `client/build/Script_Multiplayer.dll` existent **au
moment de la configuration**. Ordre à respecter :

```bash
cmake --build client/build                       # 1. build Windows d'abord
cmake -S client -B client/build-tests            # 2. (re)configurer les tests
cmake --build client/build-tests && ctest --test-dir client/build-tests   # 3. lance aussi wine_scenarios
```

Préfixe Wine utilisé : `client/build/wineprefix` (créé au premier lancement), ou
`MP_WINEPREFIX` s'il est défini.

### Transport sous Wine (`wine_net`)

`net_tests.exe` (build Windows) contient les mêmes tests du transport `src/net/` que
`client_tests` : boucle locale UDP, KCP avec et sans pertes simulées, canaux ignorés, port
occupé, arrêts répétés, file pleine. Le test CTest `wine_net` le lance sous Wine ; il n'est
enregistré que si `wine` et `client/build/net_tests.exe` existent au moment de la
configuration (même ordre que ci‑dessus ; les DLL du jeu ne sont pas nécessaires).

Le chemin du toolchain est relatif au dossier source `client/`. La DLL est liée au CRT
statique : ses seules dépendances sont `KERNEL32.dll`, `USER32.dll` (touches de la sonde) et
`WS2_32.dll` (transport de la session) :

```bash
llvm-objdump -p client/build/Script_Multiplayer.dll | grep 'DLL Name'   # KERNEL32, USER32, WS2_32
```

### Faux invité (`fake_guest`)

`tools/fake-guest/` est un invité de session en Go (outil de test, jamais livré) qui sert à
tester l'hôte sans second Risen. Il compile le même `third_party/kcp/ikcp.c` via cgo et vérifie
son codec contre `protocol/vectors/`. Le test CTest `fake_guest` lance `go test ./...` dans ce
dossier ; il n'est enregistré que si `go` est trouvé (cgo demande aussi `gcc`). Usage, fautes
(`--drop`, `--quit`, `--kill`, `--dup-hello`, `--bad-version`) et codes de sortie :
[`../tools/fake-guest/README.md`](../tools/fake-guest/README.md).

### Hôte de session face au faux invité (`session_fake_guest`)

`session_host` (build Linux) est un hôte sans le jeu : la **même** machine à états que la DLL
(`src/core/session`) avec le vrai transport (`src/net`) et un monde simulé chargé. Il sort
après `--ends E` fins de session (défaut 1) ou `--max-ms T`. `SIGUSR1` simule une fin de monde
(rechargement : le héros change d'adresse) et `SIGUSR2` la mort du héros de l'hôte.
`tests/session_fake_guest.sh` le lance face au faux invité (compilé à la volée par `go build`)
et vérifie les codes de sortie et les logs : nominal, `--dup-hello`, `--bad-version`, deux
invités (le second reçoit `Reject{4}`), `--drop 20`, `--kill` (timeout de l'hôte), puis
(story 1.5) trois cycles « rejoindre, fin de monde, réarmer » avec `SessionEnd{2}` reçu par le
faux invité et trois `conv` distincts, le réarmement de l'hôte après `--quit` (un second invité
rejoint le même monde) et la mort de l'hôte (`SessionEnd{3}`). Le test CTest
`session_fake_guest` est enregistré avec `fake_guest` (si `go` est trouvé) ; il dure environ
45 s. Lancement à la main :

```bash
client/tests/session_fake_guest.sh client/build-tests/session_host tools/fake-guest
```

Avec les deux builds faits dans l'ordre ci‑dessus, `ctest --test-dir client/build-tests` lance
6 tests : `client_tests`, `protocol_vectors`, `fake_guest`, `session_fake_guest`,
`wine_scenarios`, `wine_net`.

## Installation

1. Copier `client/build/Script_Multiplayer.dll` dans `<Steam>/steamapps/common/Risen/bin/scripts/`.
2. Copier `client/multiplayer.example.ini` au même endroit sous le nom `multiplayer.ini`, puis l'adapter.
3. Lancer Risen normalement (Windows ou Proton).
4. Lire `bin/scripts/Script_Multiplayer.log` (réécrit à chaque lancement).

Désinstallation : supprimer ces fichiers.

Log attendu (cas nominal) — chaque ligne est préfixée par `[AAAA-MM-JJ HH:MM:SS.mmm] NIVEAU` :

```
[2026-10-15 22:42:24.559] INFO  Risen Multiplayer 0.1.0 chargé depuis …\bin\scripts
[2026-10-15 22:42:24.559] INFO  empreinte Game.dll : TimeDateStamp=0x63c9aff6 SizeOfImage=0x1417000
[2026-10-15 22:42:24.559] INFO  empreinte Engine.dll : TimeDateStamp=0x63ca945d SizeOfImage=0xd3a000
[2026-10-15 22:42:24.559] INFO  version du jeu : Steam x64 (janvier 2023)
[2026-10-15 22:42:24.559] INFO  config OK (rôle=hôte, port=27960, pseudo=Joueur, log_level=info)
[2026-10-15 22:42:24.559] INFO  script « MP_Probe » exposé au chargeur
[2026-10-15 22:42:24.559] INFO  mod actif
```

La clé `log_level` (`debug` ou `info`, défaut `info`) est lue et rappelée dans la ligne
« config OK ». En `debug`, la session écrit en plus les empreintes locales, les `Hello` envoyés,
les heartbeats reçus et les datagrammes ignorés ; rien n'est écrit à chaque frame en `info`.

Version non reconnue (mise à jour Steam), config absente ou invalide : le log en donne
la raison, le mod reste inactif et le jeu fonctionne en solo. Sur une version non
reconnue, le mod n'appelle aucun code du jeu et rend des tables vides : le chargeur
de Risen l'ignore alors silencieusement (pas de boîte de dialogue). Après une mise à jour du
jeu, il faut revérifier la rétro-ingénierie puis ajouter la nouvelle empreinte à la
liste blanche (`src/core/version.cpp`).

## Tester une session (`probe = 0`, stories 1.4 et 1.5)

Sans sonde, le mod hooke seulement `eCApplication::OnProcess`. Au premier appel où le monde est
chargé (`Entity::GetPlayer()` non nul **et** `gCSession::IsGameRunning()`), la session démarre, avec le préfixe
`[sync]` au log pour chaque transition, chaque `Hello` reçu et chaque `Reject` :

- **hôte** (`role = host`) : `Inactive → Hosting`, écoute sur `port` ; un invité compatible
  passe la session en `InSession` (le `Welcome` porte la position et le lacet du joueur hôte) ;
- **invité** (`role = guest`, `host_ip` = **IPv4 littérale**) : `Inactive → Connecting`,
  `Hello` toutes les 500 ms, abandon après 10 s (« hôte injoignable ») ;
- en session : heartbeat à 1 Hz, `Ended` après 5 s sans nouvelles du pair (« timeout ») ou
  sur `SessionEnd` reçu.

Réseau : l'hôte écoute sur le port UDP `port` (27960 par défaut), à ouvrir dans le pare-feu et
à rediriger (UDP) sur la box si l'invité vient d'Internet. L'invité se connecte à ce port depuis
un port local éphémère : rien à ouvrir de son côté.

**Fin de session (story 1.5).** Toutes les fins passent par la même transition : si la cause
est locale et qu'on est en session, `SessionEnd{raison}` part (2 = fin de monde, 3 = mort de
l'hôte, 4 = erreur) ; puis `… → Ended : <raison> ; retour au solo` ; le KCP est gardé **1 s**
pour laisser partir le `SessionEnd`, puis `Ended → Inactive : transport arrêté ; …`. Causes :

- **fin de monde** (rechargement, chargement rapide, retour au menu) : le joueur devient nul,
  change d'adresse, ou `IsGameRunning` retombe, en `Hosting`, `Connecting` ou `InSession` ;
- **mort de l'hôte** (rôle hôte, `Hosting` ou `InSession`) : `Entity::IsDead` du héros ;
- timeout, `SessionEnd` reçu, `Reject` reçu, hôte injoignable, erreur.

Réarmement : au prochain monde chargé, avec un `conv` neuf. Exception : l'hôte dont l'invité
part (timeout ou `SessionEnd` reçu) se réarme **aussitôt** dans le même monde (« réarmement
immédiat »). L'invité, lui, attend toujours le monde suivant (pas de boucle de `Reject`).
Quitter le jeu n'envoie rien : le pair conclut au timeout de 5 s. Une erreur de config (nom
d'hôte au lieu d'une IPv4) laisse le jeu en solo jusqu'au redémarrage.

Diagnostic : avec `log_level = debug`, une ligne `[sync] signaux : player=0x… running=0|1
dead=0|1 (état …)` est écrite à chaque changement d'un des trois signaux (jamais à chaque
frame). Elle sert au test en jeu ci-dessous.

### Risen hôte + faux invité (une machine)

1. `multiplayer.ini` : `role = host`, `probe = 0`. Lancer Risen et charger une partie : le log
   montre `[sync] Inactive → Hosting : monde chargé, en attente d'un invité sur le port 27960`.
2. Sur la même machine : `tools/fake-guest/fake-guest --host 127.0.0.1 --duration 10s`
   (`--duration 10s` : le faux invité tient la session 10 s après le `Welcome`, puis envoie
   `SessionEnd{1}` et sort).
3. Attendu dans `Script_Multiplayer.log` : `Hello reçu de 127.0.0.1:…`, puis
   `Hosting → InSession : invité … accepté, conv=0x…, pose envoyée (…)`, puis environ 10 s plus
   tard `InSession → Ended : fin de session par le pair : départ du joueur (1)`. Le faux invité
   sort avec le code 0. Variantes : `--bad-version` (log : `Reject envoyé … (1)`, l'hôte reste
   en attente), `--kill` (log : `timeout` 5 s après la fin).

### Fin de session en jeu (story 1.5, une machine)

Prérequis : `role = host`, `probe = 0`, **`log_level = debug`**, une sauvegarde rapide.

1. **Trois rechargements puis menu.** Charger la partie, puis lancer en boucle le faux invité,
   qui se reconnecte après chaque fin :
   `while true; do tools/fake-guest/fake-guest --host 127.0.0.1; sleep 0.5; done`
   (il sort sur `SessionEnd` reçu, ou après 10 s sans réponse pendant un chargement ; la
   boucle le relance). Attendre
   `Hosting → InSession`, faire un chargement rapide, attendre la session suivante, et ainsi
   trois fois ; puis revenir au menu principal. Arrêter la boucle (Ctrl-C).
2. Attendu dans `Script_Multiplayer.log` : trois `Hosting → InSession` avec trois `conv`
   distincts, chacun suivi de `SessionEnd envoyé : fin de monde (2)`,
   `InSession → Ended : fin de monde ; retour au solo`, puis `Ended → Inactive` ; après le
   retour au menu, pas de nouveau `Inactive → Hosting`. Côté faux invité :
   `<- SessionEnd raison=2` trois fois. Aucun crash.
3. **Mort de l'hôte.** Recharger une partie, relancer la boucle du faux invité de l'étape 1,
   attendre `Hosting → InSession`, puis faire mourir le héros
   (chute, combat) : `SessionEnd envoyé : mort de l'hôte (3)`, `InSession → Ended : mort de
   l'hôte`, et `<- SessionEnd raison=3` côté faux invité. Arrêter la boucle (Ctrl-C).
4. Recopier les lignes `[sync] signaux` (et les transitions qui les entourent) dans
   [`../docs/re/fin-de-monde.md`](../docs/re/fin-de-monde.md), section « Résultats du test en
   jeu ». Point à surveiller : `running` retombe-t-il pendant un chargement rapide, et le
   pointeur `player` change-t-il ?

### Risen invité, hôte injoignable (une machine)

1. `multiplayer.ini` : `role = guest`, `host_ip` = une IPv4 où personne n'écoute (ex.
   `127.0.0.1` avec un port libre), `probe = 0`.
2. Charger une partie : `[sync] Inactive → Connecting : monde chargé, connexion à …`, puis
   environ 10 s plus tard `Connecting → Ended : hôte injoignable (aucune réponse en 10 s)`. Le jeu
   reste jouable.

Le reste du côté invité (Welcome reçu, session, Reject reçu) ne se vérifie qu'avec **deux
machines** : deux Risen ne tournent pas sur la même machine. Il est couvert sous Linux par les
tests de `core/session` (`tests/test_session.cpp`).

## Sonde du spike J1 (`probe = 1`)

Mode de rétro-ingénierie, **désactivé par défaut**. Avec `probe = 1` dans
`multiplayer.ini`, le mod hooke `eCApplication::OnProcess` et `eCAnimation_PS::PlayMotion`
(Engine.dll) et journalise, avec le préfixe `[probe]` :

- une ligne `tick` par seconde : thread (identique au premier appel ?), images/s,
  joueur présent ou non, position et matrice brute du joueur ;
- une ligne `PlayMotion` par animation lancée sur le joueur : type, **nom** de l'animation,
  `flag` et les 7 scalaires du descripteur lus par le moteur (`d00` … `d18`) (20 lignes/s au
  plus, le surplus est compté) ;
- une ligne `hook PlayMotion : thread=…` au premier appel du hook : le thread est-il celui
  d'`OnProcess` (AD-2) ?
- **F6** (fenêtre du jeu active) : fait apparaître une copie temporaire du joueur environ
  2 m devant lui, et indique si le joueur a changé après l'apparition ;
- **F7** : active ou coupe le **miroir par nom** (story 2.1). La copie rejoue chaque
  animation du joueur par le chemin de l'avatar : `Motion` encodé puis décodé, nom →
  `eCResource2::GetMotion` → `SetMotion` → `PlayMotion` avec un descripteur reconstruit (ni
  pointeur de ressource ni octets bruts). Elle suit ses déplacements en gardant son décalage
  (`Entity::MoveTo`). Les animations que le moteur lance de lui-même sur la copie sont
  bloquées. La première résolution de chaque nom est journalisée (`GetMotion(« … ») = …`,
  identique ou non à la ressource capturée). Tant qu'une copie existe, une ligne `copie` par
  seconde donne son état : rejoués, échecs, bloqués, écarts de pointeur ;
- **F4** : rejoue, par le même chemin, le dernier coup capturé sur le PNJ le plus proche
  (15 m au plus).

Protocole de test (story 2.1) : charger une partie, F6 face à un espace dégagé, F7, puis
marcher, courir, sauter, frapper (plusieurs coups), et se laisser mourir, en regardant la
copie. Attendu : la copie reproduit les gestes comme le miroir v5 du spike J1
(`docs/re/spike-j1.md`) ; les lignes `copie` montrent `0 échecs` et `0 écarts de pointeur`.
Quitter, et fournir `Script_Multiplayer.log`.

Avec `probe = 0` ou sans la clé, la sonde est désactivée et seule la
session est installée (hook `OnProcess` seul). Sonde et session sont exclusives.

## Organisation

```
cmake/clang-cl-xwin.cmake   toolchain croisée Windows x64
src/core/                   logique pure (config, version, codec du protocole, machine à états de session), testée sous Linux
src/common/                 log
src/net/                    transport UDP + KCP : socket, canaux, thread réseau (testé sous Linux et Wine)
src/game/                   adaptateur ABI du jeu (GetProcAddress) — seul code qui appelle le jeu
src/sync/                   pilote de session (tick par frame) ; filtre PlayMotion (AnimFilter) ; sonde du spike J1
src/win/                    ScriptInit, hooks MinHook
third_party/minhook/        MinHook, figé sur un commit (voir VERSION.txt)
third_party/kcp/            KCP 2.1.1, figé sur un commit (voir VERSION.txt), compilé dans mp_net
tests/                      tests unitaires (dont vecteurs de ../protocol/vectors/) (mini-framework maison, sans dépendance) + wine/
```

Le contrat binaire hôte ⇄ invité est décrit dans [`../protocol/PROTOCOL.md`](../protocol/PROTOCOL.md)
(règles de session au §8) ;
son codec est `src/core/protocol.{h,cpp}` (`mp::protocol`), vérifié contre les vecteurs
`../protocol/vectors/*.bin` (`protocol/vectors/regen.sh --check`).

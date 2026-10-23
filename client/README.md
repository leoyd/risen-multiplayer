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
`--weapon NOM` met une arme dans la main du héros de l'hôte (story 3.2), que `SIGHUP` range ou
ressort ; `--armor NOM` lui met une armure (story 3.2c), que ce même `SIGHUP` retire ou remet.
`--npcs N` (story 3.3) simule N PNJ autour du héros de l'hôte (le PNJ i à 5 m + 50 cm × i, PV qui
baissent ; de 1,5 à 2,5 s après l'entrée en session, les impairs sortent du rayon puis reviennent ;
le PNJ 0 meurt à 2 s), lus seulement quand `Session::wants_npcs` le demande, comme dans la DLL.
Chaque `ApplyHit` (story 3.5) est journalisé `[test] ApplyHit NetId N` et retire 10 PV simulés
au PNJ visé.
`--hit-guest-ms N` (story 3.6) injecte en session, toutes les N ms, un coup simulé sur l'avatar
(`LocalHitOnGuest{1000 si --npcs ≥ 1, sinon 0, 18}`), comme le détour des dégâts de la DLL.
`--pause-at MS --pause-ms MS` (story 3.4a) fige une fois sa boucle (ni `poll` ni tick), `MS` ms
après l'entrée en session, comme une cinématique ou un chargement ; à la reprise, il journalise la
même ligne que la DLL (`[sync] jeu en pause : N heartbeat(s) envoyé(s) par le thread réseau`).
`tests/session_fake_guest.sh` le lance face au faux invité (compilé à la volée par `go build`)
et vérifie les codes de sortie et les logs : nominal, `--dup-hello`, `--bad-version`, deux
invités (le second reçoit `Reject{4}`), `--drop 20`, `--kill` (timeout de l'hôte), puis
(story 1.5) trois cycles « rejoindre, fin de monde, réarmer » avec `SessionEnd{2}` reçu par le
faux invité et trois `conv` distincts, le réarmement de l'hôte après `--quit` (un second invité
rejoint le même monde) et la mort de l'hôte (`SessionEnd{3}`), puis (story 2.2) « rejoindre,
`--kill`, rejoindre, quitter » : un `SpawnAvatar` et un `DespawnAvatar` par session, jamais
deux avatars (`avatars présents=2` absent du log), `Despawn` avant `SessionEnd`, enfin (story 2.3)
un cercle avec 100 ms ± 20 ms de latence et 10 % de pertes : l'avatar parcourt plus de 8 m, avec
un saut max (du `Spawn` au dernier `MoveAvatar`) de moins de 10 cm ; (story 2.4) des gestes
horodatés et une mort rejoués dans l'ordre (`PlayAvatarMotion`, `AvatarDeath`), et `--echo` ;
(story 3.2) l'arme dans les deux sens : `Weapon` de l'hôte reçu après son `Spawn` puis rangé, et
celle du faux invité (`--weapon It_1H_Sword`, puis rangée et changée par le scénario) appliquée
dans l'ordre (`AvatarWeapon`), après `SpawnAvatar` ; (story 3.2b) `--equipment` reçu dans le
`Hello` et appliqué à l'avatar (`EquipAvatar`) juste après `SpawnAvatar`, rien sans l'option ;
(story 3.2c) l'armure dans les deux sens : `Armor` de l'hôte reçu après son `Spawn` et son
`Weapon`, puis retiré, et celle du faux invité (`--armor It_Armor_Inquisitor`, puis retirée et
changée par le scénario) appliquée dans l'ordre (`AvatarArmor`), après `SpawnAvatar` ; aucun
`AvatarArmor` sans `--armor` ; (story 3.3) 80 PNJ simulés (`--npcs 80`) : 53 annoncés au premier
snapshot (`Spawn{1}`, NetId dès 1000), les autres attendent, sorties du rayon, mort et éviction
(`Despawn`), réannonce sous le même NetId, PV suivis par le faux invité, au plus 54 entités par
snapshot et aucun message de PNJ ignoré ; (story 3.4a) l'hôte en pause 7 s face au faux invité
`--strict-timeout` (timeout compté sur les seuls messages, pas sur les acquittements KCP) : aucun
timeout des deux côtés, au moins 4 heartbeats de secours journalisés à la reprise ; (story 3.5)
`--npcs 3` face au faux invité `--hit-interval 500ms` : `ApplyHit` pour 1000 chez l'hôte
(`[test] ApplyHit NetId 1000`, 10 PV simulés en moins, vus par le faux invité dans les
snapshots), puis `--hit-target 1500` : `HitRequest PNJ 1500 ignoré : NetId inconnu`, aucune
commande ; (story 3.6) `--npcs 1 --hit-guest-ms 500` face au faux invité : `HitOnGuest envoyé,
dégâts bruts 18` chez l'hôte, un `<- HitOnGuest source=1000 dégâts=18` par coup chez le faux
invité ; (story 3.7) faux invité `--die-every 1s --duration 4s --quit` : chez l'hôte, à chaque mort,
`Death reçu : invité à terre`, `Respawn envoyé` et `réapparition confirmée par l'invité (tick T)`,
jamais d'`AvatarDeath` ; chez le faux invité `<- Respawn netid=2` et `-> Respawn accusé tick=T`. Le test CTest
`session_fake_guest` est enregistré avec `fake_guest` (si `go` est trouvé) ; il dure environ
90 s. Lancement à la main :

```bash
client/tests/session_fake_guest.sh client/build-tests/session_host tools/fake-guest
```

Avec les deux builds faits dans l'ordre ci‑dessus, `ctest --test-dir client/build-tests` lance
6 tests : `client_tests`, `protocol_vectors`, `fake_guest`, `session_fake_guest`,
`wine_scenarios`, `wine_net`.

## Installation

1. Copier `client/build/Script_Multiplayer.dll` dans `<Steam>/steamapps/common/Risen/bin/scripts/`.
2. Copier `client/multiplayer.example.ini` au même endroit sous le nom `multiplayer.ini`, puis l'adapter.
3. Lancer Risen normalement (Windows ou Proton). **Sous Proton, désactiver l'overlay Steam** pour
   Risen (Propriétés → Général → « Activer l'overlay Steam en jeu ») : constaté le 2026-10-19,
   `gameoverlayui` cesse d'accepter les connexions du jeu, et DXVK reste bloqué dans `connect()`
   sur `/tmp/steam_chrome_overlay_uid…`, ce qui fige le jeu (voir la sentinelle ci-dessous).
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

## Tester une session (`probe = 0`, stories 1.4, 1.5 et 2.2)

Sans sonde, le mod hooke `eCApplication::OnProcess`, ainsi que (story 2.2)
`eCAnimation_PS::PlayMotion` et `SetRagDollEnabled`, qui ne bloquent que les appels du moteur
sur l'avatar. Au premier appel où le monde est
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

### Avatars en jeu (story 2.2, une machine)

À l'entrée en session, chaque pair envoie le `Spawn` de son héros (`NetId` 1 pour l'hôte, 2 pour
l'invité). L'invité est d'abord téléporté à 2 m devant l'hôte, tourné vers lui. À réception,
l'avatar est une copie temporaire du héros **local** (`Entity::Spawn`, même modèle et même
équipement que le héros local) ; le moteur ne l'anime pas, et il peut rester figé (voire en
T-pose) jusqu'à la story 2.4. Toute fin de session le retire : `Entity::Kill` si le monde est
toujours là, sinon il est seulement oublié (détruit avec le monde).

Prérequis : `role = host`, `probe = 0`, overlay Steam désactivé (voir « Installation »). Le faux invité place son avatar comme le ferait un vrai
invité : 2 m devant le héros de l'hôte, tourné vers lui.

1. Charger une partie, puis `tools/fake-guest/fake-guest --host 127.0.0.1 --duration 20s --kill`.
   Attendu : `Spawn envoyé : NetId 1 (hôte)`, `Spawn reçu (NetId 2 (invité), type 0, …) : avatar
   créé`, puis `[sync] avatar 2 créé : entité 0x…, PS 0x…`. **À l'écran** : une copie du héros
   apparaît 2 m devant lui, tournée vers lui.
2. Environ 25 s plus tard (20 s, puis le timeout de 5 s) : `InSession → Ended : timeout`, `avatar
   2 (invité) retiré : fin de session`, `[sync] avatar 2 détruit (Entity::Kill, N appels moteur
   bloqués)`. **À l'écran** : la copie disparaît.
3. Relancer `tools/fake-guest/fake-guest --host 127.0.0.1 --duration 20s` (sortie normale) :
   l'avatar réapparaît, puis disparaît sur `Despawn reçu`. Il n'y a jamais deux copies à la fois.
4. Variante : relancer le faux invité, puis faire un chargement rapide en session. Attendu :
   `avatar 2 oublié : monde détruit`, et aucun crash.
5. Fournir `Script_Multiplayer.log`, et noter l'allure de l'avatar (figé, T-pose, au sol…).

**Sentinelle de gel** (`src/win/watchdog`) : si `OnProcess` ne revient plus pendant 4 s alors qu'un
avatar existe, un thread à part suspend le thread principal, déroule sa pile (`RtlVirtualUnwind`) et
l'écrit au log trois fois, à 1 s d'intervalle (`[watchdog] pile N du thread principal`, une ligne
`module+offset` par frame). Elle ne touche jamais au jeu.

### Déplacements de l'avatar en jeu (story 2.3, une machine)

Chaque pair envoie à 20 Hz (canal 1) la pose de son héros ; l'avatar est replacé à chaque frame
sur la pose interpolée, avec 150 ms de retard (Hermite pour la position, chemin le plus court
pour le lacet, extrapolation de 250 ms au plus si les snapshots s'arrêtent). Il reste en T-pose
jusqu'à la story 2.4 : il glisse sans animation.

Prérequis : `role = host`, `probe = 0`, `log_level = debug`, overlay Steam désactivé.

1. Charger une partie dans un endroit dégagé et plat, puis :
   `tools/fake-guest/fake-guest --host 127.0.0.1 --duration 60s --scenario tools/fake-guest/scenarios/mixte.txt --delay 100ms --jitter 20ms`
2. **À l'écran** : l'avatar apparaît à 2 m devant le héros, puis décrit un cercle de 3 m de rayon,
   s'arrête, fait des allers-retours tout droit (lent, puis rapide) avec des demi-tours secs,
   s'arrête, et fait un petit cercle ; puis tout recommence. Attendu : **aucune téléportation ni
   à-coup** ; l'orientation suit la direction du mouvement.
3. Variante : ajouter `--drop 10` (10 % de pertes) ; le mouvement doit rester fluide.
4. Variante : relancer avec `--duration 15s --kill` (sortie sans rien envoyer) : au milieu d'un
   cercle, les snapshots s'arrêtent ; l'avatar continue 0,25 s sur sa lancée puis s'arrête, et
   disparaît au timeout, 5 s plus tard. (Ctrl-C, lui, envoie aussitôt `Despawn`.)
5. Fournir `Script_Multiplayer.log` : toutes les 5 s, une ligne `[sync] snapshots (5 s) :
   envoyés …, reçus …` (≈ 100 de chaque) donne les frames interpolées, extrapolées et figées, et
   le décalage d'horloge.

### Animations de l'avatar en jeu (story 2.4, une machine)

Chaque `PlayMotion` du héros local part en `Motion` (le nom en `AniName` la première fois) ;
chez le pair, il est rejoué sur l'avatar au même retard que les déplacements (150 ms). La mort
(`Entity::IsDead` du héros) part en `Death`, une fois par mort : l'avatar s'effondre en ragdoll et
ne bouge plus jusqu'à son retrait. Un ragdoll sans mort (chute, renversement) n'envoie rien.

Prérequis : `role = host`, `probe = 0`, `log_level = debug`, overlay Steam désactivé.

1. **Écho** : `tools/fake-guest/fake-guest --host 127.0.0.1 --duration 120s --echo`. Le faux
   invité renvoie tes déplacements et tes gestes comme les siens : l'avatar, 2 m devant toi au
   départ, **t'imite avec un léger retard** (150 ms d'affichage, plus l'aller-retour vers le faux
   invité ; il ne renvoie pas la mort, puisque celle de l'hôte termine la session). Marcher, courir, sauter, dégainer (l'arme
   suit depuis la story 3.2), frapper à vide, parer. Attendu : mêmes gestes, calés sur le déplacement, plus
   de T-pose.
2. **Scénario** : `tools/fake-guest/fake-guest --host 127.0.0.1 --duration 60s --scenario
   tools/fake-guest/scenarios/gestes.txt`. L'avatar court sur un cercle de 3 m, saute à 2 s,
   s'arrête, puis frappe deux fois ; puis tout recommence.
3. **Mort** : `tools/fake-guest/fake-guest --host 127.0.0.1 --duration 20s --scenario
   tools/fake-guest/scenarios/mort.txt` : à 2 s (plus 150 ms), l'avatar s'effondre en ragdoll et
   ne bouge plus (`[sync] avatar 2 en ragdoll (mort)` au log).
4. Fournir `Script_Multiplayer.log` : le bilan `[sync] snapshots (5 s)` se termine par
   `Motion : envoyés …, rejoués …, écrasés …, ignorés …, jetés … ; MotionParam : envoyés …, rejoués …,
   ignorés …` ; une ligne `« … » non rejoué` signale un nom que `GetMotion` ne trouve pas.

**Locomotion (story 2.4b).** Risen démarre une animation de déplacement avec un poids presque nul,
puis en règle le poids et la vitesse de lecture à chaque frame (`SetTargetWeight`, `SetPlaySpeed`
de l'enveloppe EMotionFX), et l'arrête par un fondu (`SwitchFadeOut`, `StopMotion`). Ces réglages
du héros partent en `MotionParam` (les changements seulement) et sont rejoués sur l'avatar ; ceux que le contrôleur de l'avatar lancerait de lui-même
sont bloqués (il le croit immobile, car la frame le recale). Test : en écho, démarrer, courir en
avant puis en arrière, s'arrêter, marcher, tourner : ni T-pose au démarrage, ni blocage. La ligne
`[sync] locomotion (1 s)` compte les réglages du héros et ceux bloqués sur l'avatar.

### Avatar traversable et registre des entités (story 3.1)

L'avatar reste placé par `Entity::MoveTo` à chaque frame, comme en J1, et il reste traversable.
Les réglages d'avatar solide du spike J2 (groupe 4, collision, DCC, corps cinématique
`AddBodyFlag(0x81)`, gravité coupée) décrochaient son modèle animé : seule l'arme suivait la pose
réseau, que le placement soit `SetPose` + `PSRigidBody::SetPose` ou `MoveTo` (tests en jeu du
2026-10-20). Les monstres de l'hôte ciblent et frappent quand même l'avatar.

Story 3.1 : l'`AnimFilter` devient le registre des entités d'AD-12 (entité, PS d'animation, PS de
routine, enveloppe, rôle). Les verdicts des détours sont inchangés ; seuls les tests Linux et le
banc Wine le vérifient. Rien de nouveau à tester en jeu.

### Arme de l'avatar en jeu (story 3.2, une machine)

L'arme que le héros tient en main droite (nom de l'objet, ex. `It_1H_Sword`) part en `Weapon` à
chaque changement, et une fois à l'entrée en session, juste après le `Spawn` ; une arme rangée
part avec un nom vide. Chez le pair, au même retard que les gestes (150 ms), l'avatar crée cette
arme dans son inventaire si elle manque (une seule fois par nom), puis la tient
(`HoldRightStack`) ; un nom vide la range (`EquipStack`). Pas de mode de combat transmis : la
posture est dans le nom des animations rejouées. L'équipement de l'invité : story 3.2b
ci-dessous ; l'armure portée par l'hôte : story 3.2c.

Prérequis : `role = host`, `probe = 0`, overlay Steam désactivé ; `log_level = debug` pour voir
aussi les lectures de l'arme du héros (`[sync] arme du héros : …`). Au démarrage, le log doit
contenir `[hooks] inventaire de session résolu (arme de l'avatar)` ; sinon la cause est nommée
(`symbole d'inventaire introuvable : …`) et la session reste inactive.

1. **Avatar armé dès l'apparition** : `tools/fake-guest/fake-guest --host 127.0.0.1 --duration 60s
   --weapon It_1H_Sword`. Attendu : l'avatar apparaît l'épée en main. Log :
   `[sync] Weapon reçu : avatar 2 (invité), « It_1H_Sword » …`, puis
   `[sync] arme : avatar 2 tient « It_1H_Sword » (pile N créée, HoldRightStack → true)`.
2. **Arme de l'hôte** (même commande) : sortir son arme, puis la ranger. Attendu côté faux
   invité : `<- Weapon netid=1 nom="It_…"` puis `<- Weapon netid=1 nom=""` ; côté hôte,
   `[sync] Weapon envoyé : NetId 1 (hôte), « It_… »` puis `…, arme rangée`.
3. **Attaques arme en main** : `tools/fake-guest/fake-guest --host 127.0.0.1 --duration 120s --echo`.
   Le faux invité renvoie aussi ton arme : sortir l'arme, frapper, parer, ranger, changer d'arme.
   Attendu : l'avatar sort et range l'arme avec toi (dans la seconde) et ses attaques se font
   arme en main ; une arme déjà vue est reprise (`pile N trouvée`), jamais recréée.
4. **Nom lu = nom accepté** : avec l'écho (étape 3), sortir une vraie arme (épée, hache, bâton…).
   Vérifier que le faux invité journalise `<- Weapon netid=1 nom="…"` avec ce nom, que l'hôte
   l'applique ensuite à l'avatar (`[sync] arme : avatar 2 tient « … »`) et que le log de l'hôte ne
   contient **aucune** ligne `template « … » inconnu du jeu` pour ce nom : le nom lu par
   `Entity::GetName` sur l'objet en main doit être accepté par `Template(const char*)`. Répéter avec
   2 ou 3 armes différentes.
5. **Nom inconnu** : `--weapon It_Inexistant`. Attendu : une seule ligne
   `[sync] arme : avatar 2 : template « It_Inexistant » inconnu du jeu, avatar inchangé`.
6. Rapporter les lignes `[sync] arme …`, `[sync] Weapon …` et le côté faux invité (`<- Weapon`).

### Équipement de l'invité en jeu (story 3.2b)

`guest_equipment` (config de l'invité) est une liste de templates (`It_Armor_Don_Fighter,
It_1H_Sword_Rusty`), validée au chargement (une valeur mal formée nomme le champ
`guest_equipment` au log et le mod reste inactif). Elle part dans le `Hello`. L'invité équipe son
héros à chaque entrée en session, après la téléportation ; l'hôte équipe l'avatar de l'invité
juste après sa création. Pour chaque nom : objet créé dans l'inventaire **seulement s'il manque**,
puis équipé (`EquipStack`) s'il ne l'est pas déjà. Un nom inconnu du jeu est journalisé et les
suivants s'appliquent.

Prérequis : `probe = 0`, overlay Steam désactivé. Au démarrage, la ligne
`[hooks] inventaire de session résolu (arme de l'avatar)` (inchangée) couvre aussi
`IsStackEquiped`.

1. **Avatar équipé chez l'hôte** (une machine, Risen en hôte) :
   `tools/fake-guest/fake-guest --host 127.0.0.1 --duration 60s --equipment It_Armor_Don_Fighter`.
   Attendu : l'avatar apparaît en armure, **sans** la tunique ni le bâton par défaut du héros
   (validé le 2026-10-21). Log : `[sync] équipement de l'invité : It_Armor_Don_Fighter`,
   `[sync] EquipAvatar : avatar 2 (invité), It_Armor_Don_Fighter`, en debug
   `[sync] avatar 2 : inventaire prêt après 15 frames` (équipement et arme différés), puis
   `[sync] équipement : avatar 2 : « It_Armor_Don_Fighter » équipé (pile N créée, EquipStack → true)`
   et le bilan `1 objet(s) équipé(s), 0 en échec, sur 1`.
2. **Arme commune** : `--equipment It_1H_Sword --weapon It_1H_Sword`. Attendu : l'arme tenue
   reprend la pile de l'équipement (`pile N trouvée`), pas de seconde copie.
3. **Nom inconnu** : `--equipment It_Inexistant,It_Armor_Don_Fighter`. Attendu :
   `template « It_Inexistant » inconnu du jeu, ignoré`, puis l'armure équipée quand même.
4. **Héros invité** (deux machines, ou Risen en invité face à un hôte) : `role = guest`,
   `guest_equipment` sur un objet absent de l'inventaire. Attendu à l'entrée en session :
   `[sync] équipement du héros : …` puis `[sync] équipement : héros : « … » équipé (pile N créée, …)` ;
   l'objet est dans l'inventaire et porté. Quitter, revenir en session : `pile N trouvée` (ou
   `déjà équipé`), aucune copie ajoutée.
5. Rapporter les lignes `[sync] équipement …` et `[sync] EquipAvatar …`.

### Armure portée en jeu (story 3.2c, une machine)

Chaque joueur lit à chaque frame l'armure de son héros (pile équipée à l'emplacement 8,
`FindStackIndex(gEEquipSlot)`, nom de son objet par `GetTemplateItem`) et l'envoie en `Armor`
(protocole v8) à chaque changement, et une fois à l'entrée en session, après le `Spawn` et le
`Weapon` ; sans armure, le nom est vide. Chez le pair, au même instant d'affichage qu'un `Weapon`,
l'avatar équipe cette armure (pile trouvée ou créée, partagée avec l'arme et l'équipement :
une par nom) ou, sur un nom vide, retire celle de l'emplacement 8 (`UnEquipStack`) : la tunique
par défaut doit revenir. Un `Armor` reçu dans les 15 premières frames de l'avatar est différé
(le dernier reçu), comme l'arme. `Hello.equipment` reste appliqué à la création de l'avatar.

Prérequis : `role = host`, `probe = 0`, overlay Steam désactivé, `log_level = debug` pour voir
les lectures (`[sync] armure du héros : …`) et le diagnostic d'inventaire. La ligne
`[hooks] inventaire de session résolu (arme de l'avatar)` (inchangée) couvre aussi
`FindStackIndex(gEEquipSlot)`, `GetTemplateItem` et `UnEquipStack`.

1. **Armure de l'hôte** : `tools/fake-guest/fake-guest --host 127.0.0.1 --duration 120s --echo`.
   Changer d'armure, puis la retirer. Attendu côté faux invité :
   `<- Armor netid=1 nom="It_Armor_…"` à chaque changement, puis `<- Armor netid=1 nom=""` ; côté
   hôte, `[sync] Armor envoyé : NetId 1 (hôte), « It_Armor_… »` puis `…, sans armure`. Avec l'écho,
   l'avatar change d'armure avec toi (`[sync] armure : avatar 2 porte « … » (pile N …,
   EquipStack → true)`), puis la retire (`[sync] armure : avatar 2 retire son armure (pile N,
   UnEquipStack → true)`).
2. **Avatar en armure dès l'apparition** :
   `tools/fake-guest/fake-guest --host 127.0.0.1 --duration 60s --armor It_Armor_Inquisitor`.
   Attendu : l'avatar apparaît en armure, **sans** la tunique. Log :
   `[sync] Armor reçu : avatar 2 (invité), « It_Armor_Inquisitor » …`, en debug
   `[sync] armure : avatar 2 : Armor « It_Armor_Inquisitor » différé`, puis après
   `inventaire prêt après 15 frames`, `[sync] armure : avatar 2 porte « It_Armor_Inquisitor » …`.
3. **Retour de la tunique** : un scénario avec `armor 2 -` (par exemple `pause 4` puis
   `armor 2 -` et `armor 3 It_Armor_Inquisitor`) et `--armor It_Armor_Inquisitor`. Attendu :
   l'armure disparaît et la **tunique par défaut revient** (point à vérifier : si elle ne revient
   pas, le noter, voir le plan B de `docs/re/equipement.md`), puis l'armure revient.
4. **A remplacée par B** (`log_level = debug`) : avec l'écho, passer d'une armure A à une
   armure B. Attendu : la ligne de diagnostic qui suit
   (`[sync] diagnostic : avatar 2 en armure : inventaire, …`) ne montre que B « équipée »
   à l'emplacement 8 ; A y figure encore, mais sans « équipée ».
5. **Nom lu = nom accepté** : avec l'écho, vérifier que le log de l'hôte ne contient **aucune**
   ligne `template « … » inconnu du jeu` pour les armures portées.
6. Rapporter les lignes `[sync] armure …`, `[sync] Armor …` et le côté faux invité (`<- Armor`).

### Répliques des PNJ en jeu (story 3.3)

L'hôte fait autorité sur les PNJ (protocole v9). À chaque snapshot, il lit `Entity::ms_arrNPCs`
(après `UpdateEntityLists`), garde les PNJ vivants à 50 m au plus de son héros ou de l'avatar de
l'invité, du plus proche au plus loin (53 au plus), leur attribue un NetId ≥ 1000 lié à leur GUID
pour la session, annonce chaque entrée par `Spawn{1, pose, GUID}` et chaque sortie par `Despawn`,
puis met leur pose et leurs PV dans le snapshot. L'invité retrouve chaque PNJ natif par
`gCRuntimeInfo::GetEntity(GUID)`, l'inscrit au registre comme réplique (son
`gCScriptRoutine_PS::OnProcess` n'est plus appelé : IA figée), le place par `MoveTo` à la pose
interpolée (150 ms de retard) et écrit ses PV (`SetHitPoints(pv, 2)`) quand ils changent. À la
sortie du rayon, à la fin de session et à la fin de monde, la réplique retrouve son IA (une
réplique morte reste morte, story 3.4b) ; elle n'est jamais détruite. Les animations des répliques : story 3.4 (ci-dessous) ; leur mort :
story 3.4b (ci-dessous : depuis le protocole v11, un PNJ mort reçoit `Death` et non `Despawn`).

Prérequis : `probe = 0`, overlay Steam désactivé, la même sauvegarde des deux côtés (les GUID des
PNJ placés sont stables d'une machine et d'un chargement à l'autre, spike J2). Le log doit
contenir `[hooks] PNJ de session résolus (répliques)` et `hooks de session installés (…,
gCScriptRoutine_PS::OnProcess)`.

1. **Risen hôte + faux invité (une machine)** :
   `tools/fake-guest/fake-guest --host 127.0.0.1 --duration 120s`, puis s'approcher d'un camp de
   monstres et s'en éloigner. Attendu côté hôte : `[sync] PNJ 1000 annoncé (Spawn) : GUID …, 40.0 m,
   PV …` à l'entrée dans le rayon, `[sync] PNJ 1000 retiré (Despawn) : hors du rayon` (ou
   `évincé par plus proche`, `absent de la liste depuis 2 s` : la liste du moteur ne garde que les PNJ à 40 m environ) à la sortie, et le même NetId au retour. Côté
   faux invité : `<- Spawn PNJ netid=1000 guid=…`, `PNJ netid=1000 premier snapshot … PV=…`,
   `PNJ netid=1000 PV a -> b` quand on frappe le monstre, `<- Despawn PNJ netid=1000`, la ligne
   par seconde `PNJ suivis=… poses=… inattendues=…`, et à la fin `Bilan PNJ : … au plus N entités
   par snapshot` (N ≤ 54) suivi d'une ligne par NetId.
2. **Deux machines, même sauvegarde (validation finale)** : l'hôte approche un camp de monstres.
   Attendu chez l'invité : `[sync] PNJ 1000 : Spawn reçu (GUID …) : réplique`, puis
   `[sync] PNJ 1000 répliqué : « … », entité 0x…, GUID …, IA figée oui`. Les monstres suivent les
   positions de l'hôte, sans IA locale (ils ne poursuivent pas le héros de l'invité), et leurs PV
   baissent quand l'hôte les frappe. Un GUID inconnu chez l'invité donne une seule ligne
   `[sync] PNJ … : GUID … introuvable chez l'invité : ignoré`. À la fin de session :
   `[sync] PNJ : N réplique(s) libérée(s) : fin de session`, et les monstres reprennent leur IA.
3. En `log_level = debug`, le bilan des snapshots (toutes les 5 s) porte
   `PNJ : suivis …, annoncés …, retirés …, plafond …, liste illisible …, entités inconnues …,
   OnProcess de routine bloqués …`. Chez l'invité, ce dernier compteur doit croître tant que des
   répliques sont suivies (IA figée), et retomber à 0 après la fin de session.
4. Rapporter les lignes `[sync] PNJ …` des deux côtés et le bilan du faux invité.

### Animations des PNJ en jeu (story 3.4, à valider en jeu)

**À valider en jeu** : testé au banc (Linux, faux invité, Wine) seulement. Protocole v10. À chaque sélection, l'hôte anime ses **8** PNJ répliqués les plus proches d'un des
deux joueurs : leur PS d'animation et leur enveloppe EMotionFX entrent au registre (rôle de capture,
avec leur NetId), et leurs `PlayMotion` et réglages de locomotion partent comme ceux du héros
(`AniName`, `Motion`, `MotionParam` sous le NetId du PNJ, même espace d'`ani_id`). Les réglages d'un
PNJ sont regroupés : au plus un par frame et par (type, nature). Budget visé : 32 Ko/s pour
l'ensemble des PNJ. Chez l'invité, toute réplique est inscrite avec son PS d'animation et son
enveloppe : son moteur ne l'anime plus. Les animations de l'hôte y sont rejouées 150 ms plus tard,
avec les règles de l'avatar, PNJ par PNJ ; une réplique hors des 8 reste figée dans sa dernière
animation, et une réplique libérée est de nouveau animée par son moteur.

Limite connue : une réplique tuée localement par le héros de l'invité (possible jusqu'à la
story 3.5, qui transmettra ses coups à l'hôte) reste debout, car ses animations et son ragdoll
natifs sont bloqués.

Prérequis : ceux de la story 3.3, et **`log_level = debug`** pour le bilan.

1. **Risen hôte + faux invité (une machine)** :
   `tools/fake-guest/fake-guest --host 127.0.0.1 --duration 120s`, puis s'approcher d'un camp de
   monstres, les laisser marcher et attaquer (les frapper). Attendu côté faux invité :
   `<- Motion PNJ netid=1000 premier geste ani=… nom=…` pour chaque PNJ animé, aucune ligne
   `: ignoré (NetId non annoncé ou retiré)`, la ligne par seconde `… animés=N` avec N ≤ 8 (hors
   relève : un PNJ qui entre dans le groupe pendant qu'un autre en sort), et à la fin
   `Bilan animations PNJ : Motion=… MotionParam=… ignorés=0, au plus K PNJ animés en 1 s`, puis
   `Motion=… MotionParam=…` sur la ligne de chaque NetId. Côté hôte, le bilan
   `[sync] snapshots (5 s)` porte `animations des PNJ : animés N, Motion envoyés …,
   rejoués 0, écrasés 0, ignorés …, file pleine 0, MotionParam envoyés …, regroupés …, rejoués 0,
   ignorés …, X o/s` (suivi, depuis la story 3.4a, de `PNJ reçus avant leur Spawn …`) : N ≤ 8,
   X sous 32768. Un dépassement donne la ligne
   `[sync] animations des PNJ : … o/s envoyés, au-delà du budget de 32768 o/s` (une par bilan).
2. **Deux machines (validation finale)** : l'hôte combat un monstre. Chez l'invité, la réplique
   marche et attaque comme chez l'hôte, sans animation propre ; le bilan de l'invité porte
   `rejoués …` non nuls et `ignorés 0`. À la fin de session, les monstres reprennent leur IA et
   leurs animations.
3. Rapporter le bilan des deux côtés (dernières lignes `[sync] snapshots (5 s)`) et celui du faux
   invité.

### Robustesse de session (story 3.4a, à valider en jeu)

**À valider en jeu** : testé au banc (Linux, faux invité) seulement. Protocole inchangé (v10, puis
v11 avec la story 3.4b).
- **Pause du jeu** : quand la frame (`OnProcess`) ne lit plus le transport depuis 1,5 s
  (cinématique, chargement, menu bloquant), le thread réseau envoie lui-même un `Heartbeat` par
  seconde, 60 s au plus (au-delà, il se tait et le pair conclut au timeout 5 s plus tard). À la
  reprise, la frame journalise `[sync] jeu en pause : N heartbeat(s) envoyé(s) par le thread
  réseau`, et `[sync] jeu en pause plus de 60 s : …` si la borne a été atteinte.
- **PNJ en avance** (invité) : une entrée de snapshot d'un PNJ dont le `Spawn` n'est pas encore
  arrivé est gardée 500 ms (la plus récente par NetId, 64 NetId au plus), puis appliquée à
  l'arrivée du `Spawn`. Le bilan `[sync] snapshots (5 s)` porte le compteur `PNJ reçus avant
  leur Spawn N`, que la story 3.4b fait suivre des morts de PNJ ; une entrée expirée compte dans « entités inconnues » et « entités ignorées ».
- **Fin de session** : une seule ligne `PNJ N : plus animé` par PNJ animé ; `sync/npc` écrit
  `PNJ N : retiré du registre des captures`.
- **Fin de monde** : dès que la frame voit le monde changer (non chargé, ou autre héros), les
  répliques et les PNJ capturés sont oubliés sans appel au jeu, avant leur revérification par
  `GetEntity` (`[sync] PNJ : N réplique(s) oubliée(s) : fin de monde`, `… PNJ animé(s)
  retiré(s) : fin de monde`).

Prérequis : recompiler, copier `client/build/Script_Multiplayer.dll` dans `bin/scripts/`,
**`log_level = debug`**, overlay Steam désactivé. Log : `bin/scripts/Script_Multiplayer.log`.

1. **Une machine, Risen hôte + faux invité** :
   `tools/fake-guest/fake-guest --host 127.0.0.1 --strict-timeout --duration 120s --echo`.
   - ouvrir le menu Échap, puis faire un chargement rapide (moins de 60 s). Le menu Échap peut ne
     pas figer `OnProcess` : l'absence de la ligne « jeu en pause » est alors normale ; elle
     n'apparaît que pour une pause de plus de 1,5 s (chargement) ;
   - attendu : pas de `timeout` chez le faux invité pendant les pauses du jeu ;
   - au chargement, l'hôte journalise `[sync] PNJ : N réplique(s)/PNJ animé(s) … : fin de monde`,
     puis `InSession → Ended : fin de monde`, sans aucun `réplique perdue` ni `PNJ animé perdu` ;
   - si la frame a été suspendue plus de 1,5 s : `[sync] jeu en pause : N heartbeat(s) envoyé(s)
     par le thread réseau`.
2. **Deux machines, cinématique** : l'invité déclenche une cinématique scriptée ou un long dialogue
   avec transition (par exemple la capture par l'Inquisition). Attendu chez l'invité :
   `[sync] jeu en pause : N heartbeat(s)…` à la reprise ; chez l'hôte, pas de
   `timeout (5 s sans nouvelles du pair)`, et la session continue (snapshots reçus au bilan
   suivant).
3. **Deux machines, PNJ à l'arrivée** : s'approcher d'un camp. Attendu chez l'invité : dans
   `[sync] snapshots (5 s)`, « entités inconnues » proche de 0 et le dernier compteur
   `PNJ reçus avant leur Spawn` > 0 ; les PNJ bougent dès leur apparition.
4. **Fin de session de l'hôte** (l'invité quitte) : une seule ligne `PNJ N : plus animé` par PNJ
   animé, plus `PNJ N : retiré du registre des captures`.
5. **Deux machines, rechargement de l'hôte** : l'invité reçoit `SessionEnd{2}` ; l'hôte oublie
   ses PNJ « fin de monde » et ne plante pas.

Limite connue : pendant un chargement, la frame n'est pas appelée ; le registre peut encore
contenir les PS des répliques détruites jusqu'à la première frame du nouveau monde (une entité
du nouveau monde placée à la même adresse aurait ses `PlayMotion` ou son `OnProcess` de routine
bloqués jusque-là).

### Mort des répliques de PNJ en jeu (story 3.4b, à valider en jeu)

**À valider en jeu** : testé au banc (Linux, faux invité) seulement. Protocole **v11**.
- **Hôte** : un PNJ annoncé qui meurt (`Entity::IsDead`) reçoit `Death{NetId}` au lieu de
  `Despawn` (`[sync] PNJ N mort (Death)`), avant le snapshot du même tick, une fois ; il sort du
  groupe des 8 animés au même tick. Ce NetId n'est plus jamais annoncé de la session, même si le
  PNJ revit ou revient dans le rayon. Limite : un PNJ qui meurt hors du rayon, ou pendant son délai
  de grâce « absent de la liste », a déjà reçu `Despawn` ; sa copie chez l'invité garde son IA.
- **Invité** : `[sync] PNJ N : Death reçu : mort à l'instant d'affichage`, puis, quand le temps
  affiché atteint le dernier snapshot reçu de la réplique, une mort « visuelle » **sans `Kill`
  natif** (ni XP, ni script, ni quête) : `[sync] PNJ N mort : PV 0, AIMode 12, ragdoll` (une étape
  en échec donne une ligne Warn ou Error, et la réplique est marquée morte quand même). Elle n'est
  ensuite plus déplacée, animée ni ses PV écrits ; elle reste au registre (IA, animations et ragdoll
  natifs bloqués) jusqu'à la fin de session, où elle est libérée et **reste morte**
  (`[sync] PNJ N libéré : reste morte`, ou l'oubli « fin de monde »). Le butin reste propre à
  chaque joueur : chacun pille son propre PNJ natif.
- **PNJ vivant à 0 PV** (humain à terre chez l'hôte) : la réplique vivante reçoit au plus bas
  1 PV (`[sync] PNJ N : PV = 1` en debug) ; seule la mort met 0 PV.
- Bilan `[sync] snapshots (5 s)` : en fin de ligne, `morts de PNJ : envoyées …, appliquées …,
  ignorées …`.

Prérequis : recompiler, copier `client/build/Script_Multiplayer.dll` dans `bin/scripts/`,
**`log_level = debug`**, overlay Steam désactivé ; reconstruire le faux invité
(`cd tools/fake-guest && go build`). Log : `bin/scripts/Script_Multiplayer.log`.

1. **Une machine, Risen hôte + faux invité**
   (`tools/fake-guest/fake-guest --host 127.0.0.1 --duration 180s --echo`), près d'un camp de
   monstres. Tuer un monstre avec le héros de l'hôte. Attendu chez l'hôte : `[sync] PNJ N mort
   (Death)`, sans `retiré (Despawn) : mort` ; `PNJ N : plus animé` au même tick s'il était animé ;
   au bilan `morts de PNJ : envoyées 1, …`. Chez le faux invité : `<- Death PNJ netid=N (poses
   reçues=…, PV=…)`, puis plus aucune pose de N. S'éloigner à plus de 50 m, puis revenir : aucun
   nouveau `annoncé (Spawn)` pour ce GUID.
2. **Une machine, humain à terre** : mettre à terre un humain (mains nues ou arme contondante,
   sans le tuer). Attendu : pas de `PNJ N mort (Death)` chez l'hôte, pas de `<- Death PNJ` chez le
   faux invité pour ce NetId.
3. **Deux machines, mort d'un monstre** : l'hôte tue un monstre proche de l'invité. Attendu chez
   l'invité : `[sync] PNJ N : Death reçu`, puis `[sync] PNJ N mort : PV 0, AIMode 12, ragdoll`, une
   seule fois ; la réplique tombe en ragdoll et reste au sol ; aucun message d'XP ni de quête ; au
   bilan `morts de PNJ : … appliquées 1`.
4. **Deux machines, butin** : l'invité fouille le cadavre de sa réplique : il trouve l'inventaire
   natif de son propre PNJ ; l'hôte fouille le sien de son côté.
5. **Deux machines, fin de session** (l'invité quitte, ou l'hôte recharge). Attendu chez
   l'invité : `PNJ N libéré : reste morte` (ou l'oubli « fin de monde ») ; le cadavre ne se relève
   pas, ne joue pas de son de mort et ne donne pas d'XP. Si la routine native relève le PNJ, le
   noter pour la story 3.5 (hypothèse D4 de la spec invalidée).
6. **Deux machines, humain à terre** : la réplique d'un humain à terre chez l'hôte garde au moins
   1 PV chez l'invité (`[sync] PNJ N : PV = 1` en debug), sans mort locale.

### Dégâts de l'invité sur les monstres (story 3.5, à valider en jeu)

**À valider en jeu** : testé au banc (Linux, faux invité ; banc Wine : détour posé sur la vraie
Game.dll) seulement. Protocole **v12**.
- **Point d'accroche** (Q8) : un détour sur `gCDamage_PS::DoDamage(eCEntity*)` (Game.dll), posé
  avec les hooks de session : `[hooks] point d'accroche des dégâts posé (gCDamage_PS::DoDamage)`.
  Clé `damage_hook` (défaut `1`) : à `0`, pas de détour
  (`[hooks] point d'accroche des dégâts désactivé (damage_hook = 0)`), mais l'hôte applique
  toujours les `HitRequest` reçus. **`damage_hook = 0` est un interrupteur de diagnostic** (crash
  au chargement), à éviter chez l'invité : sans détour, les coups natifs de son héros sur une
  réplique passent, ses PV baissent localement et elle peut mourir nativement (`Kill`, XP,
  scripts), ce que la 3.4b interdit. Symboles introuvables : `[hooks] dégâts inactifs : …`, la
  session continue sans dégâts synchronisés.
- **Invité** : un coup natif de son héros sur une réplique vivante est bloqué
  (`[sync] coup natif bloqué : héros -> PNJ N (HitRequest)`) et part en `HitRequest`
  (`[sync] coup sur PNJ N : HitRequest envoyé`) ; la jauge locale ne baisse pas d'elle-même, elle
  suit les snapshots de l'hôte. Tout autre coup natif sur une réplique est bloqué sans rien émettre.
- **Hôte** : un `HitRequest` d'un PNJ annoncé et vivant s'applique par
  `PSDamage::DoDamage(arme en main de l'avatar, PNJ, avatar)` :
  `[sync] HitRequest PNJ N appliqué : DoDamage -> 0|1, PV a -> b`. Refus (Warn) :
  `HitRequest PNJ N ignoré : NetId inconnu | PNJ mort | NetId < 1000 | PNJ introuvable | pas
  d'avatar | avatar sans arme en main`. Tout coup natif de l'avatar est bloqué
  (`coup natif bloqué : avatar invité -> … (seuls les HitRequest blessent)`). Les PV d'un PNJ ne
  sont jamais écrits directement.
- Diagnostic : `[sync] dégâts : 1er passage au point d'accroche (thread T, thread OnProcess T0)`
  une fois par session (T doit égaler T0) ; en debug, chaque passage qui implique le registre ou
  le héros : `[sync] dégâts : agresseur P (rôle) cible Q (rôle) -> bloqué|laissé`. Bilan
  `[sync] snapshots (5 s)` : en fin de ligne, `coups : envoyés …, appliqués …, ignorés …,
  bloqués …`. « appliqués » compte les `ApplyHit` émis par la session de l'hôte, avant que la DLL
  ne les accepte ou ne les refuse (`PNJ introuvable`, `pas d'avatar`, `avatar sans arme en main`) :
  seules les lignes `HitRequest PNJ N appliqué : DoDamage -> 1` prouvent un dégât.

Prérequis : recompiler, copier `client/build/Script_Multiplayer.dll` dans `bin/scripts/`,
**`log_level = debug`**, `damage_hook = 1`, overlay Steam désactivé ; reconstruire le faux invité
(`cd tools/fake-guest && go build`). Log : `bin/scripts/Script_Multiplayer.log`.

1. **Installation (hôte)** : `[hooks] point d'accroche des dégâts posé (gCDamage_PS::DoDamage)`.
   `dégâts inactifs` ou crash au chargement : remettre `damage_hook = 0` et rapporter la ligne.
2. **Une machine, Risen hôte + faux invité**
   (`fake-guest --host 127.0.0.1 --duration 180s --echo --weapon It_1H_Sword --hit-interval 1s`),
   près d'un camp de monstres. Attendu chez l'hôte : `HitRequest PNJ N appliqué : DoDamage -> 1,
   PV a -> b` avec b < a, jauge du monstre qui baisse, monstre hostile envers l'avatar ; jusqu'à
   la mort : `PNJ N mort (Death)`, puis `HitRequest PNJ N ignoré : PNJ mort`. Chez le faux invité :
   `PNJ netid=N PV a -> b`. `1er passage au point d'accroche` : T = T0.
3. **Une machine, NetId inconnu** (`--hit-target 1500 --hit-interval 1s`) :
   `HitRequest PNJ 1500 ignoré : NetId inconnu` (Warn), aucun crash.
4. **Une machine, coups du héros de l'hôte** : dégâts normaux, aucune ligne `coup natif bloqué`.
5. **Une machine, avatar qui attaque** (`--echo`, le héros de l'hôte attaque près d'un monstre) :
   une ligne `coup natif bloqué : avatar invité -> …` montre que l'avatar déclenche des coups
   natifs, bloqués ; son absence n'est pas une erreur (hypothèse D5 de la spec), la noter.
6. **Deux machines** : l'invité frappe une réplique. Chez l'invité : `coup natif bloqué : héros ->
   PNJ N (HitRequest)` et `coup sur PNJ N : HitRequest envoyé` ; chez l'hôte : `HitRequest PNJ N
   appliqué`. Tuer le monstre à deux : il meurt chez les deux (`Death`, 3.4b). Bilan : `coups :
   envoyés X, … bloqués Y` côté invité, `appliqués X` côté hôte ; X compte les `ApplyHit` émis,
   donc vérifier aussi autant de lignes `HitRequest PNJ N appliqué : DoDamage -> 1` et aucun Warn
   `HitRequest PNJ N ignoré`.
7. **Deux machines, arme rangée** : frapper à mains nues ; chez l'hôte : `HitRequest PNJ N ignoré :
   avatar sans arme en main`.
8. **Facultatif, deux machines, arc ou sort** : l'invité tire sur une réplique. Hypothèse non
   vérifiée : pour un projectile ou un sort, l'agresseur lu au proxy `+0x30` pourrait être le
   projectile et non le héros ; le coup serait alors bloqué sans `HitRequest`
   (`coup natif bloqué : … -> PNJ N (autre agresseur, sans HitRequest)`), dégâts à distance perdus
   en silence (hors périmètre de l'epic, mêlée seulement). Noter la ligne observée.
9. Si l'agresseur n'est jamais reconnu (aucune ligne `bloqué` alors que l'invité frappe),
   rapporter les lignes debug `dégâts : agresseur … cible …` : l'hypothèse « proxy `+0x30` =
   personnage » est alors fausse (repli : `DoMeleeDamage`, Script_Game).

### Dégâts subis par l'invité (story 3.6, à valider en jeu)

**À valider en jeu** : testé au banc (Linux, faux invité ; banc Wine : symboles des getters sur la
vraie Game.dll) seulement. Protocole **v13** (`HitOnGuest`, type 23). Même détour que la 3.5
(`damage_hook = 1`), aucun nouveau hook.
- **Hôte** : un coup natif sur l'avatar de l'invité est **mesuré** (correctif du 2026-10-23 : les
  dégâts bruts de l'arme valaient 0 pour les monstres) :
  - d'un monstre : l'avatar est remis à ses PV max, le coup s'applique (scripts du jeu, armure de
    l'avatar comprise ; mise à terre ou mort annulée), puis
    `[sync] coup sur l'avatar invité mesuré (source PNJ N) : DoDamage -> 1, dégâts D (HitOnGuest), PV de l'avatar rendus à P`
    et `[sync] coup sur l'avatar invité (source PNJ N) : HitOnGuest envoyé, dégâts bruts D` (D = dégâts
    mesurés). Un agresseur hors registre (PNJ non animé, projectile) donne `source inconnue` ;
  - du héros de l'hôte : `coup natif bloqué : … -> avatar invité (pas de tir ami)`, rien d'émis ;
  - d'un agresseur nul : `… (agresseur nul, sans HitOnGuest)`, rien d'émis.

  PV de l'avatar illisibles : Warn une fois par session `PV de l'avatar illisibles : coup sur
  l'avatar bloqué sans HitOnGuest`.
- **Invité** : tout coup natif sur son héros est bloqué, quel que soit l'agresseur
  (`coup natif bloqué : PNJ N -> héros (seuls les HitOnGuest blessent)`). Un `HitOnGuest` reçu est
  appliqué par `sync/guest_health`, seul écrivain des PV du héros (`SetHitPoints` fixé) :
  `[sync] HitOnGuest (source PNJ N) appliqué : dégâts bruts D, PV a -> b`. Un coup mortel laisse le
  héros à 1 PV et le met à terre (story 3.7, section suivante). Pas de recul chez l'invité.
- **Effet d'impact** (correctif du 2026-10-23) : un coup natif bloqué rend `true` sans appeler
  l'original, donc le jeu joue l'effet d'impact (sang, son) sans dégâts. Exceptions qui rendent
  `false` (pas d'effet) : tir ami entre l'hôte et l'avatar, agresseur nul.
- Bilan `[sync] snapshots (5 s)` : en fin de ligne, `coups subis : HitOnGuest envoyés …, reçus …,
  ignorés …` (hôte : envoyés ; invité : reçus ; un `HitOnGuest` reçu par l'hôte est ignoré, Warn).

Prérequis : comme la 3.5 (`log_level = debug`, `damage_hook = 1`, overlay Steam désactivé, faux
invité reconstruit). Si `dégâts inactifs` nomme un symbole, rapporter la ligne.

1. **Une machine, hôte + faux invité** (`fake-guest --host 127.0.0.1 --duration 180s --echo`), avatar
   près d'un camp hostile. Attendu chez l'hôte : `coup natif bloqué : PNJ N -> avatar invité
   (HitOnGuest, dégâts bruts D)` puis `HitOnGuest envoyé` ; l'avatar ne meurt jamais et ne tombe
   pas. Chez le faux invité : `<- HitOnGuest source=N dégâts=D`, une ligne par coup. Noter D. Une
   source `inconnue` : rapporter la ligne debug `dégâts : agresseur …`.
2. **Comparaison** : se faire frapper par le même type de monstre avec le héros de l'hôte et noter
   la perte de PV native ; la comparer à D (hypothèse H2 de la spec).
3. **Tir ami** : frapper l'avatar avec le héros de l'hôte : `(pas de tir ami)`, aucun `HitOnGuest`.
4. **Monstre sur le héros de l'hôte** : dégâts natifs normaux, aucune ligne `bloqué`.
5. **Deux machines** : une réplique attaque le héros invité. Chez l'invité : éventuellement
   `coup natif bloqué : PNJ N -> héros (seuls les HitOnGuest blessent)` (son absence tranche H4),
   et à chaque coup `HitOnGuest (source PNJ N) appliqué : …, PV a -> b`, jauge qui baisse (H6).
   Bilans : `HitOnGuest envoyés X` côté hôte, `reçus X` côté invité.
6. **Deux machines, coup mortel** : voir la story 3.7 ci-dessous (mise à terre puis réapparition).

### À terre et réapparition de l'invité (story 3.7, à valider en jeu)

**À valider en jeu** : testé au banc (Linux, faux invité `--die-every` ; banc Wine : symboles
`DownOrDeadBy*` et `GetHitPointsMax`) seulement. Protocole **v14** (`Respawn`, type 25 ; `Death{2}` =
« à terre »).
- **Invité, mise à terre** : un `HitOnGuest` mortel (`sync/guest_health`, héros laissé à 1 PV) ou un
  coup natif mortel intercepté dans `DownOrDeadBy*` (Warn `[sync] coup natif mortel intercepté
  (DownOrDeadBy…) : héros à 1 PV, à terre`) donne `[sync] héros à terre (<cause>) : Death envoyé,
  attente du Respawn`. `Death{2}` part une seule fois : les mises à terre suivantes sont ignorées
  (Debug) et les `HitOnGuest` reçus à terre aussi (`ignoré (héros à terre)`).
- **Hôte** : `[sync] Death reçu : invité à terre, Respawn à envoyer`, puis au premier tick où la pose
  de son héros est lisible `[sync] Respawn envoyé : invité replacé (x, y, z) lacet=…` (2 m devant son
  héros, tourné vers lui) ; l'avatar y saute aussitôt, **sans ragdoll**. Ses snapshots sont ignorés
  (« snapshots périmés ») jusqu'à `[sync] réapparition confirmée par l'invité (tick T)`, puis ceux de
  tick < T.
- **Invité, réapparition** : `[sync] Respawn reçu : héros replacé (…), PV pleins ; accusé envoyé
  (tick T)`, puis `[sync] héros local replacé (réapparition) : …` et `[sync] réapparition : PV du
  héros remis à N ; placé en …`.
- **Replis** : invité sans `Respawn` depuis 10 s → Warn `pas de Respawn de l'hôte depuis 10 s : PV
  rendus sur place` ; hôte sans accusé depuis 5 s → Warn `pas d'accusé de Respawn depuis 5 s :
  snapshots de l'invité repris`.
- **Mort native (limite)** : chute ou eau profonde ne passent pas par les détours. Warn `[sync] mort
  native du héros (chute, eau profonde ?) : Death envoyé ; réapparition impossible, la session finira
  au rechargement` ; le `Respawn` est refusé (`pas de relève d'un mort`), l'écran de mort natif
  s'affiche et la session finit au rechargement de l'invité (fin de monde) ; l'hôte se réarme.
- **Installation** : `[hooks] détours DownOrDeadBy* de session posés (mise à terre de l'invité)`, ou
  Warn `[hooks] DownOrDeadBy* inactifs : … (coup natif mortel non intercepté si le point d'accroche
  des dégâts manque)` : la session continue (sur le banc Wine, `Script_Game.dll` est absente).
- Bilan `[sync] snapshots (5 s)` : en fin de ligne, `à terre : Death …, Respawn envoyés …, reçus …,
  accusés …, ignorés …, snapshots périmés …` (hôte : Death reçus, Respawn envoyés, accusés reçus ;
  invité : Death envoyés, Respawn reçus, accusés envoyés).

Prérequis : comme la 3.6, plus `cd tools/fake-guest && go build`.

1. **Installation** : la ligne des détours `DownOrDeadBy*` de session ; un Warn `inactifs` est à
   rapporter.
2. **Une machine, hôte + faux invité qui meurt** : `fake-guest --host 127.0.0.1 --duration 120s
   --echo --die-every 10s`. Chez l'hôte, une fois par mort : `Death reçu : invité à terre`, `Respawn
   envoyé`, `réapparition confirmée (tick T)`. L'avatar ne passe jamais en ragdoll, saute 2 m devant
   le héros de l'hôte, puis reprend l'écho sans revenir à l'ancienne position. Chez le faux invité :
   `<- Respawn netid=2 pose=…` et `-> Respawn accusé tick=T`.
3. **Mort de l'hôte** : fin de session raison 3, comme avant.
4. **Deux machines, monstres** : l'invité tombe à 1 PV, `Death` part une fois, il réapparaît devant
   l'hôte jauge pleine, sans menu de chargement ni ragdoll ; la session continue (H1, H2).
5. **Deux machines, filet** : `damage_hook = 0` chez l'invité, se faire tuer : `coup natif mortel
   intercepté (DownOrDeadBy…)`, puis la réapparition (H4).
6. **Deux machines, chute ou eau profonde** : Warn `mort native du héros …`, `Respawn` refusé, fin de
   session au rechargement de l'invité, aucun crash (H5).

### Overlay à l'écran (rôle et état de session)

Une ligne permanente en haut à gauche de l'image dit quelle fenêtre est l'hôte et laquelle est
l'invité : `HÔTE · <pseudo> · <état>` (or) ou `INVITÉ · <pseudo> · <état>` (cyan), avec une ombre
noire. États : `hors partie` (Inactive), `en attente d’un invité` (Hosting),
`connexion à <ip>:<port>…` (Connecting), `en session` (InSession), `session terminée` (Ended).
- Dessinée par un détour de `IDirect3DDevice9::Present` (vtable lue sur un device factice créé
  à l'installation, `d3d9.dll` résolue dynamiquement) avec `ID3DXFont` (`D3DXCreateFontW` de
  `d3dx9_39.dll`, déjà chargée par le jeu, interface déclarée à la main) ; `Reset` est détourné
  pour relâcher puis recréer les ressources de la police. Aucun appel au jeu depuis ces détours.
- Clé `overlay` (défaut `1`) : à `0`, aucun détour (`[overlay] désactivé (overlay = 0)`).
  Jamais posé en mode sonde (`probe = 1`).
- Facultatif : un échec (`d3dx9_39.dll` absente, device factice impossible, MinHook) donne
  `[overlay] inactif : …` et la session continue sans overlay.
- Log attendu : `[overlay] détours posés (IDirect3DDevice9::Present, Reset)` au chargement,
  puis `[overlay] premier dessin, thread N` au premier `Present`.

**À valider en jeu** (deux Risen sur la même machine, en fenêtré) : chaque fenêtre affiche son
rôle et son état ; l'invité passe à `en session` quand l'hôte l'accepte ; le texte reste affiché
après un changement de résolution. `grep '\[overlay\]'` dans les deux logs.

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
- **mort (story 2.1b)** : les appels à `eCAnimation_PS::SetRagDollEnabled` du joueur sont
  capturés (ligne `SetRagDollEnabled actif=… vecteur=…`) et rejoués sur la copie en mode
  miroir. Tant que la copie est en ragdoll, sa pose n'est plus imposée (`ragdoll=oui` dans la
  ligne `copie`) ;
- **F4** : rejoue, par le même chemin, le dernier coup capturé sur le PNJ le plus proche
  (15 m au plus).

Protocole de test (story 2.1) : charger une partie, F6 face à un espace dégagé, F7, puis
marcher, courir, sauter, frapper (plusieurs coups), et se laisser mourir, en regardant la
copie. Attendu : la copie reproduit les gestes comme le miroir v5 du spike J1
(`docs/re/spike-j1.md`) ; les lignes `copie` montrent `0 échecs` et `0 écarts de pointeur`.
Quitter, et fournir `Script_Multiplayer.log`.

Test de la mort (story 2.1b) : F6, F7 (**avant** la mort : un ragdoll survenu miroir coupé
n'est pas rattrapé), puis mourir d'une chute ; recharger, F6, F7, puis se laisser tuer au
combat. Attendu : la copie s'effondre en ragdoll en même temps que le héros (une frame de
retard, invisible), et le log montre `SetRagDollEnabled actif=1 … vecteur=…` puis `rejoué sur
la copie : OK`. À relever dans le log : le vecteur (comparer à la position du héros dans la
ligne `tick` : position du monde ou impulsion ?) et tout `PlayMotion` au nom de mort juste
avant ou après (piste `NeedsDeathAnimation`). Une copie morte ne repart pas : recharger et
refaire F6.

Avec `probe = 0` ou sans la clé, la sonde est désactivée et seule la
session est installée (hook `OnProcess` seul). Sonde et session sont exclusives.

## Organisation

```
cmake/clang-cl-xwin.cmake   toolchain croisée Windows x64
src/core/                   logique pure (config, version, codec du protocole, machine à états de session, texte de l'overlay), testée sous Linux
src/common/                 log
src/net/                    transport UDP + KCP : socket, canaux, thread réseau (testé sous Linux et Wine)
src/game/                   adaptateur ABI du jeu (GetProcAddress) — seul code qui appelle le jeu
src/sync/                   pilote de session (tick par frame) ; filtre PlayMotion (AnimFilter) ; sonde du spike J1
src/win/                    ScriptInit, hooks MinHook, overlay à l'écran (détour de Present)
third_party/minhook/        MinHook, figé sur un commit (voir VERSION.txt)
third_party/kcp/            KCP 2.1.1, figé sur un commit (voir VERSION.txt), compilé dans mp_net
tests/                      tests unitaires (dont vecteurs de ../protocol/vectors/) (mini-framework maison, sans dépendance) + wine/
```

Le contrat binaire hôte ⇄ invité est décrit dans [`../protocol/PROTOCOL.md`](../protocol/PROTOCOL.md)
(règles de session au §8) ;
son codec est `src/core/protocol.{h,cpp}` (`mp::protocol`), vérifié contre les vecteurs
`../protocol/vectors/*.bin` (`protocol/vectors/regen.sh --check`).

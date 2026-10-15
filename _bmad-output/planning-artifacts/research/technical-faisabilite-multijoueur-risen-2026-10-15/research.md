---
title: 'Recherche technique : faisabilité d’un mod multijoueur coop pour Risen 1'
type: 'technical'
topic: 'Faisabilité d’un mod multijoueur coop (2–8 joueurs) pour Risen 1 (Genome, Steam x64, D3D9)'
decision: 'Go / no-go sur l’approche technique du MVP (injection DLL + MinHook, appels d’exports MSVC, KCP + relais Go) et briques à ajuster avant l’architecture'
source: 'native-run'
status: complete
preset: 'standard'
validation: 'normal'
created: '2026-10-15'
updated: '2026-10-15'
claims_verified: 8
claims_unverified: 5
---

# Recherche technique : faisabilité d’un mod multijoueur coop pour Risen 1

**Décision servie :** Go / no-go sur l’approche technique du MVP (injection DLL + MinHook, appels d’exports MSVC, KCP + relais Go) et briques à ajuster avant l’architecture.

## Résumé exécutif

**Verdict : GO, avec trois ajustements avant l’architecture.**

1. **Pas besoin d’injecteur : le chargeur de plugins natif fonctionne encore en 64 bits.** Risen charge les DLL `bin/scripts/Script_*.dll` et appelle leur export `ScriptInit` [7][8]. Le build Steam 64 bits contient toujours `bin/scripts/Script_Game.dll` (x64, exporte `ScriptInit`), et `Game.dll` contient les chaînes `*.dll` et `ScriptInit` [9]. Le client peut donc probablement être livré comme un `Script_Multiplayer.dll` déposé dans ce dossier, sans `WINEDLLOVERRIDES` ni proxy. Ce n’est pas encore testé avec une DLL tierce sous Proton.
2. **MinGW g++ n’est pas utilisable tel quel : passer à clang-cl + lld-link + xwin.** Côté MSVC, une méthode membre non statique qui retourne un objet le fait via un pointeur caché placé *après* `this` [25][28]. GCC/mingw-w64 se trompe sur ce cas, et aucun correctif publié n’a été trouvé [26][27]. Or le jeu exporte plus de 1 300 méthodes de ce type, dont `Entity::GetPose → bCMatrix`, au cœur du MVP [9]. clang-cl suit l’ABI MSVC [28] et dispose d’une chaîne de cross-compilation Linux officielle [30][31].
3. **KCP convient, à condition d’ajouter un démultiplexage.** KCP ne fournit qu’un canal fiable : pas de session ni de canal non fiable [34]. kcp-go est maintenu (v5.6.72, 2026-03) et interopère avec le `ikcp.c` en C tant que FEC et chiffrement sont désactivés [35][36]. Il faut donc un octet de canal en tête de chaque datagramme, un heartbeat applicatif et un serveur Go qui lit l’UDP brut.

**Plus gros risque :** il n’existe **aucun précédent** sur le moteur Genome [9][D1]. Les mods comparables (Skyrim Together, KenshiCoop) mettent des années à se stabiliser et sont publiés avec des avertissements du type « quêtes cassées, crashs » [14][16][18]. Le MVP « deux joueurs se voient bouger » est le bon périmètre. La synchro des quêtes et des PNJ est la longue traîne.

## D1 — Mods multijoueurs existants sur des jeux solo

- **Aucun multijoueur Risen ou Genome connu.** Rien sur le web, Nexus ou GitHub en dehors du dépôt du projet. Confiance moyenne, puisqu’il s’agit d’une absence de preuve [D1].
- **Le cousin ZenGin.** Gothic Multiplayer (GMP, Gothic 2) est sorti en 2011 avec une équipe de 3 personnes. Il est open source et relancé depuis 2022, avec des commits en janvier 2026, dont un « Player state manipulation via Server-Side lua ». Le README indique encore une version de dev « work-in-progress » [12]. Gothic 2 Online (2015) est une plateforme scriptée en Squirrel côté serveur et côté client, orientée RP/MMO plutôt que coop narrative [13]. Confiance moyenne : les pages ModDB étaient bloquées.
- **Le modèle dominant en coop est l’hôte autoritaire.**
  - *Skyrim Together Reborn* : chef de groupe, 2 à 8 joueurs. Seul le leader parle aux PNJ de quête, et changer de leader « cassera probablement les quêtes actives » [16][17]. L’architecture comprend un client injecté, un serveur dédié et des définitions de messages partagées [14]. Le développement a pris plus de 5 ans, avec une réécriture complète [15]. La synchro d’animation (variables du graphe d’animation, pas les clips) est encore corrigée en octobre 2026 [14].
  - *KenshiCoop* (2 joueurs, projet amateur) : l’hôte fait autorité sur le monde et chaque client sur son escouade. Transport ENet/UDP, mutations exécutées sur le thread principal du jeu, une sauvegarde partagée, et un contrôle de version de protocole entre les deux clients. L’auteur prévient : « desyncs, and crashes » [18].
- **Pièges récurrents :** l’état des quêtes et scripts (contourné en réservant ces actions à l’hôte), la synchro d’animation, les crashs, les sauvegardes et les versions de build différentes entre joueurs [14][16][18].

## D2 — Moteur Genome et communauté de reverse

- **Le passage en 64 bits** a été annoncé le 2023-01-24, en même temps que les portages console. Il a ajouté le support manette, un réglage du FOV et les succès Steam, et les sauvegardes restent compatibles. Le build 32 bits reste disponible via la branche bêta Steam `original` [1][2]. Le nom de la branche n’est confirmé que par le post du développeur.
- **La mise à jour 64 bits a cassé les mods natifs 32 bits**, c’est-à-dire les DLL de script [3][4][5][6]. Les outils, offsets et SDK d’avant 2023 ne servent plus que de référence de reverse.
- **Le chargeur `bin/scripts/Script_*.dll` + `ScriptInit` est vérifié localement en x64** [9]. La convention de dossier est confirmée par des pages de mods indépendantes [8]. Le RisenSDK communautaire est en 32 bits, Patch 1.10 (licence CC BY-NC-SA) [7]. Ses en-têtes restent utiles pour retrouver la disposition de `gCSession` et `eCEntity`, avec prudence puisque les tailles de pointeurs ont changé.
- **Outillage existant :** Baltram/rmtools et RisenEditor ne couvrent que les formats de fichiers [11]. Il n’existe aucun script extender et aucun code public qui référence `gCSession` ou `eCEntity` [D2D3]. Un chargeur tiers `d3d9.dll` (Desgeras, Nexus) se déclare compatible 64 bits [10]. Non vérifié, mais c’est un plan B possible.

## D3 — Injection et hooks sous Proton/Wine

- **Proxy DLL** (Ultimate ASI Loader : `dinput8`, `version`, `winmm`…) [19]. Sous Proton, il faut normalement `WINEDLLOVERRIDES="<dll>=n,b" %command%`, et cela échoue dans certains jeux [20]. Wine 11.6 (2026-04) préfère la DLL du dossier du jeu quand son éditeur n’est pas Microsoft. Proton ne l’avait pas encore intégré lors de l’annonce [21]. **Le chargeur `Script_*.dll` rend tout ce sujet inutile** : c’est le jeu lui-même qui charge la DLL [9].
- **MinHook** est disponible pour mingw-w64 via MSYS2 (1.3.3/1.3.4) [22]. Aucun problème connu sous Wine x64, mais c’est une absence de preuve (confiance moyenne).
- **DRM :** Tages a été retiré, et GOG vend le jeu sans DRM [42]. Je n’ai pas pu vérifier la présence de SteamStub sur l’exe 64 bits.
- **Hook D3D9 sous DXVK : non résolu.** Les points de hook classiques sont `EndScene`, `Present` et `Reset` sur la vtable du device [24]. DXVK 3.1 (2026-08) a amélioré la compatibilité avec les overlays, mais seulement pour DXGI [23]. Je n’ai trouvé aucun rapport primaire dans un sens ou dans l’autre pour D3D9 (ImGui, MinHook). ReShade fonctionne sous Proton via des scripts communautaires [41]. Ce sujet ne bloque pas le MVP, puisque la synchro n’exige pas d’overlay, mais il est à prototyper tôt.

## D4 — Toolchain et ABI MSVC depuis Linux

- **Règle MSVC x64.** Un type utilisateur n’est retourné dans RAX que par une fonction globale ou une méthode statique [25]. Une méthode d’instance passe un pointeur de retour caché, toujours *après* `this` (RCX=this, RDX=sret) [28]. Les tests de Clang le confirment [28].
- **GCC/mingw-w64 est incompatible.** Le bug PR 64384 se reproduit encore en GCC 10.1 (commentaire de 2020-06) [26]. Wine documente que g++ place le pointeur de retour *avant* `this` et contourne le problème avec des paramètres de retour explicites [27][29]. Je n’ai trouvé aucun correctif publié. Le statut actuel n’est **pas vérifié** (Bugzilla inaccessible), confiance moyenne.
- **Impact concret :** 1 363 exports concernés dans Engine, Game, Script et SharedBase, dont `Entity::GetPose`, `eCRigidBody_PS::GetPose` et `gCCombatMoveMelee::CalcMoveDir` [9]. Avec MinGW, chacun devrait être déclaré à la main en `Ret* fn(This*, Ret* out, …)`. C’est faisable, mais une erreur corrompt la pile en silence.
- **clang-cl + lld-link + xwin :**
  - LLVM fournit un fichier de toolchain CMake officiel (`WinMsvc.cmake`) [30].
  - xwin est maintenu (0.10.0, août, l’année n’est pas affichée) [31]. Il exige d’accepter la licence Microsoft, et seule la variante `desktop` est bien testée [31].
  - Un retour d’expérience de 2024 juge les flags délicats mais l’ensemble fonctionnel [32].
  - Alternative pour le SDK : msvc-wine, sans Wine, qui demande CMake 3.23+ et Clang 13+ [33].
  - Je n’ai trouvé aucun projet de modding sous Linux qui documente cette chaîne.
- **llvm-mingw** (clang avec la cible GNU) suit très probablement GCC sur ce point, puisque la règle relève de l’ABI C++ MS. Croyance non vérifiée.

## D5 — Transport réseau

- **KCP** n’est qu’un algorithme ARQ : pas de connexion, pas de SYN/FIN, pas de canal non fiable, et un `conv` identique des deux côtés. Mode rapide : `ikcp_nodelay(1,10,2,1)` [34]. Confirmé par une source indépendante [36].
- **kcp-go :**
  - v5.6.72 du 2026-03-27, maintenu [35].
  - `Dial`/`Listen` sans chiffrement ni FEC produisent des segments KCP bruts, compatibles avec ikcp. Le chiffrement ou le FEC ajoutent des en-têtes qui cassent l’interopérabilité [36].
  - Le Listener lit le `conv` dans les 4 premiers octets de tout datagramme, donc un paquet non fiable sur le même socket serait pris pour du KCP [36].
  - L’interopérabilité en mode brut n’a pas été testée de façon indépendante : il faut la tester.
- **Alternatives :**

| Option | Fiable + non fiable | Client C | Serveur Go | Remarque |
|---|---|---|---|---|
| KCP + démux maison | oui, via un préfixe | `ikcp.c` (2 fichiers) | kcp-go, mature | Solution la plus légère. Session et heartbeat à écrire soi-même [34][36] |
| GameNetworkingSockets | oui, natif | API C | bindings tiers `nielsAD/gns` | NAT/ICE inclus. Plus lourd [37] |
| ENet | oui | oui | cgo (`go-enet`) ou `genet`, expérimental (2026-05) | Le serveur Go perdrait la compilation croisée sans cgo [38] |
| netcode + reliable | non fiable seulement + lib séparée | oui | port Go | Tokens et chiffrement, prévu pour du serveur dédié [39] |

- **Interpolation :** afficher les entités distantes environ 3 intervalles d’envoi en retard, plus une marge de gigue (environ 150 ms à 30 Hz). Ne jamais attendre une retransmission. Courbes de Hermite pour la position, slerp pour la rotation [40].

## Enseignements croisés

- **L’atout « exports » est réel mais à double tranchant.** Les exports évitent les bibliothèques d’adresses dont Skyrim et Kenshi ont besoin [D1][9], mais ils imposent l’ABI MSVC exacte, ce qui fait basculer le choix de toolchain (D2 × D4).
- **Le chargeur natif change toute la chaîne de chargement (D2 × D3).** Pas de proxy DLL, pas d’override Wine, et une seule DLL à déposer pour l’utilisateur final. MinHook reste utile pour intercepter des fonctions internes, mais pas pour se faire charger.
- **Le modèle « premier client = hôte qui simule les PNJ »** correspond au schéma dominant en coop (Skyrim Together, KenshiCoop) [16][18]. Ces projets montrent aussi qu’il faut, dès le départ, un **contrôle de version de protocole et de build du jeu** (32 et 64 bits coexistent [1]).

## Recommandations

| # | Recommandation | Confiance | Alimente |
|---|---|---|---|
| R1 | Charger le client comme `bin/scripts/Script_<nom>.dll` exportant `ScriptInit` (plan A). Plan B : proxy DLL ou chargeur Desgeras [10][19] | Moyenne-haute : mécanisme vérifié localement, pas encore testé avec une DLL tierce | Architecture : chargement du client |
| R2 | **Spike n°1** (½ journée) : une DLL vide `Script_Probe.dll` qui écrit un log depuis `ScriptInit` sous Proton. Retrouver au passage la signature de `ScriptInit` dans `Script_Game.dll` | — | Première story de l’epic 1 |
| R3 | Toolchain **clang-cl + lld-link + xwin** (CMake `WinMsvc.cmake`), en remplacement de MinGW dans le CLAUDE.md et l’architecture | Haute pour la règle MSVC, moyenne pour le statut GCC | Architecture : toolchain |
| R4 | Dans tous les cas, déclarer les méthodes membres qui retournent un objet sous la forme `Ret* (*)(This*, Ret* out, …)`, et ne jamais utiliser de pointeur de membre C++ | Haute | Conventions de code du client |
| R5 | Garder KCP + kcp-go **sans FEC ni chiffrement**, avec un octet de canal (0 = KCP fiable, 1 = état non fiable), un heartbeat, un serveur Go en lecture UDP brute et un test d’interopérabilité C↔Go en CI | Moyenne-haute | `protocol/PROTOCOL.md`, architecture réseau |
| R6 | Cibler uniquement le build 64 bits. Ajouter un handshake avec version de protocole et hash de build | Haute | PRD (exigences non fonctionnelles), protocole |
| R7 | Reporter tout overlay ou UI D3D9 après le MVP. Tant que ce n’est pas prouvé sous DXVK, utiliser un log fichier et la console | Faible (manque de données) | Périmètre du MVP |
| R8 | Avant d’écrire le code de rendu des joueurs distants, lire le code de GMP (cousin ZenGin) et de TiltedEvolution | Moyenne | Architecture : rendu des joueurs distants |

## Questions ouvertes

- **La signature de `ScriptInit` et l’acceptation d’une DLL tierce sous Proton.** Pour trancher : désassembler `Script_Game.dll` et `Game.dll`, puis faire le spike R2.
- **Le statut actuel des bugs GCC PR 64384 et 52792.** Pour trancher : les ouvrir dans un navigateur, ou compiler une méthode qui retourne une struct avec mingw g++ et avec clang-cl, puis comparer le désassemblage. Aucun compilateur croisé n’est installé sur la machine.
- **Le hook `EndScene`/`Present` sous DXVK d3d9.** Pour trancher : un prototype ImGui DX9 + MinHook, ou une recherche dans les tickets DXVK.
- **SteamStub sur l’exe 64 bits.** Pour trancher : SteamDB (app 40300) ou un test de chargement.
- **Les threads RisenSDK sur World of Players** (886883, 1061573), bloqués par une protection anti-bot. Pour trancher : les lire à la main afin d’obtenir les en-têtes 32 bits.
- **L’interopérabilité kcp-go ↔ ikcp en mode brut.** Pour trancher : un test de bout en bout.

## Annexe des sources

| [n] | Affirmation soutenue | Éditeur | Date pub. | Consulté | Confiance |
|---|---|---|---|---|---|
| [1] | MAJ 64 bits du 2023-01-24, branche `original` | [Steam Community (post dév.)](https://steamcommunity.com/app/40300/discussions/0/3758851410778653161) | 2023-01-24 | 2026-10-15 | haute |
| [2] | MAJ PC en même temps que les portages console | [GameBanshee](https://www.gamebanshee.com/news/127250-risen-console-port-and-pc-update-available.html) | 2023-01 | 2026-10-15 | haute |
| [3] | Mods DLL cassés (« Could not load script module ») | [World of Players](https://forum.worldofplayers.de/forum/threads/1620417) | 2023 | 2026-10-15 | moyenne |
| [4] | EPU incompatible avec le 64 bits | [Nexus Mods](https://nexusmods.com/risen/mods/37) | inconnue | 2026-10-15 | moyenne |
| [5] | FPS Patch incompatible avec la version 2023 | [GitHub VaanaCZ](https://github.com/VaanaCZ/Risen-FPS-Patch) | inconnue | 2026-10-15 | haute |
| [6] | DLL du patch non officiel incompatibles | [PCGamingWiki forum](https://community.pcgamingwiki.com/topic/5669-risen-on-pc/) | 2023 | 2026-10-15 | moyenne |
| [7] | RisenSDK, `Script_*.dll` + `ScriptInit` | [World of Players](https://forum.worldofplayers.de/forum/threads/886883-release-RisenSDK) | ~2010 | 2026-10-15 | moyenne |
| [8] | Convention `bin/scripts/Script_*.dll` | [VGTimes.ru](https://vgtimes.ru/games/risen/files/mods-skins/top-files/) | inconnue | 2026-10-15 | moyenne |
| [9] | Inspection locale : Script_Game.dll x64 + ScriptInit, chaînes de Game.dll, 1 363 exports retournant un objet par valeur | Observation primaire (objdump/strings, `risen_ori/bin`, voir `digests/LOCAL-r1-0.md`) | 2026-10-15 | 2026-10-15 | haute |
| [10] | Chargeur d3d9.dll (Desgeras) annoncé compatible 64 bits | [Nexus Mods (Risen, mod 58)](https://www.nexusmods.com/risen/mods/58) | inconnue | 2026-10-15 | faible |
| [11] | rmtools = formats de fichiers uniquement | [GitHub Baltram](https://github.com/Baltram/rmtools) | inconnue | 2026-10-15 | haute |
| [12] | GMP open source, relancé, actif en 2026-01 | [GitHub skejt23](https://github.com/skejt23/GothicMultiplayer) | 2026-01-05 | 2026-10-15 | haute |
| [13] | G2O : scripts Squirrel, orienté RP/MMO | [GRY-Online](https://www.gry-online.pl/newsroom/gothic-2-online-historia-kolonii-to-nowy-raj-dla-fanow-gorniczej/z5227d6) | inconnue | 2026-10-15 | moyenne |
| [14] | Architecture de TiltedEvolution, synchro d’animation encore corrigée | [GitHub tiltedphoques](https://github.com/tiltedphoques/TiltedEvolution) | 2026-10-14 | 2026-10-15 | haute |
| [15] | Plus de 5 ans de développement, réécriture | [GamesBeat](https://gamesbeat.com/skyrim-together-reborn-is-finally-out-and-playable/) | 2022-07 | 2026-10-15 | moyenne |
| [16] | Modèle chef de groupe, quêtes réservées au leader | [Wabbanode](https://wabbanode.com/help/skyrim-together/understanding-parties-in-skyrim-together-reborn) | 2025-12 | 2026-10-15 | haute |
| [17] | 2–8 joueurs, bugs annoncés | [mp1st](https://mp1st.com/news/skyrim-multiplayer-co-op-mod-together-reborn-finally-released) | 2022-07 | 2026-10-15 | haute |
| [18] | KenshiCoop : hôte autoritaire, ENet, sauvegarde partagée | [GitHub nhoral](https://github.com/nhoral/KenshiCoop/wiki) | 2026 | 2026-10-15 | haute |
| [19] | Proxy DLL Ultimate ASI Loader | [GitHub ThirteenAG](https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases/tag/v9.7.1) | inconnue | 2026-10-15 | moyenne |
| [20] | Échecs de `WINEDLLOVERRIDES` | [Steam Community](https://steamcommunity.com/app/12210/discussions/0/7599331480041103654) | inconnue | 2026-10-15 | moyenne |
| [21] | Heuristique d’ordre de chargement dans Wine 11.6 | [GamingOnLinux](https://www.gamingonlinux.com/2026/04/wine-11-6-is-an-exciting-release-to-make-modding-windows-games-on-linux-simpler/) | 2026-04-04 | 2026-10-15 | haute |
| [22] | MinHook packagé pour mingw-w64 | [MSYS2](https://packages.msys2.org/packages/mingw-w64-x86_64-MinHook) | inconnue | 2026-10-15 | moyenne |
| [23] | DXVK 3.1 : overlays côté DXGI | [GamingOnLinux](https://www.gamingonlinux.com/2026/08/direct3d-8-9-10-11-to-vulkan-translation-layer-dxvk-3-1-brings-improvements-for-game-launchers-older-frostbite-engine-and-more/) | 2026-08 | 2026-10-15 | moyenne |
| [24] | Points de hook D3D9 | [GameDev.net](https://gamedev.net/forums/topic/359794-c-direct3d-hooking-sample/) | ~2005 | 2026-10-15 | moyenne |
| [25] | Règle de retour d’objet en x64 MSVC | [Microsoft Learn](https://learn.microsoft.com/en-us/cpp/build/x64-calling-convention) | 2026-05-21 | 2026-10-15 | haute |
| [26] | PR 64384 toujours présent en GCC 10.1 | [GCC gcc-bugs](https://gcc.gnu.org/pipermail/gcc-bugs/2020-June/705246.html) | 2020-06 | 2026-10-15 | moyenne |
| [27] | g++ place sret avant `this`, contournement dans widl | [WineHQ wine-cvs](https://www.winehq.org/pipermail/wine-cvs/2017-July/122239.html) | 2017-07 | 2026-10-15 | moyenne |
| [28] | MSVC : sret toujours après `this`, jamais en registre | [LLVM](https://llvm.googlesource.com/clang/+/master/test/CodeGenCXX/thiscall-struct-return.cpp) | 2014 | 2026-10-15 | haute |
| [29] | Contournement par paramètre de sortie explicite | [Josh Staiger](https://joshstaiger.org/notes/C-Language-Problems-in-Direct3D-12-GetCPUDescriptorHandleForHeapStart.html) | ~2020 | 2026-10-15 | moyenne |
| [30] | Toolchain CMake `WinMsvc.cmake` | [LLVM](https://llvm.googlesource.com/llvm-project/llvm/+/refs/heads/main/cmake/platforms/WinMsvc.cmake) | main | 2026-10-15 | haute |
| [31] | xwin 0.10.0, licence, variante desktop | [GitHub Jake-Shadle](https://github.com/Jake-Shadle/xwin) | 08 (année ?) | 2026-10-15 | moyenne |
| [32] | Retour d’expérience clang-cl + xwin | [Evan Martin](https://neugierig.org/software/blog/2024/04/cross-compile-2.html) | 2024-04 | 2026-10-15 | moyenne |
| [33] | msvc-wine sans Wine | [GitHub mstorsjo](https://github.com/mstorsjo/msvc-wine) | inconnue | 2026-10-15 | moyenne |
| [34] | KCP = ARQ pur, `conv`, mode rapide | [GitHub skywind3000](https://github.com/skywind3000/kcp) | inconnue | 2026-10-15 | haute |
| [35] | kcp-go v5.6.72 | [pkg.go.dev](https://pkg.go.dev/github.com/xtaci/kcp-go/v5?tab=versions) | 2026-03-27 | 2026-10-15 | haute |
| [36] | Valeurs par défaut de kcp-go, interopérabilité, Listener, pas de SYN/FIN | [GitHub xtaci (sess.go)](https://raw.githubusercontent.com/xtaci/kcp-go/master/sess.go) | 2026 | 2026-10-15 | haute |
| [37] | GameNetworkingSockets | [GitHub Valve](https://github.com/ValveSoftware/GameNetworkingSockets) | inconnue | 2026-10-15 | moyenne |
| [38] | ENet en Go (go-enet, genet) | [pkg.go.dev](https://pkg.go.dev/github.com/SeanTolstoyevski/genet) | 2026-05-18 | 2026-10-15 | moyenne |
| [39] | netcode | [GitHub mas-bandwidth](https://github.com/mas-bandwidth/netcode) | inconnue | 2026-10-15 | moyenne |
| [40] | Snapshot interpolation | [Gaffer On Games](https://gafferongames.com/post/snapshot_interpolation/) | 2014-11-30 | 2026-10-15 | haute |
| [41] | ReShade sous Proton | [GitHub kevinlekiller](https://github.com/kevinlekiller/reshade-steam-proton) | 2024 | 2026-10-15 | faible |
| [42] | Tages retiré, GOG sans DRM | [DSOGaming](https://www.dsogaming.com/news/deep-silver-partners-with-gog-com-will-be-releasing-drm-free-titles-first-is-risen/) | ~2014 | 2026-10-15 | moyenne |

Les références `[D1]` et `[D2D3]` renvoient aux digests bruts dans `digests/`.

## Carte de péremption

Calculée par `recon_kit.py staleness` avec les fenêtres suivantes : version/compat 1 mois, écosystème 6 mois, pattern et échec 24 mois. Les échéances sont mesurées depuis la date de publication de la source.

| Affirmation | Classe | Pub. | À revérifier | Périmée |
|---|---|---|---|---|
| [26] GCC incompatible avec MSVC pour les retours d’agrégats (PR 64384) | version-compat | 2020-06 | 2020-07-01 | **oui** : à trancher par un test |
| [1] MAJ 64 bits, branche `original` | version-compat | 2023-01 | 2023-02-01 | oui (fait historique ; la branche reste à revérifier) |
| [3] Mods 32 bits cassés par la MAJ | échec | 2023-01 | 2025-01-01 | oui (historique) |
| [35] kcp-go v5.6.72 | version-compat | 2026-03 | 2026-04-01 | oui |
| [21] Wine 11.6, pas encore dans Proton | version-compat | 2026-04 | 2026-05-01 | **oui** : vérifier Proton actuel |
| [25] Règle sret de MSVC | version-compat | 2026-05 | 2026-06-01 | oui (règle d’ABI stable) |
| [12] GMP actif | écosystème | 2026-01 | 2026-07-01 | oui |
| [31] xwin 0.10.0 | version-compat | 2026-08 | 2026-09-01 | oui |
| [23] DXVK 3.1 et hooks D3D9 | version-compat | 2026-08 | 2026-09-01 | oui |
| [9] Chargeur `Script_*.dll` en x64, exports par valeur | version-compat | 2026-10 | 2026-11-01 | non (à revérifier à chaque MAJ du jeu) |
| [36] Interopérabilité kcp-go ↔ ikcp | version-compat | 2026-10 | 2026-11-01 | non |
| [34] KCP sans session | pattern | 2026-10 | 2028-10-01 | non |
| [16] Modèle chef de groupe (Skyrim Together) | pattern | 2025-12 | 2027-12-01 | non |

Échéance la plus ancienne : 2020-07-01 ([26]). Les deux revérifications qui comptent vraiment pour la décision sont [26] (toolchain) et [21] (Proton). Celle de [9] est à refaire après chaque mise à jour de Risen sur Steam.

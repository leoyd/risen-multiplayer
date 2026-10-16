---
status: final
stepsCompleted: [1, 2, 3, 4]
inputDocuments:
  - '_bmad-output/planning-artifacts/prds/prd-risen-multiplayer-2026-10-15/prd.md'
  - '_bmad-output/planning-artifacts/prds/prd-risen-multiplayer-2026-10-15/addendum.md'
  - '_bmad-output/planning-artifacts/architecture/architecture-risen-multiplayer-2026-10-15/ARCHITECTURE-SPINE.md'
  - 'docs/re/spike-j1.md'
scope: 'J1 détaillé (FR-4 à FR-8) ; J0 fait ; J2 esquissé'
---

# risen-multiplayer - Epic Breakdown

## Overview

Ce document découpe en epics et en stories les exigences du PRD et les décisions de la spine d’architecture (mise à jour après le spike J1). Le périmètre détaillé est **J1 — Session et présence**. J0 est livré (chargement, version, config, puis sonde du spike J1). J2 n’est qu’esquissé.

## Requirements Inventory

### Functional Requirements

- **FR-1 : Chargement natif** — Risen charge le mod automatiquement depuis le dossier prévu. Le log donne la version du mod et celle du jeu, sous Windows et Proton, sans option de lancement. *(J0 — fait)*
- **FR-2 : Vérification de la version du jeu** — Sur une version non supportée, le mod se désactive proprement, le jeu reste jouable en solo et le log indique « version non supportée ». *(J0 — fait)*
- **FR-3 : Config** — Le mod lit la config au démarrage. Absente ou invalide : il reste inactif, le jeu reste jouable en solo et le log nomme le champ fautif. *(J0 — fait)*
- **FR-4 : Héberger** — Avec le rôle hôte, le chargement d’une partie démarre le serveur.
- **FR-5 : Rejoindre** — Avec le rôle invité, le chargement de la sauvegarde de départ déclenche la connexion à l’hôte. La connexion échoue proprement si l’hôte est injoignable ou si la version du mod ou du protocole diffère : la raison va au log et le jeu solo reste intact. Une deuxième connexion d’invité est refusée.
- **FR-6 : Apparition de l’invité** — À la connexion, le personnage de l’invité est placé près de celui de l’hôte, avec l’équipement défini dans sa config.
- **FR-7 : Avatar synchronisé** — Chaque joueur voit l’avatar de l’autre se déplacer et jouer ses animations : marche, course, saut, dégainer, attaques de mêlée, mort. Aucune téléportation visible avec une latence ≤ 100 ms. L’avatar disparaît à la déconnexion de l’autre joueur.
- **FR-8 : Fin de session** — La session prend fin quand l’invité se déconnecte ou quitte, ou quand l’hôte quitte ou meurt. Chaque jeu revient à un état solo, sans crash.
- **FR-9 : Répliques** *(J2)* — Chez l’invité, la position, l’orientation, l’animation, les PV et la mort des PNJ proches de l’hôte suivent l’état de l’hôte, sans IA locale. Un monstre peut cibler l’invité.
- **FR-10 : Dégâts de l’invité** *(J2)* — Un coup de l’invité inflige des dégâts au monstre chez l’hôte, et les PV visibles baissent chez les deux.
- **FR-11 : Dégâts subis par l’invité** *(J2)* — Les attaques des monstres sur l’invité réduisent ses PV dans son jeu.
- **FR-12 : Mort et réapparition** *(J2)* — L’invité mort réapparaît près de l’hôte, PV pleins. Si l’hôte meurt, la session prend fin.

### NonFunctional Requirements

- **NFR-1 : Robustesse.** Aucun crash du jeu, quel que soit l’échec du mod (réseau, version, config, appel au jeu). Toute erreur laisse le jeu en solo et en donne la raison au log.
- **NFR-2 : Fluidité.** Avec une latence ≤ 100 ms (LAN ou VPN), aucune téléportation visible de l’avatar (FR-7). `[ASSUMPTION PRD]`
- **NFR-3 : Endurance.** Une session de 30 minutes d’exploration se déroule sans crash (SM-2). `[ASSUMPTION PRD]`
- **NFR-4 : Plateformes.** Seule la version Steam 64 bits de Risen est visée (empreinte de janvier 2023), sous Windows et Proton.
- **NFR-5 : Livraison.** Le mod se livre en une DLL et un `multiplayer.ini`, copiés dans `bin/scripts/`. Pas d’installeur, pas d’option de lancement.
- **NFR-6 : Pas d’interface en jeu.** Ni menu, ni chat, ni overlay ; le seul retour est le log.
- **NFR-7 : Strict nécessaire (SM-C1).** On vise la fiabilité du minimum synchronisé, pas l’étendue.
- **NFR-8 : Licence.** GPL-3.0, et aucun fichier du jeu dans le dépôt.
- **NFR-9 : Réseau.** Connexion directe par IP (redirection de port ou VPN). Pas de traversée de NAT, de matchmaking ni de chiffrement.

### Additional Requirements

Exigences de la spine (AD-n) et du spike J1 qui touchent l’implémentation de J1 :

- **Base existante (pas de starter).** Le projet est brownfield. Les stories J1 partent du client existant :
  - couches `core/`, `net/`, `game/`, `sync/`, `win/`, `common/` (migration J0 faite) ;
  - MinHook vendorisé et hooks `OnProcess`/`PlayMotion` en place (sonde) ;
  - banc Wine `tests/wine/` et tests Linux de `core/`.
- **AD-1** — La DLL de l’hôte héberge, sans serveur séparé (ni Go ni `server/` dans le produit). Un seul invité ; une autre adresse reçoit `Reject`. Exception : l’outil de test `tools/fake-guest/` en Go, jamais livré, vérifié contre les vecteurs binaires de `protocol/vectors/` et qui utilise le même `ikcp.c` (via cgo).
- **AD-2** :
  - Le jeu n’est appelé que depuis le hook `OnProcess` ; le thread réseau échange avec la frame par deux files sous verrou.
  - L’état partagé entre le hook `PlayMotion` et la frame est un seul `sync::AnimFilter`, avec un garde de rejeu `thread_local`.
  - Toute cadence se mesure à l’horloge (QPC).
  - Journaliser le thread du hook `PlayMotion` (hypothèse : thread principal).
- **AD-3** — Seul `game/` appelle le jeu. Résolution en bloc par nom décoré, sret explicite, SEH autour de chaque appel. Les objets passés par valeur sont détruits par l’appelé, ceux retournés par valeur par le mod. L’empreinte couvre Game, Engine, Script et SharedBase.
- **AD-4** — Un socket UDP par pair. L’octet 0 donne le canal : 0 pour KCP fiable, 1 pour les snapshots, 2 pour le contrôle. KCP en `nodelay(1,10,2,1)`, MTU 1199, datagramme ≤ 1200 octets. Messages en structs little-endian, contrat dans `protocol/PROTOCOL.md`.
- **AD-5** :
  - `Hello{versions, empreinte, pseudo, équipement}` → `Welcome{conv, pose de l’hôte}` ou `Reject{raison}` ;
  - un `Hello` retransmis depuis la même adresse reçoit le même `Welcome` ;
  - heartbeat 1 Hz, et 5 s sans paquet mettent fin à la session.
- **AD-6** — `NetId` u32 attribué par l’hôte : 1 = avatar de l’hôte, 2 = avatar de l’invité. Aucun pointeur sur le fil. Aucun pointeur d’entité gardé hors de la table `NetId`.
- **AD-7** :
  - Snapshots complets à 20 Hz : `u8 canal`, `u32 tick`, `u8 nombre`, puis 22 octets par entité (`NetId`, position, lacet, PV).
  - `tick = floor((QPC − t0)/50 ms)`.
  - Interpolation à 150 ms (Hermite et slerp), extrapolation de 250 ms au plus.
  - Une seule horloge distante par pair, dans `core/`.
- **AD-8** :
  - Animation par rejeu des `PlayMotion` par nom : `AniName` et `Motion{NetId, ms, type, AniId, flag, paramètres}` sur le canal fiable. Le récepteur fait `GetMotion(nom)` → `SetMotion` → `PlayMotion`.
  - Le moteur est bloqué sur les PS des répliques.
  - L’avatar est créé uniquement sur `Spawn` (`Entity::Spawn(template du héros, pose, temporary)`) et placé par `MoveTo` ; `sync/avatar` est son seul propriétaire.
  - La sonde et la session sont exclusives.
  - `[ASSUMPTION]` Le chemin par nom et le descripteur reconstruit sont à valider en premier, par un miroir local par nom.
- **AD-10** — Liste fermée des événements fiables : `Heartbeat`, `AniName`, `Motion`, `Spawn`, `Despawn`, `HitRequest`, `HitOnGuest`, `Death`, `Respawn`, `SessionEnd`. Contrôle : `Hello`, `Welcome`, `Reject`.
- **AD-11** :
  - Machine à états `Inactive → Hosting | Connecting → InSession → Ended → Inactive`.
  - Monde chargé = `GetPlayer` non nul.
  - Fin de monde = changement du pointeur du joueur ou d’un second signal de génération `[ASSUMPTION à valider en jeu : retour au menu, chargement rapide]`.
  - Une seule transition vers `Ended` supprime les avatars et rend la main au solo.
- **Conventions** :
  - pas d’exception qui franchit une couche ;
  - préfixes de log `[net]`, `[game]`, `[sync]`, et rien par frame hors `log_level = debug` (clé à ajouter) ;
  - toute nouvelle clé de config a un défaut documenté dans `multiplayer.example.ini` ;
  - tests : `core/` sous Linux, `net/` en boucle locale puis sous Wine, `game/` et `sync/` sur le banc Wine et le jeu réel ;
  - KCP vendorisé (version 2.1.1).
- **Reportés à J2** (questions ouvertes 3, 5, 6, 7 de la spine) : arme dégainée et mode de combat de l’avatar, collisions PNJ/avatar, mort native de l’invité, PNJ natifs de l’invité. *Conséquence pour J1 :* « dégainer » (FR-7) n’est couvert que par l’animation ; l’arme en main est reportée au spike J2.

### UX Design Requirements

Aucun document UX : le PRD exclut toute interface en jeu (NFR-6). Le seul « affichage » est le log, régi par les conventions de la spine.

### FR Coverage Map

- FR-1, FR-2, FR-3 : J0 — livré
- FR-4 : Epic 1 — l’hôte héberge au chargement d’une partie
- FR-5 : Epic 1 — l’invité se connecte ; échecs propres ; second invité refusé
- FR-6 : Epic 2 — apparition de l’invité près de l’hôte ; **l’équipement de la config est reporté à J2 (Epic 3)**, avec l’arme en main
- FR-7 : Epic 2 — déplacement et animations de l’avatar, fluides
- FR-8 : Epic 1 (fin de session, retour au solo) + Epic 2 (retrait des avatars)
- FR-9 à FR-12 : Epic 3 — esquisse J2 (plus l’équipement de l’invité, reporté de FR-6)

## Epic List

### Epic 1 : Se retrouver en session (hôte ↔ invité)
L’hôte charge sa partie et héberge ; l’invité charge la sienne et se connecte. Les deux jeux savent qu’ils sont en session. Tout échec ou toute fin de session ramène proprement au solo, et la raison est au log. Pas encore d’avatar : la session se vérifie dans les deux logs.
**FRs covered:** FR-4, FR-5, FR-8 (sauf le retrait des avatars)

### Epic 2 : Voir l’autre joueur bouger
En session, chacun voit apparaître l’avatar de l’autre (celui de l’invité près de l’hôte), qui se déplace et joue ses animations (marche, course, saut, attaques, mort) de façon fluide, puis disparaît en fin de session. Première story : un miroir local par nom, sans réseau, pour lever le risque d’AD-8. Les tests se font avec une seule instance de Risen (hôte) et le faux invité de la story 1.3 ; la validation finale se fait à deux machines.
**FRs covered:** FR-6, FR-7, FR-8 (retrait des avatars)

### Epic 3 (esquisse J2) : Combattre ensemble
Les deux joueurs combattent les mêmes monstres. Commence par le spike J2 (questions ouvertes 3, 5, 6 et 7 de la spine).
**FRs covered:** FR-9, FR-10, FR-11, FR-12, FR-6 (équipement de l’invité, reporté de J1)

## Epic 1 : Se retrouver en session (hôte ↔ invité)

L’hôte charge sa partie et héberge ; l’invité charge la sienne et se connecte. Les deux jeux savent qu’ils sont en session. Tout échec ou toute fin de session ramène proprement au solo, et la raison est au log. FR-4, FR-5, FR-8 (sauf le retrait des avatars). Les tests en jeu se font avec Risen en hôte et le faux invité (story 1.3) ; le côté invité de la DLL est validé à deux machines.

### Story 1.1 : Contrat binaire de session et codec

As a développeur du mod,
I want un contrat binaire versionné (`protocol/PROTOCOL.md`) et son codec testé sous Linux,
So that l’hôte et l’invité échangent des messages identiques et que toute différence de version soit détectable.

**Acceptance Criteria:**

**Given** `protocol/PROTOCOL.md` v1
**When** on le lit
**Then** il décrit les 3 canaux (octet 0) ; `Hello{version du protocole, version du mod, empreinte du jeu, pseudo, équipement}`, `Welcome{conv, pose}` et `Reject{raison}` ; `Heartbeat` et `SessionEnd{raison}` ; les codes de raison, le little-endian, les textes UTF-8 préfixés `u8` et la limite de 1200 octets.

**Given** chaque message de la v1
**When** on l’encode puis qu’on le décode (`core/protocol`)
**Then** on retrouve exactement le message de départ (tests Linux)
**And** un tampon tronqué, un type inconnu ou une longueur incohérente donne une erreur, jamais une lecture hors limites.

**Given** les vecteurs binaires de `protocol/vectors/` (un fichier `.bin` d’octets attendus par message, plus quelques tampons invalides)
**When** les tests C++ les décodent et réencodent
**Then** ils retrouvent les octets exacts, et les tampons invalides sont rejetés
**And** ces mêmes fichiers servent de référence au faux invité en Go (story 1.3).

**Given** la config
**When** la clé `log_level = debug | info` est présente ou absente
**Then** elle est lue avec `info` par défaut et documentée dans `multiplayer.example.ini`.

### Story 1.2 : Transport UDP + KCP en boucle locale

As a développeur du mod,
I want un transport réseau (socket UDP, canaux, KCP, thread réseau) qui n’échange avec le jeu que par deux files,
So that la session s’y branche sans jamais appeler le jeu hors du thread principal (AD-2, AD-4).

**Acceptance Criteria:**

**Given** KCP 2.1.1 vendorisé dans `client/third_party/kcp/` (version et licence notées, compilé dans la cible)
**When** on construit le client
**Then** il compile sous Linux (tests) et en DLL Windows, et la DLL n’importe que KERNEL32, USER32 et WS2_32.

**Given** deux pairs en boucle locale (`127.0.0.1`)
**When** ils échangent des datagrammes de contrôle (canal 2) et des messages KCP (canal 0, `nodelay(1,10,2,1)`, MTU 1199)
**Then** les messages fiables arrivent dans l’ordre, même avec 20 % de pertes simulées
**And** aucun datagramme ne dépasse 1200 octets.

**Given** le thread réseau en marche
**When** la frame dépose un message dans la file sortante ou en retire un de la file entrante
**Then** seules ces deux files sont partagées, sous verrou
**And** l’arrêt du thread est propre (join, socket fermé), testé sous Linux puis sous Wine.

### Story 1.3 : Faux invité de session (outil de test en Go)

As a développeur du mod,
I want un faux invité en ligne de commande, `tools/fake-guest/`, qui joue le protocole de session comme un vrai invité,
So that je teste l’hôte avec une seule instance de Risen, puisque deux instances ne peuvent pas tourner sur la même machine.

**Acceptance Criteria:**

**Given** `tools/fake-guest/` (module Go, hors de la DLL et jamais livré)
**When** on lance `go test ./...`
**Then** il décode et réencode chaque vecteur de `protocol/vectors/` à l’octet près et rejette les tampons invalides
**And** son KCP est le `ikcp.c` vendorisé du client (story 1.2), compilé via cgo, avec les mêmes réglages (`nodelay(1,10,2,1)`, MTU 1199).

**Given** un hôte simulé dans les tests Go (il répond `Welcome` ou `Reject` selon le `Hello`)
**When** on lance `fake-guest --host <ip> --port <port>` contre lui
**Then** le faux invité renvoie `Hello` toutes les 500 ms jusqu’au `Welcome`, échange les `Heartbeat` à 1 Hz, et journalise ses transitions (`Connecting`, `InSession`, `Ended`) et tout ce qu’il reçoit.

**Given** les options de faute : `--drop <pourcentage>`, `--kill` (arrêt sans `SessionEnd`), `--quit` (arrêt avec `SessionEnd`), `--dup-hello` et `--bad-version`
**When** on les utilise contre l’hôte simulé
**Then** chacune produit le comportement attendu côté réseau (pertes, silence, `SessionEnd`, `Hello` en double, `Reject`).

### Story 1.4 : Héberger et rejoindre une session

As a joueur hôte ou invité,
I want que le chargement de ma partie démarre le serveur (hôte) ou la connexion (invité),
So that nous nous retrouvions en session sans aucune manipulation (FR-4, FR-5).

**Acceptance Criteria:**

**Given** le mod actif, `probe = 0` et le rôle hôte
**When** le premier `OnProcess` voit un monde chargé (`GetPlayer` non nul)
**Then** la session passe à `Hosting` : l’hôte écoute sur `port`, et le log l’indique.

**Given** le rôle invité et `host_ip` renseigné
**When** un monde est chargé
**Then** la session passe à `Connecting` et `Hello` est renvoyé toutes les 500 ms.

**Given** un `Hello` compatible
**When** l’hôte le reçoit
**Then** il répond `Welcome{conv, pose}`, les deux KCP sont créés et les deux pairs passent en `InSession`, ce que les deux logs indiquent
**And** un `Hello` retransmis depuis la même adresse `ip:port` reçoit le même `Welcome`.

**Given** une version de protocole, une version du mod ou une empreinte différente
**When** l’hôte reçoit le `Hello`
**Then** il répond `Reject{raison}`, la raison est journalisée des deux côtés et l’invité revient au solo.

**Given** une session en cours
**When** un `Hello` arrive d’une autre adresse
**Then** il reçoit `Reject` (session pleine) et la session en cours n’est pas affectée.

**Given** un hôte injoignable
**When** 10 s se passent sans `Welcome` `[ASSUMPTION : délai]`
**Then** l’invité abandonne, journalise « hôte injoignable » et reste en solo.

**Given** une session établie
**When** les pairs ne font rien
**Then** un `Heartbeat` circule à 1 Hz, mesuré à l’horloge
**And** 5 s sans paquet font passer en `Ended`, avec la raison « timeout ».

**Given** `probe = 1`
**When** le mod démarre
**Then** aucune session n’est lancée (la sonde et la session sont exclusives).

**Given** Risen en hôte et le faux invité (story 1.3)
**When** on joue les cas nominal, `--dup-hello`, `--bad-version`, deux faux invités à la fois et `--drop 20`
**Then** l’hôte se comporte comme décrit ci-dessus (même `Welcome`, `Reject` de version, `Reject` de session pleine, session maintenue malgré les pertes).

**Given** le côté invité de la DLL (`Connecting`, réception de `Welcome` ou `Reject`, hôte injoignable)
**When** la story est livrée
**Then** l’hôte injoignable est vérifié avec une seule instance (aucun hôte en écoute)
**And** le reste du côté invité n’est validé qu’à deux machines ; le log de la story le dit explicitement, sans le présenter comme couvert par le faux invité.

### Story 1.5 : Fin de session propre

As a joueur,
I want que toute fin de session (quitter, recharger, retour au menu, mort de l’hôte) ramène les deux jeux au solo sans crash,
So that nous puissions rejouer sans relancer le jeu (FR-8, AD-11).

**Acceptance Criteria:**

**Given** une session en cours
**When** le joueur charge une sauvegarde, recharge rapidement ou revient au menu
**Then** la fin de monde est détectée par le changement du pointeur du joueur ou par un second signal. Le signal retenu, `gCSession::AccessState()` ou `eCEntity::GetID()`, est choisi par un test en jeu et consigné dans `docs/re/`
**And** la session passe en `Ended`, `SessionEnd{raison}` est envoyé à l’autre pair, et les deux reviennent au solo.

**Given** l’hôte en session
**When** son héros meurt
**Then** `SessionEnd{mort de l’hôte}` est envoyé et les deux reviennent au solo (FR-8, FR-12).

**Given** un joueur qui quitte le jeu
**When** le processus se termine
**Then** l’autre pair détecte la fin, par `SessionEnd` si l’envoi a eu le temps de partir, sinon par le timeout de 5 s, et revient au solo.

**Given** n’importe quelle cause de fin
**When** elle survient
**Then** elle passe par la même transition `Ended → Inactive`, journalisée avec sa raison
**And** une nouvelle session redevient possible au monde chargé suivant, sans relancer le jeu.

**Given** Risen en hôte et le faux invité
**When** le faux invité part avec `--quit`, puis avec `--kill`
**Then** l’hôte revient au solo par `SessionEnd`, puis par le timeout de 5 s, avec la bonne raison au log.

**Given** le scénario « héberger, rejoindre (faux invité), recharger 3 fois » sous Proton
**When** on le joue
**Then** il n’y a aucun crash, et les logs montrent 3 sessions distinctes
**And** le même scénario est rejoué à deux machines lors de la validation finale de J1.

## Epic 2 : Voir l’autre joueur bouger

En session, chacun voit apparaître l’avatar de l’autre (celui de l’invité près de l’hôte), qui se déplace et joue ses animations de façon fluide, puis disparaît en fin de session. FR-6 (placement), FR-7, FR-8 (retrait des avatars). Les tests du quotidien se font avec Risen en hôte et le faux invité (story 1.3) ; la validation finale se fait à deux machines.

### Story 2.1 : Miroir local par nom (lever le risque d’AD-8)

As a développeur du mod,
I want que la copie de la sonde rejoue mes animations à partir de leur seul **nom** et d’un descripteur reconstruit,
So that le chemin d’animation exact que suivra l’avatar soit validé sans réseau.

**Acceptance Criteria:**

**Given** la sonde en mode miroir
**When** je joue
**Then** chaque animation est rejouée par `eCResource2::GetMotion(nom)` → `SetMotion` → `PlayMotion`, sans pointeur de ressource ni descripteur brut
**And** la copie reproduit marche, course, saut, attaques et mort comme en v5.

**Given** les dumps de descripteurs de la sonde
**When** on compare leurs champs
**Then** les champs scalaires à transmettre sont identifiés et consignés dans `docs/re/spike-j1.md`
**And** la disposition de `Motion` est ajoutée à `PROTOCOL.md`, avec son vecteur dans `protocol/vectors/`.

**Given** l’état partagé avec le hook `PlayMotion`
**When** la story est finie
**Then** il passe par un `sync::AnimFilter` unique avec un garde de rejeu `thread_local` (AD-2), réutilisable par l’avatar.

**Given** le hook `PlayMotion`
**When** il est appelé
**Then** son thread est journalisé une fois, pour confirmer l’hypothèse d’AD-2.

### Story 2.2 : Apparition et retrait des avatars

As a joueur,
I want voir le personnage de l’autre apparaître à la connexion, puis disparaître à la fin de la session,
So that je sache que nous sommes ensemble (FR-6, FR-7, FR-8).

**Acceptance Criteria:**

**Given** le passage en `InSession`
**When** la session démarre
**Then** le héros de l’invité est téléporté (`MoveTo`) près du héros de l’hôte, d’après la pose de `Welcome`
**And** chaque pair reçoit un `Spawn` pour l’avatar de l’autre (`NetId` 1 pour l’hôte, 2 pour l’invité) et le crée par `Entity::Spawn(template du héros local, pose, temporary)`.

**Given** n’importe quelle fin de session
**When** on passe en `Ended`
**Then** l’avatar est retiré de l’`AnimFilter` et de la table `NetId`, puis détruit (`Despawn`)
**And** il ne reste aucun pointeur vers lui.

**Given** Risen en hôte et le faux invité
**When** on enchaîne « rejoindre, quitter (`--kill` puis normal), rejoindre »
**Then** il n’y a jamais deux avatars à la fois, et aucun crash.

**Given** les messages `Spawn` et `Despawn` (AD-10)
**When** la story est livrée
**Then** leur disposition est dans `PROTOCOL.md`, avec leurs vecteurs dans `protocol/vectors/` vérifiés par les tests C++ et Go
**And** le faux invité les reçoit et les journalise, et envoie le `Spawn` de son propre avatar.

### Story 2.3 : Déplacements fluides de l’avatar

As a joueur,
I want voir l’avatar de l’autre se déplacer sans à-coups,
So that sa présence soit crédible (FR-7, NFR-2).

**Acceptance Criteria:**

**Given** `core/`
**When** on lance les tests Linux
**Then** le codec de snapshot (AD-7), l’horloge distante et l’interpolation (Hermite et slerp, retard de 150 ms, extrapolation ≤ 250 ms) sont couverts, pertes et gigue comprises.

**Given** le format de snapshot (AD-7)
**When** la story est livrée
**Then** il est dans `PROTOCOL.md` avec ses vecteurs, vérifiés par les tests C++ et Go
**And** le faux invité accepte `--scenario <fichier>` décrivant une trajectoire (cercle, aller-retour, demi-tours) et envoie ses snapshots à 20 Hz avec le tick de la spine.

**Given** une session
**When** chaque pair envoie ses snapshots à 20 Hz, mesurés à l’horloge
**Then** l’avatar est placé à chaque frame par `MoveTo` sur la pose interpolée.

**Given** le faux invité avec un scénario (cercle, aller-retour, demi-tours) et une latence simulée de 100 ms (`tc netem` ou `--drop`)
**When** l’avatar suit la trajectoire chez l’hôte
**Then** on ne voit aucune téléportation.

### Story 2.4 : Animations de l’avatar

As a joueur,
I want voir l’avatar refaire ce que fait l’autre joueur (marche, course, saut, dégainer, attaques, mort),
So that nous puissions jouer ensemble (FR-7).

**Acceptance Criteria:**

**Given** une session
**When** le joueur local joue une animation
**Then** `AniName` (la première fois) et `Motion{NetId, ms, type, AniId, flag, paramètres}` partent sur le canal fiable.

**Given** `AniName` et `Motion`
**When** la story est livrée
**Then** leurs vecteurs (la disposition de `Motion` vient de la story 2.1) sont vérifiés par les tests C++ et Go
**And** le scénario du faux invité peut contenir des noms d’animation horodatés, envoyés en `AniName`/`Motion`.

**Given** la réception d’un `Motion`
**When** l’instant d’interpolation est atteint
**Then** l’animation est rejouée sur l’avatar
**And** un `Motion` en retard s’applique tout de suite ; pour les types 0 et 1, seul le plus récent par frame est appliqué ; un `AniId` inconnu fait ignorer le `Motion`, avec une ligne de log.

**Given** l’avatar
**When** le moteur lui lance une animation de lui-même
**Then** elle est bloquée (`AnimFilter`).

**Given** le faux invité qui rejoue des noms d’animation pris dans les logs de la sonde (course, saut, attaques)
**When** il les envoie
**Then** l’avatar les joue chez l’hôte, synchronisés avec le déplacement.

**Given** deux machines (validation finale de J1)
**When** l’un des joueurs court, saute, frappe et dégaine
**Then** l’autre voit les mêmes gestes, synchronisés avec le déplacement
**And** pour le dégainage, seule l’animation est attendue ; l’arme en main est reportée à J2.

## Epic 3 (esquisse J2) : Combattre ensemble

Les deux joueurs combattent les mêmes monstres. FR-9 à FR-12, plus l’équipement de l’invité reporté de FR-6. Les stories seront détaillées après le spike J2 ; titres pressentis :

- **3.0 Spike J2** : questions ouvertes 3, 5, 6 et 7 de la spine (arme en main et mode combat de l’avatar, collisions, mort native de l’invité, PNJ natifs chez l’invité).
- **3.1 Équipement de l’invité depuis la config** (`guest_equipment`, porté aussi par son avatar chez l’hôte).
- **3.2 Arme en main et mode combat de l’avatar.**
- **3.3 Répliques des PNJ chez l’invité** (FR-9).
- **3.4 Dégâts de l’invité sur les monstres** (FR-10, `HitRequest`).
- **3.5 Dégâts subis par l’invité** (FR-11, `HitOnGuest`).
- **3.6 Mort et réapparition** (FR-12, `Death`/`Respawn`).

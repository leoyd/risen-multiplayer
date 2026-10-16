# Epic 1 Context: Se retrouver en session (hôte ↔ invité)

<!-- Compiled from planning artifacts. Edit freely. Regenerate with compile-epic-context if planning docs change. -->

## Goal

Établir une session coop à 2 joueurs sans manipulation : l’hôte charge sa partie et la DLL se met à héberger ; l’invité charge la sauvegarde de départ et se connecte directement à l’IP de l’hôte. Les deux jeux savent qu’ils sont en session, et tout échec ou toute fin de session (version différente, hôte injoignable, timeout, départ, rechargement, mort de l’hôte) ramène proprement au solo, sans crash, avec la raison au log. Il n’y a pas encore d’avatar : la session se vérifie dans les deux logs. C’est le socle (contrat binaire, transport, machine à états) sur lequel reposent les avatars (Epic 2) et le combat coop (Epic 3).

## Stories

- Story 1.1 : Contrat binaire de session et codec
- Story 1.2 : Transport UDP + KCP en boucle locale
- Story 1.3 : Faux invité de session (outil de test en Go)
- Story 1.4 : Héberger et rejoindre une session
- Story 1.5 : Fin de session propre

## Requirements & Constraints

- **Héberger** : avec le rôle hôte, le chargement d’une partie démarre l’écoute sur `port` (27960 par défaut).
- **Rejoindre** : avec le rôle invité et `host_ip` renseigné, le chargement d’un monde lance la connexion. Échec propre si l’hôte est injoignable (abandon après ~10 s sans `Welcome`, délai supposé) ou si la version du protocole, du mod ou l’empreinte du jeu diffère. Un second invité est refusé (session pleine) sans perturber la session en cours.
- **Fin de session** : invité qui part ou quitte, hôte qui quitte ou meurt, rechargement, chargement rapide, retour au menu → les deux jeux reviennent au solo, et une nouvelle session redevient possible au monde chargé suivant sans relancer le jeu.
- **Robustesse** : aucun échec du mod (réseau, version, config, appel au jeu) ne doit faire crasher le jeu ; toute erreur laisse le jeu en solo avec la raison au log. Session de 30 min sans crash visée.
- **Pas d’interface en jeu** : le seul retour est le log (`Script_Multiplayer.log`). Chaque transition de session et chaque `Reject` y sont journalisés.
- **Réseau** : connexion directe par IP (redirection de port ou VPN) ; pas de NAT traversal, matchmaking ni chiffrement.
- **Livraison** : une DLL + `multiplayer.ini` dans `bin/scripts/`. La DLL n’importe que KERNEL32, USER32 et WS2_32. Aucun fichier du jeu dans le dépôt (GPL-3.0).
- **Config** : nouvelle clé `log_level = debug | info` (défaut `info`). Toute nouvelle clé a un défaut documenté dans `multiplayer.example.ini`. Le champ équipement existe dans `Hello` dès la v1, mais son application est reportée à J2.

## Technical Decisions

- **Pas de serveur séparé** : la DLL de l’hôte ouvre le socket UDP et accepte exactement un invité. Seule exception : l’outil de test `tools/fake-guest/` (Go, jamais livré).
- **Transport** : un socket UDP par pair ; octet 0 = canal (`0` KCP fiable, `conv` = identifiant de session tiré par l’hôte ; `1` snapshot non fiable ; `2` contrôle hors KCP). KCP 2.1.1 vendorisé dans `client/third_party/kcp/`, compilé dans la cible, en `ikcp_nodelay(1,10,2,1)` + `ikcp_setmtu(1199)`, sans FEC. Datagramme ≤ 1200 octets.
- **Poignée de main** : `Hello{version protocole, version mod, empreinte jeu, pseudo, équipement}` renvoyé toutes les 500 ms → `Welcome{conv, pose de l’avatar hôte}` ou `Reject{raison u8}`. KCP créé des deux côtés seulement après `Welcome`. Un `Hello` retransmis depuis la même `ip:port` reçoit le même `Welcome` (même `conv`) ; l’hôte cesse de renvoyer `Welcome` au premier segment KCP portant ce `conv`. Toute autre adresse reçoit `Reject`.
- **Heartbeat** à 1 Hz ; 5 s sans paquet → `Ended`, raison « timeout ».
- **Messages (liste fermée)** : contrôle = `Hello`, `Welcome`, `Reject` ; fiable = `Heartbeat`, `SessionEnd{raison u8}` pour cet epic (les autres types sont réservés aux epics suivants). Ajouter un type exige de mettre à jour `protocol/PROTOCOL.md` et d’incrémenter la version du protocole.
- **Sérialisation** : structs little-endian empaquetées, `enum class MsgType : u8`, une struct par message dans `core/protocol.h`, textes UTF-8 préfixés `u8`. Le décodage ne lit jamais hors limites (tampon tronqué, type inconnu, longueur incohérente → erreur). Vecteurs binaires de référence dans `protocol/vectors/` (un `.bin` par message + tampons invalides), vérifiés par les tests C++ et Go.
- **Threads** : le jeu n’est appelé que depuis le hook `eCApplication::OnProcess` (thread principal). Le thread réseau ne fait qu’envoyer/recevoir et n’échange avec la frame que par deux files (entrante, sortante) sous verrou. Arrêt propre (join, socket fermé).
- **Temps** : `OnProcess` n’est pas bridé (160–250 appels/s en jeu, > 2 000 au menu) → toute cadence et tout délai se mesurent à l’horloge (QPC), jamais en nombre d’appels.
- **Machine à états** : `Inactive → Hosting | Connecting → InSession → Ended → Inactive`. Monde chargé = `Entity::GetPlayer()` non nul (nul au menu) ; `Hosting`/`Connecting` démarrent au premier `OnProcess` où c’est le cas. Fin de monde = changement du pointeur du joueur ou d’un second signal de génération (`gCSession::AccessState()` ou `eCEntity::GetID()` du héros, à départager par un test en jeu et à consigner dans `docs/re/`). Toutes les causes de fin passent par **la même** transition vers `Ended`, qui envoie `SessionEnd{raison}` si possible, journalise la raison et rend la main au solo.
- **Sonde et session exclusives** : avec `probe = 1`, aucune session n’est lancée.
- **ABI** : seul `game/` appelle le jeu ; résolution par nom décoré en tout ou rien, sret explicite, SEH autour de chaque appel. Aucun pointeur d’entité gardé hors de la table `NetId` ni réutilisé après `Ended`.
- **Conventions** : couches `core/` (pur, tests Linux), `net/` (boucle locale Linux puis Wine), `game/` et `sync/` (banc Wine + jeu réel) ; espaces de noms `mp::core|net|game|sync|win|log`. Pas d’exception qui franchit une couche : `net/`/`game/` retournent `bool` ou un résultat, `sync/` décide. Préfixes de log `[net]`, `[game]`, `[sync]` ; rien par frame hors `log_level = debug`.

## Cross-Story Dependencies

- 1.1 (contrat + vecteurs) est le prérequis de 1.2, 1.3 et 1.4 ; les vecteurs `protocol/vectors/` sont la référence commune C++/Go.
- 1.3 réutilise le `ikcp.c` vendorisé en 1.2 (via cgo) avec les mêmes réglages ; il offre les fautes `--drop`, `--kill`, `--quit`, `--dup-hello`, `--bad-version`, utilisées pour valider 1.4 et 1.5.
- 1.4 s’appuie sur 1.1–1.3 ; 1.5 étend la machine à états de 1.4.
- Deux instances de Risen ne tournent pas sur la même machine : l’hôte est testé avec Risen + faux invité ; côté invité de la DLL, seul « hôte injoignable » se vérifie avec une instance, le reste ne se valide qu’à deux machines (à dire explicitement dans les logs de story).
- Epic 2 branche les avatars (`Spawn`/`Despawn`, retrait des avatars à `Ended`) sur la session et la transition `Ended` de cet epic ; la mort de l’hôte (fin de session) prépare FR-12 de l’Epic 3.

---
type: review
lens: rubric
target: ARCHITECTURE-SPINE.md (J1–J2), mise à jour post-spike J1 (2026-10-16)
scope: AD-2, AD-7, AD-8, AD-10, AD-11, Open Questions + cohérence avec le reste et le code
date: 2026-10-16
verdict: needs-revision (mineure)
---

# Revue rubrique — mise à jour post-spike J1

**Verdict : bonne mise à jour, fidèle au spike, mais deux trous laisseraient diverger des stories (animation des PNJ en J2, et un chemin récepteur présenté comme validé alors qu’il ne l’est pas). À corriger avant de découper les stories J1.**

Sources lues : la spine (diff vs `HEAD`), les 14 dernières entrées de `.memlog.md`, `docs/re/spike-j1.md`, et le code `client/src/` (`game/entity.h`, `game/abi.h`, `sync/probe.cpp`, `win/hooks.cpp`, `common/log.h`).

Sévérités : **critique** (divergence certaine / protocole faux), **haute** (divergence probable entre stories), **moyenne** (règle incomplète ou non applicable telle quelle), **basse** (cohérence, hygiène).

---

## Ce qui est bien

- AD-2 : la cadence à l’horloge (`QueryPerformanceCounter`) est une vraie règle, applicable, appuyée sur la mesure (160–250/s en partie, plus de 2 000 au menu). Elle évite le piège « 1 appel = 1 frame à 60 Hz ».
- AD-7 : sortir l’animation du snapshot est la bonne décision (les couches 0/1 se mélangent et les coups ponctuels se perdraient dans un état échantillonné). Le passage de 25 à 22 octets est cohérent (4 + 12 + 4 + 2).
- AD-8 : la règle « le descripteur n’est jamais transmis brut » et « seul `sync/avatar` anime un avatar » ferment deux divergences réelles. Les repères `bCMatrix` (lignes, `m[12..14]`, Y vertical, avant en ligne 2, cm) sont tous appuyés par le spike.
- AD-10 : `Motion` est bien ajouté à la liste fermée, sur le canal fiable.
- AD-11 : « monde chargé = `GetPlayer()` non nul » et « aucun pointeur d’entité ne survit à un changement de joueur » sont des règles testables.
- Open Questions : les questions résolues sont barrées avec leur résolution ; les deux nouvelles (arme dégainée, collisions) sont bien rattachées à J2.
- Le code existant est ratifié pour l’essentiel : `game/` est le seul à appeler le jeu, `win/hooks` installe `OnProcess` et `PlayMotion` en une activation groupée, `sync/probe` ne passe que par `mp::game`.

---

## Constats

### F1 — haute — L’animation des PNJ n’a plus de chemin (AD-7 × AD-8 × AD-9)

- AD-7 retire l’animation du snapshot pour **toutes** les entités, PNJ compris.
- AD-8 limite l’émetteur au « PS d’animation du **héros local** » et le blocage au « PS d’un **avatar** ».
- AD-9 dit pourtant que chez l’invité « leur position, leur **animation**, leurs PV et leur mort viennent exclusivement des snapshots et des événements ».

Résultat : aucune règle ne dit comment l’animation d’un PNJ part de l’hôte, ni qui bloque les `PlayMotion` que le moteur de l’invité lance sur ses PNJ natifs (IA neutralisée ou non, l’animation locale continue). La story `sync/npc` de J2 inventera son propre schéma (retour d’un `AniId` dans le snapshot, ou autre), ce que AD-8 voulait justement interdire (« deux encodages d’animation concurrents »).

**Correction proposée :** généraliser AD-8 à « toute entité répliquée » :
- émetteur = PS d’animation de toute entité dont le pair a l’autorité **et** qu’il réplique (héros local ; PNJ du périmètre AD-9 chez l’hôte) ;
- blocage = PS de toute **réplique** (avatars et PNJ répliqués) ;
- `Binds` d’AD-8 : ajouter FR-9 ; ajouter AD-8 à la ligne FR-9 (et FR-6) de la Capability Map.
Si on préfère trancher plus tard, l’inscrire explicitement dans l’Open Question 3 (« animation des PNJ : même flux `Motion` qu’AD-8, à confirmer par le spike J2 ») pour qu’aucune story ne parte sans règle.

### F2 — haute — « Validé en local » couvre un autre chemin que celui imposé au récepteur (AD-8)

Le mode miroir de la sonde (`sync/probe.cpp`, `replay_capture`) rejoue :
- le **pointeur** de ressource capturé (pas `eCResource2::GetMotion(nom)`) ;
- une **copie brute** des 0x60 octets du descripteur, pointeurs compris (`memcpy(desc, c.bytes, …)`), dans le même processus.

Or AD-8 impose exactement l’inverse au récepteur : ressource retrouvée **par nom**, descripteur **reconstruit** à partir de scalaires. Ni `GetMotion` (non résolu dans `game/entity.h`) ni la reconstruction n’ont été testés. De plus, `spike-j1.md` documente v4 (`SetPose`) mais pas les **résultats** de v5 (`MoveTo`) : la règle « Placement = `MoveTo` » repose sur une analyse de désassemblage, pas encore sur une observation.

Risque : la story avatar découvre en réseau que `GetMotion(nom)` ne rend pas la même ressource (autre chemin de cache, nom ambigu `GetFileTitleA` vs `GetFileName`) ou que le descripteur reconstruit plante, et contourne la règle localement.

**Correction proposée :**
- reformuler en « validé en local avec le pointeur de ressource et le descripteur brut ; le chemin par nom et le descripteur reconstruit restent `[ASSUMPTION]` » ;
- faire de la première tâche de la story avatar un **miroir par nom** en local (nom → `GetMotion` → `SetMotion` → `PlayMotion` avec descripteur reconstruit), avant tout réseau ;
- consigner les résultats de la sonde v5 (saut, équipement dans le dos) dans `spike-j1.md`, ou marquer la règle de placement `[ASSUMPTION]`.

### F3 — moyenne — `Motion` sur le canal fiable : volume, retard et précision temporelle non réglés (AD-7 × AD-8)

- **Volume :** « chaque appel donne un événement fiable ». Le spike montre des `PlayMotion` en paires (types 0/1) à chaque changement de locomotion, mais la cadence n’est pas mesurée. En J2 avec F1, il faut y ajouter les PNJ. Rien ne borne ce flux sur KCP (fenêtre, MTU 1199).
- **Retard :** AD-7 dit « il n’attend jamais une retransmission », mais un `Motion` perdu bloque KCP (ordre garanti) et arrive après l’instant où il devait être joué. Aucune règle ne dit quoi faire d’un `Motion` en retard (jouer tout de suite, sauter les locomotions périmées, ne jamais sauter une attaque).
- **Précision :** `tick` est un `u32` à 20 Hz (Consistency Conventions). Un `PlayMotion` survient à la résolution d’une frame (4–6 ms). Quantifié à 50 ms, l’attaque peut être décalée d’une demi-période par rapport à la position interpolée.

**Correction proposée :** compléter AD-8 (ou AD-7) par :
1. un horodatage plus fin dans `Motion` (par exemple `tick` + décalage en ms `u8`, ou un temps en ms `u32`) ;
2. une règle de rattrapage : un `Motion` dont l’instant est dépassé est appliqué immédiatement ; pour un même type, seul le plus récent compte si plusieurs sont en retard ;
3. un plafond par entité et par tick (coalescence des locomotions 0/1 d’un même tick), mesuré par la sonde avant d’être chiffré.

### F4 — moyenne — Le hook `PlayMotion` crée un troisième canal de partage non spécifié (AD-2)

AD-2 autorise l’exception « le hook `PlayMotion` ne fait que copier des octets ou bloquer ». Mais :
- le **thread** du hook n’est pas établi. Le spike mesure le thread d’`OnProcess`, pas celui de `PlayMotion`. Le moteur charge `VCOMP140` (OpenMP) ; une mise à jour d’animation parallèle n’est pas exclue. La sonde suppose implicitement le même thread (`g_replaying` est un `bool` non atomique, « même thread que le hook ») tout en protégeant l’anneau par un `SRWLOCK` ;
- pour **bloquer**, le hook doit lire l’ensemble des PS d’avatars (et de répliques, cf. F1), écrit par la frame. Pour **émettre**, il doit transmettre des captures à la frame. C’est un canal hook → frame, distinct des deux files du thread réseau, que la règle « aucune autre donnée n’est partagée » ne couvre pas.

**Correction proposée :** ajouter à AD-2 :
- « le hook `PlayMotion` dépose ses captures dans un anneau borné protégé par un verrou, vidé par la frame ; il ne lit qu’un ensemble de PS (héros local, répliques) publié par la frame de façon atomique » ;
- une vérification à faire par la sonde : journaliser le thread du hook `PlayMotion` (à ajouter à l’Open Question 4 ou en tâche de la story avatar). Si ce n’est pas le thread principal, le marqueur de rejeu doit être `thread_local` ou atomique.

### F5 — moyenne — Contrat d’internement `AniName` incomplet (AD-8 × AD-10)

Points qui peuvent faire diverger l’émetteur et le récepteur :
- **espace des `AniId` :** propre à chaque émetteur (sens hôte → invité et invité → hôte indépendants) ? Ce n’est pas dit ; les deux pairs émettent ;
- **cycle de vie :** remise à zéro à `Ended` (AD-11) ? Sinon un `AniId` d’une session précédente peut être interprété ;
- **ordre :** `AniName` doit précéder le premier `Motion` qui l’utilise sur le même flux KCP (garanti par KCP, mais à écrire) ; que fait le récepteur d’un `AniId` inconnu ou d’un nom que `GetMotion` ne trouve pas (ignorer et journaliser) ?
- **format de `Motion` :** « paramètres » reste ouvert. Ce n’est pas grave, mais il faut nommer qui le fige.

**Correction proposée :** une phrase dans AD-8 : « `AniId` est attribué par l’émetteur, par sens et par session, remis à zéro à `Ended` ; un `AniId` inconnu ou un nom introuvable est ignoré et journalisé ; la story avatar fige la disposition de `Motion` dans `PROTOCOL.md` avant le codec `core/protocol` ».

### F6 — basse — AD-11 s’appuie sur un cas non testé

`spike-j1.md` : « le retour au menu n’a pas encore été testé ». La règle « changement du pointeur du joueur = fin de session » est saine, mais le cas « chargement d’une sauvegarde depuis une partie » (le pointeur passe-t-il par nul ? redevient-il le même pointeur ?) n’est pas observé. Si l’adresse est réutilisée, la détection rate.

**Correction proposée :** marquer `[ASSUMPTION : retour au menu et rechargement font changer ou annuler le pointeur]` et ajouter le test à la sonde ; en complément, accepter aussi `gCSession::AccessState()` comme signal si le pointeur est réutilisé.

### F7 — basse — Arme dégainée : prévoir le type d’événement (Open Question 5 × AD-10)

La liste fermée d’AD-10 n’a pas de place pour l’état de combat (`gECombatMode`). Le spike J2 ajoutera forcément un type (par exemple `CombatMode{NetId, mode}`), donc une montée de version du protocole. Ce n’est pas une divergence (la règle d’ajout existe), mais en J1 l’invité peut déjà dégainer : l’avatar jouera « attaque sans arme » dès J1.

**Correction proposée :** préciser dans l’Open Question 5 que la réponse ajoutera un type à AD-10 (et noter que J1 accepte ce défaut visuel).

### F8 — basse — Ratification du code existant : deux points périmés

- Consistency Conventions : la migration J0 (`win/game_abi` → `game/`, `win/log` → `common/`, `mp::abi` → `mp::game`) est annoncée « dans la première story de J1 ». Elle est **déjà faite** (`client/src/game/abi.h` en `mp::game`, `client/src/common/log.h` en `mp::log`). À marquer comme faite.
- Structural Seed : `sync/probe` existe et `win/hooks` en dépend directement (`detour_play_motion` → `sync::probe::on_play_motion`). La spine ne dit pas si la sonde reste (outil de diagnostic derrière `probe = 1`) ou disparaît quand `sync/avatar` prend le hook. Sans règle, deux détours concurrents peuvent cohabiter.
  **Correction :** une ligne dans le Structural Seed : « `sync/probe` : outil de diagnostic, exclusif de `sync/avatar` (un seul consommateur du hook `PlayMotion`) » ou sa suppression planifiée.

### F9 — basse — Offsets codés en dur hors du filet d’AD-3

AD-3 protège les **symboles** (tout ou rien). Les nouvelles dépendances sont des **offsets** (`this + 0x120 + type*0x10`, taille du descripteur 0x60, PS 0x52, champs +0x00/+0x0C/+0x14). Ils ne sont protégés que par l’empreinte de version, ce qui est suffisant, mais la règle ne le dit pas.

**Correction proposée :** dans AD-3 : « tout offset de structure du jeu vit dans `game/` sous une constante nommée, et n’est valable que pour l’empreinte vérifiée ». Le code respecte déjà cela (`kAnimationPropertySet`, `read_motion_resource` dans `game/`).

---

## Grille de la rubrique

| Critère | État | Commentaire |
| --- | --- | --- |
| Fixe les vrais points de divergence de J1–J2, n’en oublie aucun | Partiel | F1 (animation PNJ), F3 (retard/volume de `Motion`), F4 (canal hook → frame), F5 (contrat `AniId`) |
| Chaque Rule est applicable et empêche la divergence annoncée | Oui pour AD-2, AD-7, AD-10, AD-11 ; partiel pour AD-8 | AD-8 : « Prevents : deux encodages concurrents » non tenu pour les PNJ (F1) |
| Rien dans Deferred ne permet une divergence | Oui | Le Deferred n’a pas changé ; l’équipement reste confié à la story FR-6 |
| Tech nommée vérifiée et actuelle | Inchangé | Pas de nouvelle dépendance. KCP n’est pas encore vendorisé (`client/third_party/` ne contient que `minhook/`) ; voir `review-versions.md` |
| Ratifie le code existant | Oui, avec deux points périmés | F8 |
| Chaque dimension de l’altitude est décidée, différée ou en question ouverte | Presque | Animation PNJ (F1) et thread du hook (F4) ne sont ni décidés, ni différés, ni ouverts |

## Priorités

1. F1 et F2 avant la story avatar (J1) — F2 bloque la story avatar, F1 la story `sync/npc`.
2. F3, F4, F5 dans la même passe sur AD-8 / AD-2 : quelques phrases chacune.
3. F6 à F9 : hygiène, à faire au passage.

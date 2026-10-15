---
type: review
lens: rubric
target: ARCHITECTURE-SPINE.md (J1–J2)
date: 2026-10-15
verdict: needs-revision
---

# Revue rubrique — Architecture Spine Risen Multiplayer (J1–J2)

**Verdict : à réviser avant de découper les stories.** La spine est solide sur le fond (paradigme clair, couches, règle de thread, NetId, liste fermée d'événements, machine à états de session). Mais elle laisse ouverts plusieurs points de divergence réels entre stories J1/J2, contient un défaut de protocole (poignée de main KCP), contredit sur quelques points le code J0 existant sans le dire, et n'aborde presque pas l'enveloppe opérationnelle.

Sévérité : **B** = bloquant (deux stories divergeront ou le protocole est faux), **M** = majeur, **m** = mineur.

---

## 1. Points de divergence pour le niveau inférieur

### B1 — Poignée de main impossible : `conv` KCP tiré par l'hôte, mais `Hello` est le premier message fiable de l'invité (AD-4 × AD-5)
AD-4 : « `conv` = identifiant de session tiré par l'hôte ». AD-5 : « le premier message fiable de l'invité est un `Hello` ». L'invité doit créer son `ikcpcb` avec un `conv` **avant** d'envoyer `Hello`, donc avant d'avoir reçu quoi que ce soit de l'hôte. Deux stories (net côté hôte, net côté invité) vont résoudre ce paradoxe différemment.
**Fix :** choisir une règle, par exemple : « `Hello`/`Welcome`/`Reject` circulent hors KCP sur un canal `2` = contrôle (non fiable, renvoyé toutes les 500 ms jusqu'à réponse, 5 tentatives) ; `Welcome` porte le `conv` ; KCP n'est créé qu'après `Welcome`. » Ou bien : « l'invité tire le `conv` et l'envoie dans `Hello`, l'hôte l'adopte ». Mettre à jour AD-4, AD-5 et AD-10.

### B2 — PNJ chez l'invité : réutiliser les PNJ natifs de la sauvegarde ou les remplacer ? (AD-6, AD-9, AD-10)
Les deux joueurs chargent la **même sauvegarde** : le jeu de l'invité contient déjà les loups, avec leur propre IA. AD-10 prévoit `Spawn{NetId, type, pose}`, ce qui suggère de créer des répliques neuves, mais rien ne dit ce qu'on fait des PNJ natifs de l'invité (les cacher/supprimer ? les associer à un `NetId` ?). Si on les associe, il faut une clé d'appariement stable commune aux deux jeux (GUID d'entité du monde), que AD-6 ne mentionne pas (« aucun pointeur ne circule » ≠ « comment on retrouve l'entité »). C'est la question ouverte n°3 du PRD, et la spine ne la tranche pas, ne la diffère pas et ne la pose pas comme question ouverte.
**Fix :** ajouter une règle à AD-9 : soit « Appariement : chaque PNJ est identifié par son GUID d'entité Genome ; `Spawn` porte ce GUID ; l'invité associe le `NetId` à son PNJ natif et neutralise son IA ; un PNJ natif absent de la réplication reste gelé/invisible », soit « Remplacement : à l'entrée en session, l'invité désactive tous les PNJ hostiles natifs dans le rayon ; seuls les `Spawn` créent des répliques ». Si ça dépend du spike J2, l'inscrire explicitement en Deferred **avec la contrainte** que le choix est fait une seule fois, dans le spike, avant toute story `sync/npc`.

### B3 — Dégâts des PNJ sur l'invité (FR-11) incompatibles avec l'IA neutralisée (AD-9)
AD-9 dit à la fois : l'IA des répliques est neutralisée chez l'invité, et les dégâts infligés à l'invité sont calculés dans le jeu de l'invité. Or, chez l'invité, une réplique ne fait que jouer une animation pilotée par snapshot ; rien ne garantit que le moteur appliquera des dégâts sans son IA/combat actif. Inversement, chez l'hôte, l'avatar de l'invité **est** une entité du jeu : les loups de l'hôte vont le frapper et le jeu de l'hôte va lui retirer des PV, voire le tuer. Deux stories (avatar J1, combat J2) peuvent trancher différemment.
**Fix :** décider l'émetteur du coup : par exemple « L'hôte détecte le coup d'un PNJ sur l'avatar NetId 2 et envoie un événement fiable `HitOnGuest{source NetId, dégâts}` ; l'invité l'applique à son héros avec la fonction de dégâts du jeu. Chez l'hôte, l'avatar de l'invité est invulnérable (ses PV viennent des snapshots de l'invité). » Ou l'inverse, mais écrit. Ajouter le message à AD-10. À défaut, le déplacer en Deferred (spike J2) en nommant les deux options.

### M1 — Déclencheur de session et de la boucle par frame non décidé (FR-4, FR-5, AD-2, AD-11)
FR-4/FR-5 : « le chargement d'une partie démarre le serveur / déclenche la connexion ». La spine ne dit pas quel événement du jeu marque « partie chargée » (hook dédié ? premier `OnProcess` où `Entity::GetPlayer` est valide ?), ni ce qu'il se passe si on recharge une sauvegarde **pendant** une session (entités détruites par le moteur → table `NetId ↔ entité` pendante). Elle ne dit pas non plus quand les hooks MinHook sont installés (dans `ScriptInit` ? différé ?), ni comment détecter « l'hôte quitte le jeu » pour envoyer `SessionEnd` (FR-8).
**Fix :** ajouter à AD-11 : « `Inactive → Hosting|Connecting` au premier `OnProcess` où `GetPlayer()` renvoie une entité valide ; tout changement de monde/chargement de sauvegarde en session → `Ended` ; hooks installés à la fin de `ScriptInit` si `g_mod_active` ; à la sortie du jeu (hook X ou `DLL_PROCESS_DETACH` côté thread réseau), envoi best-effort de `SessionEnd` ».

### M2 — Équipement de l'invité : différé à une story, mais touche le protocole (Deferred × AD-10)
« Format et application choisis dans la story FR-6 ». Or l'équipement doit apparaître sur l'avatar de l'invité **chez l'hôte** : il doit donc voyager sur le réseau, et AD-10 est une liste fermée dont `Spawn{NetId, type, pose}` n'a pas de champ équipement. Ce différé permet à la story FR-6 de modifier le protocole hors AD-10, ou à deux stories (avatar, session) de l'encoder différemment.
**Fix :** dans AD-10, donner un champ réservé : `Hello{…, equipment: liste de noms de templates u8-préfixés}` (ou `Spawn{…, equipment}`), et ne laisser à la story que la grammaire de `guest_equipment` dans l'ini.

### M3 — Placement initial de l'invité (FR-6) sans message
FR-6 : le **personnage de l'invité** (dans son propre jeu) est placé près de celui de l'hôte. Seul `Respawn` (mort) transporte une pose ; `Welcome` n'en a pas. 
**Fix :** `Welcome{conv, pose de l'hôte}` et règle « à `Welcome`, l'invité téléporte son héros à 2 m de cette pose ».

### M4 — Format du snapshot incomplet vs budget de 1200 octets (AD-4, AD-7)
Pas d'encodage de la rotation (quaternion 16 o ? yaw 4 o ?), pas de taille de PV, pas de règle quand les entités ne tiennent pas dans 1200 o (avec ~38 o/entité, ~30 entités max). Hermite exige une vitesse non transmise. `AniId` seul ne dit pas si l'animation a été **relancée** (deux coups d'épée identiques successifs).
**Fix :** figer dans AD-7 : `pos f32×3, yaw f32` (ou quat), `hp u16`, `AniId u16 + ani_seq u8` ; « si l'état dépasse 1200 o, on découpe en plusieurs datagrammes portant le même `tick` » ou « on garde les N plus proches ». Vitesse dérivée de deux snapshots (ou Hermite remplacé par lerp).

### m1 — Règle de 50 m et départ/arrivée dans le rayon
AD-9 fixe 50 m mais pas l'hystérésis ni l'envoi de `Despawn` quand un PNJ sort du rayon (risque de clignotement Spawn/Despawn). **Fix :** « entrée à 50 m, sortie à 60 m ».

---

## 2. Les règles sont-elles applicables et empêchent-elles la divergence ?

- **AD-2** : applicable (une revue peut vérifier qu'aucune fonction de `game/` n'est appelée hors du hook). OK. Préciser que `mp::log` est le seul service partagé autorisé depuis le thread réseau (il l'est déjà via SRWLOCK).
- **AD-3** : applicable, mais **contredite par le code J0** : `win/script_init.cpp` appelle directement `bCString::bCString` via `win/game_abi`. Voir §4.
- **AD-4** : applicable, sauf B1.
- **AD-5** : « le moindre écart donne un `Reject` » : bon. Préciser que la comparaison de l'empreinte porte sur le couple Game.dll + Engine.dll (comme `version.cpp`) et que la version du mod est comparée en égalité stricte de chaîne (`MP_MOD_VERSION`).
- **AD-7** : « le récepteur … n'attend jamais une retransmission » est vérifiable ; le reste dépend de M4.
- **AD-8** : la clause « si ça saccade, le repli … remplacera cette règle » n'est pas une règle, c'est un déclencheur de révision. Acceptable, mais il faut nommer qui décide et sur quel critère (ex. « mesuré en story FR-7 sur le banc ; décision consignée dans la spine »).
- **AD-9** : voir B2/B3.
- **AD-11** : bonne règle unique pour `Ended`. Incomplète sur les entrées (M1).
- **Couches** : la règle « dépendances vers le bas, `game` et `net` n'utilisent que `core` » est violée dès le départ par `mp::log`, qui vit dans `win/` (couche du haut) mais doit être utilisé par `net/`, `game/` et `sync/` (la convention Log l'exige). **Fix :** déplacer `log` dans `core/` (interface + implémentation de sink Windows injectée) ou déclarer `log` comme exception transverse explicite.

## 3. Le Deferred peut-il faire diverger deux unités ?

| Élément différé | Risque | Avis |
| --- | --- | --- |
| J3–J5 | Aucun pour J1–J2 | OK |
| Template de l'avatar (spike J1) | Local à `game/entity` | OK |
| Neutralisation IA + fonction de dégâts (spike J2) | Conditionne B2/B3 et donc `sync/npc`, AD-10 | **Risque** : ajouter « le spike produit une décision consignée dans la spine avant toute story `sync/npc` » |
| NAT, chiffrement, delta, >2 joueurs | Non-objectifs | OK |
| Équipement (story FR-6) | Touche le protocole | **Divergent** (M2) |
| CI | Aucun | OK |

## 4. Ratifie-t-elle le code J0 existant ?

Globalement oui (config, version, log, ScriptInit, toolchain, CRT statique). Écarts **non mentionnés** :

1. **Espaces de noms** : la convention impose `mp::core`, `mp::game`, `mp::win` ; le code J0 utilise `mp` (config, version), `mp::abi` (ABI) et `mp::log`. **Fix :** soit ratifier `mp::` pour `core/` et `mp::log`, soit inscrire une story de migration.
2. **Adaptateur ABI** : AD-3 dit « seul `game/` appelle le jeu », mais l'ABI J0 vit dans `win/game_abi.{h,cpp}` et `script_init.cpp` appelle le constructeur `bCString`. **Fix :** dire explicitement « `win/game_abi` migre vers `game/` en tête de J1 ; `bCString` y devient un service de l'adaptateur » ou admettre l'exception de `ScriptInit`.
3. **Résolution « tout ou rien » et `g_mod_active`** : compatible avec J0 (`resolve` s'arrête au premier symbole manquant). OK — le dire comme ratification.
4. **`log_level = debug`** : la convention Log s'appuie sur une clé qui n'existe ni dans `core/config.{h,cpp}` ni dans `multiplayer.example.ini`, et `mp::log` n'a pas de niveau `debug`. Aujourd'hui la clé produirait un avertissement « champ inconnu ». **Fix :** l'ajouter à la liste des clés à créer (et `mp::log::debug`).
5. **SEH** : AD-3 « chaque appel au jeu est protégé par SEH » ratifie bien `construct_bcstring`/`destroy_bcstring`. OK.
6. **Documentation du dépôt** : `CLAUDE.md` décrit encore `server/` en Go, `kcp-go` et « premier client connecté = hôte ». AD-1 les supprime (ADOPTED). Le PRD (glossaire « Serveur », FR-4) parle aussi d'un « serveur ». **Fix :** une ligne dans la spine « AD-1 remplace `server/` de CLAUDE.md ; dans le PRD, “serveur” = le thread réseau de la DLL de l'hôte » et mettre CLAUDE.md à jour.

## 5. Couverture FR-4..FR-12

| FR | Couvert par | Trou |
| --- | --- | --- |
| FR-4 Héberger | AD-1, AD-11 | Déclencheur « chargement d'une partie » (M1) |
| FR-5 Rejoindre | AD-1, AD-4, AD-5 | Handshake KCP (B1) ; « hôte injoignable » : pas de délai de connexion défini (timeout 5 s ne vaut qu'en session) |
| FR-6 Apparition | AD-6, AD-10 | Placement (M3), équipement (M2) |
| FR-7 Avatar | AD-7, AD-8 | « disparaît à la déconnexion » : couvert par AD-11. Animations « mort » de l'avatar : OK via AniId |
| FR-8 Fin de session | AD-11 | Détection « quitter le jeu » (M1) |
| FR-9 Répliques | AD-9 | Natifs vs répliques (B2) ; « un monstre peut cibler l'invité » : l'avatar invité chez l'hôte doit être une cible valide → règle d'invulnérabilité (B3) |
| FR-10 Dégâts invité→PNJ | AD-9 `HitRequest` | Qui détecte le coup chez l'invité (la réplique a-t-elle une hitbox active sans IA ?) — à lier au spike J2 |
| FR-11 Dégâts PNJ→invité | AD-9 | Contradiction (B3) |
| FR-12 Mort/réapparition | AD-11 `Respawn` | Comment l'hôte apprend la mort de l'invité : `Death{2}` émis par l'invité ? À écrire (seul l'hôte « attribue » les NetId, mais qui émet `Death` n'est pas dit). Mort de l'hôte → `SessionEnd` : OK |

## 6. Dimensions de l'altitude : décidées, différées ou ouvertes ?

| Dimension | État |
| --- | --- |
| Paradigme, couches, threading | Décidé |
| Protocole, transport, identité | Décidé (défaut B1) |
| Erreurs, config, tests, tiers | Décidé |
| Stack, versions | Décidé |
| **Déploiement / installation** | **Absent.** Le README J0 l'établit (`bin/scripts/Script_Multiplayer.dll` + `multiplayer.ini`), la spine ne le ratifie pas. Rien sur la distribution (archive de release GitHub ? DLL non signée → antivirus/SmartScreen ?), ni sur le fait que hôte et invité doivent avoir la **même build** (conséquence d'AD-5). |
| **Environnements Windows / Proton** | **Partiel.** La ligne « Cible » cite Windows et Proton, mais rien sur le pare-feu Windows (UDP entrant 27960 côté hôte), la redirection de port/VPN (seulement un diagramme), Winsock sous Wine/Proton, ni sur l'hôte sous Proton derrière le NAT Linux. |
| **Ops / diagnostics** | **Partiel.** Log par sous-système : oui. Manquent : ce que le log doit contenir pour diagnostiquer une session (adresse du pair, RTT, perte, raison de fin), un résumé périodique hors `debug` (ex. toutes les 30 s), et la conduite après une mise à jour Steam (déjà dans le README J0 — à ratifier). |
| Questions ouvertes | **Aucune section.** Les incertitudes (spikes, B2, PRD Q3) sont dispersées dans Deferred et `[ASSUMPTION]`. |

**Fix :** ajouter une section « Enveloppe opérationnelle » (installation = 2 fichiers dans `bin/scripts/`, même version des deux côtés, port UDP à ouvrir, Windows/Proton testés sur banc Wine + jeu réel, log de session minimal : connexion, RTT/perte toutes les 30 s, raison de fin) et une section « Questions ouvertes » (B2, B3, PRD Q3, critère de repli d'AD-8).

---

## Synthèse des corrections prioritaires

1. **B1** — Réécrire la poignée de main : contrôle hors KCP ou `conv` choisi par l'invité.
2. **B2 + B3** — Trancher (ou différer avec contrainte d'ordre) l'appariement PNJ natifs/répliques et l'émetteur des dégâts PNJ→invité ; invulnérabilité de l'avatar invité chez l'hôte ; ajouter les messages à AD-10.
3. **M1** — Déclencheurs d'AD-11 (partie chargée, rechargement en session, sortie du jeu, installation des hooks).
4. **Code J0** — Ratifier ou planifier la migration : namespaces, `win/game_abi` → `game/`, `log` hors de `win/`, clé `log_level`.
5. **Enveloppe opérationnelle + Questions ouvertes** — nouvelles sections ; équipement et placement initial dans le protocole (M2, M3).

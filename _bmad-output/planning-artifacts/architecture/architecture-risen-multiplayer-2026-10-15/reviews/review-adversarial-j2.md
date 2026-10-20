---
type: review
lens: adversarial
target: ARCHITECTURE-SPINE.md (J1–J2, mise à jour post-spike J2 du 2026-10-20)
focus: AD-2, AD-3, AD-8, AD-9, AD-10, AD-11, questions ouvertes 3 à 10 — Epic 3 (stories 3.1 à 3.6)
date: 2026-10-20
verdict: needs-revision
---

# Revue adversariale — spine Risen Multiplayer après le spike J2

**Verdict : à resserrer avant de découper les stories 3.3 à 3.6.** Les décisions issues du spike sont bonnes sur le *quoi* (GUID, hook de routine, `DoDamage`, coup mortel annulé dans `DownOrDeadBy*`, corps cinématique). Elles laissent ouverts trois points : le *contrat de données* d'un PNJ (`Spawn`), le *propriétaire unique* des hooks de combat et de l'ensemble des répliques, et le *chemin unique* de mutation des PV et de la mort. On peut écrire deux stories de l'Epic 3 qui respectent chaque AD à la lettre et qui ne s'emboîtent pas. Six trous sont bloquants ou majeurs.

Méthode : pour chaque trou, deux unités du niveau inférieur (story, module `sync/npc` contre `sync/avatar`, ou hôte contre invité) conformes à la lettre, puis la divergence concrète et la règle qui la ferme. Les faits cités viennent de la spine, de `protocol/PROTOCOL.md` (v6), de `docs/re/spike-j2.md` et du code actuel (`client/src/sync/anim_filter.h`, `client/src/win/hooks.cpp`, `client/src/sync/session.cpp`).

Gravité : **B** = bloquant (incompatibilité binaire, crash ou comportement faux garanti), **M** = majeur (divergence probable, coûteuse à rattraper), **m** = mineur.

---

## B1 — Le `Spawn` d'un PNJ n'a pas de forme : AD-9 et AD-10 se contredisent (AD-6, AD-9, AD-10, Q10)

**Textes en jeu.** AD-9 : le GUID « voyage dans le `Spawn` du PNJ, avec son `NetId` ». AD-10 : `Spawn{NetId, type, pose}`. `PROTOCOL.md` v6 : `Spawn` fait **22 octets fixes**, `type` 0 seulement, et « chaque pair n'accepte que le `Spawn` du `NetId` de l'autre, de `type` 0 ». Q10 : les PNJ créés en cours de partie n'ont pas de GUID commun, et leur repli est « à définir dans la story 3.3 ».

**Unité A — story 3.3, côté hôte (`sync/npc`).** Elle ajoute `type = 1` (PNJ) et ajoute le GUID (16 octets) après la pose : `Spawn` passe à 38 octets pour ce type. Conforme à AD-9.
**Unité B — story 3.3, côté invité, ou le faux invité Go.** Elle lit AD-10 à la lettre : `Spawn{NetId, type, pose}`, longueur fixe de 22 octets. Elle refuse le tampon trop long, ou ignore la queue. Conforme à AD-10.
**Unité C — repli Q10 décidé dans 3.3.** Pour un PNJ sans GUID apparié, une variante crée une copie chez l'invité (`Entity::Spawn(template)`). Il faut alors **aussi** le nom du template dans `Spawn`, donc une chaîne de longueur variable. Une autre variante l'ignore : l'invité est frappé (`HitOnGuest{source}`) par un `NetId` qu'il n'a pas, et la source est invisible.

**Divergences supplémentaires.**
- Sortie puis retour dans le rayon de 50 m : le même GUID reçoit-il le **même** `NetId` ? Si l'hôte en tire un neuf, l'invité a deux `NetId` pour une même entité locale tant que le `Despawn` n'est pas traité, et la table `NetId ↔ entité` d'AD-6 n'est plus bijective.
- Une règle de v3 (« un `Spawn` reçu alors que l'entité existe est ignoré ») appliquée aux PNJ masque un PNJ réapparu sous un autre `NetId`.

**Règle proposée (AD-9 + AD-10 + PROTOCOL v7, avant 3.3).**
- `Spawn` devient un message à **variante par `type`**, décrite dans `PROTOCOL.md` : `type 0` = héros (22 o, inchangé) ; `type 1` = PNJ apparié `{NetId, 1, pose, guid[16]}` ; `type 2` = PNJ non apparié, réservé pour le repli Q10 `{NetId, 2, pose, template str8}`, ou explicitement **non supporté en J2** (l'hôte ne réplique alors pas ce PNJ et le journalise). La décision Q10 est prise **dans la spine avant 3.3**, pas dans la story.
- AD-6 : un `NetId` de PNJ est **lié à son GUID pour toute la session** (table GUID → NetId chez l'hôte, jamais réattribuée). Un `Spawn` d'un `NetId` déjà connu est idempotent ; un `Spawn` d'un GUID déjà apparié sous un autre `NetId` est une erreur de protocole (journalisée, ignorée).
- Les vecteurs `spawn_npc.bin` (et `spawn_npc_template.bin` si le type 2 est retenu) sont ajoutés, vérifiés par C++ et Go.

---

## B2 — Deux propriétaires pour les hooks de combat, et un ensemble de répliques qui n'a pas de clé commune (AD-2, AD-3, AD-9, AD-11, Q8)

**Textes en jeu.** AD-2 : les détours de `DownOrDeadBy*` sont une exception au thread unique ; le hook de routine « ne fait que comparer le PS reçu à l'ensemble des répliques ». AD-9 : « l'ensemble des répliques vit dans le même filtre que celui d'AD-2 ». AD-11 : les détours de `DownOrDeadBy*` agissent quand la victime est **son héros** (invité). Q8 : l'invulnérabilité de l'avatar chez l'hôte se fera peut-être dans `DownOrDeadBy*` (victime = avatar), ou dans `gCDamage_PS::DoDamage`, ou dans `OnDamage`. Le choix est laissé à la story 3.5.

### B2a — `DownOrDeadBy*` : deux stories, un même détour

**Unité A — story 3.6 (invité).** Elle installe les trois détours dans `install_session` : victime == héros local → 1 PV, rend 0, `Death{2}`. Sinon, elle appelle l'original.
**Unité B — story 3.5 (hôte), qui choisit le candidat « `DownOrDeadBy*` » de Q8.** Elle installe **aussi** les trois détours : victime == avatar invité → rend 0 sans appeler l'original. Sinon, elle appelle l'original.
**Divergence.** `MH_CreateHook` sur une cible déjà accrochée échoue (`MH_ERROR_ALREADY_CREATED`). Selon l'ordre d'installation, le mod devient inactif en tout ou rien (AD-3), ou l'une des deux règles disparaît en silence. Si les deux stories passent par une fonction de détour commune écrite deux fois, l'ordre des tests (héros ? avatar ?) n'est pas fixé. Le code actuel pose déjà ces détours dans `install_probe_j2` : la sonde devient un troisième propriétaire, et seule l'exclusivité du détour `PlayMotion` est écrite (AD-8).

### B2b — `gCDamage_PS::DoDamage` : le même hook pour deux besoins opposés

Aucune règle ne dit **où** l'invité détecte son propre coup pour produire `HitRequest` (AD-9 ne nomme que le message).
**Unité A — story 3.4 (invité).** Elle accroche `gCDamage_PS::DoDamage`. Si la cible est une réplique, elle émet `HitRequest` et **n'appelle pas** l'original (AD-9 : l'invité ne modifie jamais une réplique).
**Unité B — story 3.5 (hôte), candidat « `DoDamage` » de Q8.** Elle accroche la **même** fonction. Si la cible est l'avatar invité, elle émet `HitOnGuest` et n'appelle pas l'original.
**Divergence.** Même collision MinHook qu'en B2a. Pire : le `DoDamage` qu'appelle l'hôte pour **appliquer** un `HitRequest` (AD-9) repasse par le détour de 3.5. Si la cible de ce `HitRequest` était l'avatar (cas impossible, mais non interdit par le décodeur), on a une boucle. Il faut une garde de « ré-entrée volontaire », à la manière du `ReplayGuard`.

### B2c — L'« ensemble des répliques » n'a pas de clé partagée

`sync::AnimFilter` range des pointeurs de **`eCAnimation_PS`** (PS `0x52`, `game/entity.h`). Le hook de routine reçoit un **`gCScriptRoutine_PS`** (PS `0x24`). Les détours `DownOrDeadBy*` reçoivent des **`Entity*`** (`eCDynamicEntity*`).
**Unité A — story 3.3.** Elle ajoute à `AnimFilter` un second vecteur, `routine_replicas_`, rempli par `sync/npc`.
**Unité B — story 3.5 ou 3.6.** Elle ajoute un troisième ensemble, d'`Entity*` cette fois, alimenté par `sync/avatar`.
**Divergence.** Trois ensembles, deux modules pour les alimenter. La règle « une entité est retirée du filtre **avant** d'être oubliée » (AD-2) n'est tenue que pour celui que chaque module connaît. Un PNJ retiré de l'ensemble d'animation mais pas de l'ensemble de routine reste figé après la fin de session (son IA ne repart jamais en solo). Un avatar détruit dont l'`Entity*` reste dans l'ensemble des détours finit par une comparaison sur un pointeur recyclé.

**Règle proposée (nouvel AD-12 « Registre des entités répliquées et hooks de combat »).**
- Un **seul** objet partagé entre hooks et frame : `sync::ReplicaRegistry` (successeur d'`AnimFilter`, sous le même verrou). Chaque entrée = `{Entity*, anim_ps, routine_ps, rôle: HérosLocal | Avatar | PNJ}`. Elle est enregistrée et retirée **d'un seul appel** par le seul propriétaire de l'entité (`sync/avatar` pour l'avatar, `sync/npc` pour les PNJ). Chaque hook interroge le registre avec la clé qu'il reçoit.
- **Un hook = un détour = un propriétaire** : `win/hooks` installe chaque cible une seule fois, quel que soit le rôle. Le détour délègue à une fonction unique de `sync/combat_hooks`, qui aiguille sur `(rôle de la victime, rôle de la session)` : héros local de l'invité → annuler le coup mortel ; avatar chez l'hôte → invulnérable + `HitOnGuest` ; réplique de PNJ chez l'invité → `HitRequest`, original bloqué ; sinon → original. La liste des cibles de détour est **fermée** dans la spine (`PlayMotion`, `SetRagDollEnabled`, locomotion EMotionFX, `gCScriptRoutine_PS::OnProcess`, `DownOrDeadBy*` ×3, et au plus un hook de dégâts choisi **dans la spine**).
- **Q8 est tranchée dans la spine avant 3.4 et 3.5**, pas dans 3.5 : 3.4 a besoin du même point d'accroche.
- La sonde (`probe = 1`) et la session sont exclusives pour **tous** ces détours, pas seulement pour `PlayMotion`.
- Les appels au jeu faits par le mod lui-même pour appliquer un `HitRequest` passent sous une garde `thread_local` (comme `ReplayGuard`), que le détour de dégâts laisse passer.

---

## B3 — Les PV et la mort du héros invité ont trois chemins d'écriture (AD-9, AD-11, Q8, Q9)

**Textes en jeu.** AD-9 : l'invité applique `HitOnGuest` « à son propre héros ». AD-11 : le détour `DownOrDeadBy*` remet le héros à 1 PV, puis « l'invité émet `Death{2}` ». `PROTOCOL.md` v5 et `session.cpp` : `Death` part quand `Entity::IsDead` passe à 1.

**Unité A — story 3.5 (invité).** Elle applique `HitOnGuest` par `SetHitPoints(-dégâts, ajout)`. Les PV peuvent passer à 0 ou en dessous. Aucun `DownOrDeadBy*` n'est appelé, puisque le coup ne vient pas d'une arme native : le détour de 3.6 ne voit rien.
**Unité B — story 3.6 (invité).** Elle émet `Death{2}` depuis le détour, et seulement de là. Le code v5 garde l'émission sur `IsDead`.
**Divergence.**
1. Un `HitOnGuest` mortel laisse un héros à 0 PV, vivant pour le moteur, sans `Death` : ni réapparition, ni fin.
2. `IsDead` ne passe jamais à 1 (la mort native est interdite). La seconde source de `Death` est morte, mais une story qui lit `PROTOCOL.md` v5 la garde : chute ou noyade (Q9) produisent `Death` par `IsDead` **et** un menu de chargement, car le héros est vraiment mort, donc session finie par rechargement.
3. **Dégâts natifs des répliques.** Chez l'invité, une réplique de PNJ rejoue ses `Motion` d'attaque (AD-8) et peut garder son arme en main. Si le moteur résout les coups par l'animation et la collision de l'arme (comme pour l'avatar au spike, frappé par des PNJ figés ou non), le héros invité reçoit **aussi** des dégâts natifs, en plus de `HitOnGuest` : dégâts doublés. 3.3 ne voit pas le problème (elle ne touche pas aux PV), et 3.5 non plus (elle n'ajoute que `HitOnGuest`).
4. Après l'annulation à 1 PV, l'invité reste debout et ciblé. Les coups mortels suivants repassent dans le détour : `Death{2}` part plusieurs fois avant le `Respawn`.

**Règle proposée (AD-11 resserré, « PV et mort du héros invité »).**
- **Un seul écrivain des PV du héros invité** : `sync/guest_health` (chez l'invité). Toute baisse passe par lui (`HitOnGuest`, et le détour du coup mortel, qui lui délègue). Il borne à 1 PV et déclenche la mort **lui-même** quand un coup y mène.
- **Les dégâts natifs reçus par le héros invité sont bloqués** (au point d'accroche choisi en B2), sauf ceux que la spine liste explicitement comme locaux (chute, noyade : décision Q9 **avant** 3.6, soit convertis en mort mod, soit laissés natifs et alors fin de session assumée).
- **`Death{2}` a une seule source** : la transition `Vivant → À terre` de `sync/guest_health`, une fois par mort. L'émission sur `IsDead` est retirée pour le héros invité (PROTOCOL v7). Entre `Death` et `Respawn`, l'invité est dans l'état `À terre` : invulnérable localement, aucun nouveau `Death`.

---

## B4 — Les PV de l'avatar invité chez l'hôte : invulnérabilité, snapshot et détection des coups se marchent dessus (AD-7, AD-9, Q8)

**Textes en jeu.** AD-7 : l'invité envoie l'état complet de son avatar, PV compris. AD-9 : chez l'hôte, l'avatar est « invulnérable ». Q8 : détection de chaque coup reçu, point d'accroche non tranché.

**Unité A — `sync/avatar`, côté hôte, prolongement de 2.3.** Pour afficher la jauge, elle copie à chaque frame le `pv` du snapshot dans l'avatar (`SetHitPoints(pv, fixer)`). C'est conforme : `sync/avatar` est seul propriétaire de l'avatar (AD-8).
**Unité B — story 3.5, côté hôte.** Elle détecte un coup reçu par la **baisse des PV** de l'avatar d'une frame à l'autre (aucun hook à poser), émet `HitOnGuest{source, delta}` puis remet les PV au maximum (invulnérabilité).
**Divergence.** Chaque snapshot qui baisse les PV (l'invité a pris un `HitOnGuest`) est vu par B comme un nouveau coup : `HitOnGuest` en boucle, dégâts amplifiés. Inversement, la remise au maximum par B est écrasée par A à la frame suivante. Si B choisit plutôt `DownOrDeadBy*` (coup mortel seulement), les coups non mortels ne sont **jamais** convertis en `HitOnGuest`.

**Autre divergence : le montant.** `HitOnGuest{dégâts}` ne dit pas s'il s'agit de dégâts **bruts** (arme du PNJ) ou **finaux** (après l'armure de l'avatar chez l'hôte). L'avatar est un `Spawn` du template du héros **de l'hôte** (AD-8), avec l'équipement de 3.1. Son armure n'est pas celle du héros invité. Une story calcule après l'armure de l'avatar, l'autre réapplique l'armure du héros invité : protection comptée deux fois ou jamais.

**Règle proposée (AD-9 resserré).**
- Chez l'hôte, les PV de l'avatar invité sont **affichage seulement**, écrits par `sync/avatar` à partir du snapshot. Aucune détection de coup ne lit les PV. Un coup sur l'avatar est détecté **au point d'accroche des dégâts** (B2), qui bloque l'original, d'où l'invulnérabilité sans écriture de PV.
- `HitOnGuest.dégâts` = **dégâts bruts** de l'attaque, avant toute protection. L'invité applique sa propre protection, ou la spine dit explicitement « pas d'armure en J2 ». Même règle pour `HitRequest.dégâts`, voir M1.

---

## M1 — Coups de l'invité : dégâts natifs de l'avatar plus `HitRequest`, donc comptés deux fois (AD-8, AD-9, stories 3.2 et 3.4)

**Unité A — story 3.2 (avatar, côté hôte).** Arme en main (`CreateItems` + `HoldRightStack`), rejeu des `Motion` d'attaque par nom. Au spike, l'avatar armé est une entité de combat complète. Si la résolution du coup suit l'animation et la collision de l'arme, l'épée de l'avatar **blesse nativement** les PNJ de l'hôte à chaque attaque rejouée.
**Unité B — story 3.4 (`HitRequest`).** Chaque coup détecté chez l'invité arrive par `HitRequest` et est appliqué par `DoDamage(arme de l'avatar, cible, avatar)`.
**Divergence.** Deux coups par coup. Et si les deux positions diffèrent (interpolation à 150 ms), le coup natif peut toucher un autre PNJ que celui visé par l'invité.

**Autre divergence.** `HitRequest{NetId, dégâts}` : AD-9 l'applique par `DoDamage`, qui **calcule** ses dégâts depuis le PS de l'arme de l'avatar (`(+0x40 + +0x44) × +0x48`, spike J2). Le champ `dégâts` est-il ignoré, ou imposé par `SetDamageAmount` puis restauré (comme la sonde) ? Si l'arme de l'avatar (3.1, depuis la config) diffère de celle que tient vraiment le héros invité, les deux lectures donnent des PV différents.

**Règle proposée.**
- Chez l'hôte, **les dégâts natifs infligés par l'avatar invité sont bloqués** (même point d'accroche que B2, rôle de l'agresseur = avatar). La seule source de dégâts de l'avatar est `HitRequest`, appliqué sous la garde de B2.
- La spine fixe le sens de `HitRequest.dégâts` : soit « indicatif, journalisé, `DoDamage` calcule », soit « imposé par `SetDamageAmount` puis restauré ». Elle fixe aussi que l'arme de l'avatar est **celle que l'invité tient** (transmise), pas seulement celle de la config.

---

## M2 — `Death`, `Respawn` et `Spawn` : trois cycles de vie pour un même avatar (AD-8, AD-10, AD-11, PROTOCOL v5)

**Textes en jeu.** PROTOCOL v5 : à réception de `Death`, l'avatar passe en ragdoll, puis n'est plus déplacé ni animé « jusqu'à son retrait ; un `Spawn` suivant lève cet état », mais « un `Spawn` reçu alors que l'avatar existe est ignoré ». AD-11 : l'invité ne meurt plus nativement ; l'hôte répond `Respawn` près de son avatar. AD-8 : `Spawn` et `Respawn` réinitialisent l'interpolation. AD-7 : l'invité a l'autorité sur la pose de son avatar.

**Unité A — story 3.6, côté hôte (`sync/avatar`).** Elle garde le comportement v5 : `Death{2}` → avatar en ragdoll, figé. Elle envoie `Respawn{2, pose}` et réinitialise l'interpolation de l'avatar **à l'envoi**.
**Unité B — story 3.6, côté invité.** À `Respawn`, elle téléporte son héros (`MoveTo`) et remet ses PV au maximum. Ses snapshots repartent de la nouvelle pose.
**Divergences.**
1. Chez l'hôte, rien ne sort l'avatar de son état « mort » : `Respawn` est un message **envoyé** par l'hôte, pas reçu, et le `Spawn` qui lèverait l'état est ignoré parce que l'avatar existe. L'avatar reste en ragdoll pour toujours.
2. Les snapshots de l'invité encore en vol, avec la pose de la mort, arrivent **après** la réinitialisation faite à l'envoi : l'avatar revient sur le lieu de la mort, puis Hermite le fait glisser jusqu'au point de réapparition.
3. Avec AD-11 (pas de ragdoll natif chez l'invité, héros debout à 1 PV), mettre l'avatar en ragdoll chez l'hôte est un état qui n'existe pas chez l'invité.

**Règle proposée (AD-11 + PROTOCOL v7).**
- Machine à états de l'avatar, **la même des deux côtés** : `Vivant → À terre (Death) → Vivant (Respawn appliqué)`. La spine dit ce que fait `Death` sur l'avatar (ragdoll ou animation « à terre » rejouée), et le texte v5 sur le ragdoll est remplacé.
- `Respawn` est **accusé** : l'invité, après l'avoir appliqué, renvoie `Spawn{2, 0, nouvelle pose}`, ou porte une **époque de vie** (u8) dans ses snapshots. L'hôte ne quitte `À terre` et ne réinitialise l'interpolation qu'à cet accusé, et il ignore les snapshots d'époque antérieure. La règle « `Spawn` d'un avatar existant ignoré » devient « `Spawn` d'un avatar existant = réapparition ».

---

## M3 — « Supprime les répliques » à la fin de session : des PNJ natifs détruits, ou figés pour toujours (AD-9, AD-11, `Despawn`)

**Textes en jeu.** AD-11 : la transition vers `Ended` « supprime les avatars et les répliques, puis rend la main au jeu solo ». AD-9 : chez l'invité, les répliques sont les **PNJ natifs** de sa sauvegarde, appariés par GUID. PROTOCOL : `Despawn` = « retrait d'une entité ».

**Unité A — story 3.3, lecture littérale d'AD-11.** À `Ended` (et à `Despawn`), elle traite la réplique comme l'avatar : retirée du filtre, puis **détruite**. Le monde solo de l'invité perd ses monstres. Un `Spawn` ultérieur du même GUID (retour dans les 50 m) ne trouve plus l'entité (`GetEntity` → nul).
**Unité B — même story, autre lecture.** Elle retire seulement l'entité du filtre de routine, sans rien d'autre. Le PNJ reprend son IA avec les PV, la position et peut-être l'état « mort en ragdoll » imposés par la réplication. Un PNJ tué chez l'hôte (`Death` → ragdoll chez l'invité, sans `Kill` natif) **se relève** en solo avec ses PV d'avant.
**Divergence connexe.** AD-9 dit que l'invité ne modifie « jamais l'état d'une réplique », mais aussi que ses PV « viennent des snapshots ». Une story n'écrit pas les PV de la réplique (la jauge ne baisse pas chez l'invité), l'autre les écrit par `SetHitPoints`.

**Règle proposée (AD-9 resserré).**
- Les répliques de PNJ chez l'invité ne sont **jamais détruites** par le mod. `Despawn` et `Ended` les **libèrent** : retrait du registre (B2), IA rendue, plus aucune écriture.
- Seul `sync/npc` écrit l'état d'une réplique (pose, PV depuis le snapshot, animation, mort) ; c'est le sens de « l'invité ne modifie jamais » (aucun **autre** chemin : ni IA, ni dégâts natifs, ni joueur).
- La mort d'une réplique chez l'invité est **définitive et native** : la spine choisit le chemin (`Kill` natif sous garde, sans XP ni butin pour l'invité, ou PV 0 + ragdoll + état de mort), pour qu'une réplique tuée reste morte une fois libérée.

---

## m1 — AD-10 a oublié `MotionParam`

La liste fermée d'AD-10 ne contient pas `MotionParam` (26), défini en v6 dans `PROTOCOL.md` et requis par la locomotion des PNJ (epic-3-context). Une story `sync/npc` qui lit AD-10 à la lettre ne l'émet pas pour les PNJ ; une autre, qui lit le protocole, l'émet. **Règle :** ajouter `MotionParam` à AD-10, et indiquer qu'il s'étend aux PNJ.

## m2 — Le budget de `Motion` des PNJ et l'espace des `AniId` ne sont pas fixés

AD-8 exige que le budget soit fixé « avant toute story `sync/npc` », et Q3 le laisse ouvert alors que 3.3 en est une. Par ailleurs, « chaque émetteur a son propre espace d'`AniId` » : émetteur = pair, ou entité ? Si 3.3 tient un espace par PNJ et le récepteur un espace par pair, les `AniId` se mélangent. **Règle :** un espace d'`AniId` **par pair**, partagé par toutes ses entités ; budget chiffré (débit max de `Motion` + `MotionParam` par PNJ et au total, politique de rejet : par exemple seuls les PNJ des N plus proches émettent leur locomotion).

## m3 — `Script_Game.dll` dans un « tout ou rien » (AD-3) rend le banc Wine inutilisable

AD-3 : un symbole manquant laisse le mod inactif ; l'empreinte couvre `Script_Game.dll`. Le spike : sur le banc Wine, `Script_Game.dll` est absente. Une story 3.6 qui ajoute `DownOrDeadBy*` à la résolution en bloc rend le mod inactif sur le banc, et tous les scénarios `run_scenarios.sh` échouent. Une autre la résout à part (facultatif, comme la sonde), et la mort de l'invité redevient native sans bruit. **Règle :** les symboles de `Script_Game.dll` sont **obligatoires en session invité réelle** (absence = `Reject` ou mod inactif, journalisé) et **dispensés** sur le banc, par un drapeau explicite, pas par tolérance silencieuse. Préciser aussi que l'ordre de chargement (`Script_Game` avant `Script_Multiplayer`) est une hypothèse vérifiée au démarrage.

## m4 — Format de `guest_equipment` et arme de l'avatar (3.1 contre 3.2)

`Hello.équipement` est une `str8` sans format. 3.1 peut y mettre une liste (`It_1H_Sword;It_Armor_…`) et tout équiper. 3.2 tient en main « la première pile » ou « l'objet de type arme ». L'arme réellement tenue par l'invité peut changer en session sans que rien ne le transmette (le `Hello` ne part qu'une fois). **Règle :** fixer le format dans la spine (liste de templates séparés par `;`, première arme = arme en main), et dire si un changement d'arme en session est hors périmètre J2 (journalisé) ou transmis par un événement.

## m5 — PNJ natifs non répliqués chez l'invité

AD-9 ne neutralise l'IA **que** des répliques. Avant le premier `Spawn`, et au-delà de 50 m pour un PNJ que l'hôte ne réplique pas encore, les monstres de l'invité vivent leur IA native, peuvent le frapper (dégâts natifs, voir B3) ou être tués chez lui seul. **Règle :** chez l'invité en session, les PNJ hostiles **non** répliqués sont figés aussi, ou les dégâts natifs entre le héros invité et tout PNJ sont bloqués (B3), et la spine choisit.

---

## Synthèse des resserrements

| # | Gravité | AD touchés | Règle |
|---|---|---|---|
| B1 | B | AD-6, AD-9, AD-10, Q10 | `Spawn` à variante par `type` (PNJ = GUID), `NetId` lié au GUID pour la session, Q10 tranchée dans la spine, vecteurs |
| B2 | B | AD-2, AD-3, AD-9, AD-11, Q8 | AD-12 : registre unique des entités répliquées (toutes les clés de PS), un détour par cible, aiguillage par rôle, liste fermée des hooks, Q8 tranchée avant 3.4 et 3.5 |
| B3 | B | AD-9, AD-11, Q9 | Un seul écrivain des PV du héros invité, dégâts natifs bloqués, une seule source de `Death{2}`, état `À terre` |
| B4 | B | AD-7, AD-9, Q8 | PV de l'avatar chez l'hôte = affichage seulement ; coups détectés au hook de dégâts ; dégâts bruts dans `HitOnGuest` |
| M1 | M | AD-8, AD-9 | Dégâts natifs de l'avatar bloqués chez l'hôte ; sens de `HitRequest.dégâts` fixé ; arme de l'avatar = arme tenue |
| M2 | M | AD-8, AD-10, AD-11 | Cycle de vie commun de l'avatar ; `Respawn` accusé (époque de vie) ; texte v5 sur le ragdoll remplacé |
| M3 | M | AD-9, AD-11 | Répliques jamais détruites, libérées à `Despawn`/`Ended` ; seul `sync/npc` les écrit ; mort de réplique définitive |
| m1–m5 | m | AD-3, AD-8, AD-10, Deferred | `MotionParam` dans AD-10 ; budget et espace `AniId` ; `Script_Game` sur le banc ; format d'équipement ; PNJ non répliqués |

**Ordre conseillé :** B2 (registre et hooks) et B1 (`Spawn` v7) avant la story 3.3. B3, B4 et M1 avant 3.4 et 3.5, avec Q8 et Q9 tranchées dans la spine. M2 et M3 avant 3.6.

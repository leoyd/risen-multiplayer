---
type: review
lens: rubric
target: ARCHITECTURE-SPINE.md (J1–J2), mise à jour post-spike J2 (2026-10-20)
scope: AD-2, AD-3, AD-8, AD-9, AD-10, AD-11, Open Questions, Deferred + cohérence avec le memlog, l’Epic 3 et le code
date: 2026-10-20
verdict: needs-revision
---

# Revue rubrique — mise à jour post-spike J2

**Verdict : mise à jour fidèle au spike (toutes les décisions du memlog du 2026-10-20 ont atterri, sauf le budget `Motion` des PNJ), mais elle laisse ouverts, sans les nommer, les deux points où les stories 3.3, 3.4 et 3.5 divergeront le plus sûrement : comment l’invité détecte et neutralise les coups sur une réplique (et sur son héros), et comment il applique PV et mort aux répliques. Plusieurs textes anciens contredisent aussi les nouvelles règles ou le code (AD-10, AD-2, `HitRequest`). À corriger avant de détailler les stories 3.3 à 3.6.**

Sources lues : la spine (diff vs `HEAD`), les 15 dernières lignes de `.memlog.md`, `docs/re/spike-j2.md` (résultats + conclusions), `_bmad-output/implementation-artifacts/epic-3-context.md`, et le code `client/src/` (`core/protocol.h`, `core/session.{h,cpp}`, `sync/anim_filter.h`, `sync/probe_j2.cpp`, `game/combat.{h,cpp}`, `win/hooks.cpp`).

## 1. Report du memlog dans la spine

| Entrée du 2026-10-20 | Atterrie ? |
| --- | --- |
| AD-9 Q3 appariement GUID / `GetEntity` | Oui (AD-9, « Appariement ») |
| AD-9 Q3 IA par hook de routine, `EnableClassProcessing` interdit | Oui (AD-9, « IA ») |
| AD-9 Q3 `DoDamage`, jamais `SetHitPoints` sur un PNJ | Oui, mais formulé de façon trop générale (voir F4) |
| AD-9 Q5 arme `CreateItems` + `HoldRightStack` | Oui |
| AD-8/AD-9 Q6 avatar cinématique | Oui (AD-8, « Placement ») |
| AD-11 Q7 interception `DownOrDeadBy*` | Oui |
| AD-2 exception détours de script | Oui |
| AD-3 `Script_Game.dll` dans les modules et l’empreinte | Oui, sans la conséquence protocole (voir F7) |
| Questions 3.5 / 3.6 / 3.3 | Oui (OQ 8, 9, 10) |
| Hypothèse « rayon 50 m et budget `Motion` PNJ inchangés » | **Partiellement** : le rayon garde son `[ASSUMPTION]` ; le budget n’est fixé nulle part et n’a plus de question ouverte qui le porte (voir F5) |

## 2. Constats

### F1 — Détection et neutralisation des coups côté invité : point de divergence non nommé — **Haute**

AD-9 exige que l’invité « ne modifie **jamais** l’état d’une réplique » et qu’un coup produise `HitRequest`. Mais les répliques sont des **PNJ natifs** : quand l’épée du héros invité touche un Vulture répliqué, le moteur de l’invité applique nativement les dégâts (et éventuellement `DownOrDeadBy*` → `Kill`) sur la réplique. Rien dans la spine ne dit :
- où l’invité **détecte** le coup pour émettre `HitRequest` ;
- comment il **empêche** les dégâts natifs sur la réplique ;
- symétriquement, si une réplique qui rejoue une animation d’attaque (AD-8) peut toucher **nativement** le héros invité, ce qui doublerait les dégâts avec `HitOnGuest`.

Le spike n’a validé que l’application côté hôte (Num5). L’OQ 8 ne couvre que le côté hôte (invulnérabilité de l’avatar, `HitOnGuest`). Les stories 3.4 et 3.5 vont choisir chacune un point d’accroche (`gCDamage_PS::DoDamage`, `OnDamage`, `DownOrDeadBy*`…), potentiellement différent, alors que c’est **le même mécanisme** des deux côtés : « un coup natif entre un joueur et une entité dont ce pair n’a pas l’autorité est bloqué et converti en événement ».

**Correctif :** élargir l’OQ 8 en une seule question « point d’accroche des coups natifs » couvrant les quatre cas (avatar invité chez l’hôte, réplique chez l’invité, héros invité frappé par une réplique, coup mortel), avec une règle de principe déjà posée dans AD-9 : *tout coup natif dont la cible n’est pas sous l’autorité locale est bloqué à un seul point d’accroche, commun à 3.4 et 3.5, et converti en `HitRequest` ou `HitOnGuest`*. Trancher le point d’accroche dans la première des deux stories (3.4), pas en 3.5.

### F2 — Application de `HitOnGuest` et interception de la mort : deux stories qui doivent s’accorder — **Haute**

AD-11 intercepte la mort de l’invité dans `DownOrDeadBy*`. Or ces fonctions ne sont appelées que si les dégâts passent par le pipeline de script natif. Si la story 3.5 applique `HitOnGuest` par `SetHitPoints` (le plus simple), `DownOrDeadBy*` n’est **jamais** appelé et la story 3.6 n’intercepte rien ; si elle l’applique par `DoDamage` sur le héros, l’interception fonctionne mais il faut une arme/agresseur local (la réplique source ?). AD-9 dit seulement que l’invité « applique ces dégâts à son propre héros ».

**Correctif :** fixer dans AD-9 la fonction d’application de `HitOnGuest` (par ex. `DoDamage(arme de la réplique source, héros, agresseur = réplique)`, cohérente avec la règle « jamais `SetHitPoints` direct »), ou, si on choisit `SetHitPoints`, dire dans AD-11 que la mort de l’invité est alors détectée par le mod (PV ≤ 0 après application) et que `DownOrDeadBy*` ne sert qu’aux coups natifs résiduels. L’une ou l’autre, mais écrite.

### F3 — AD-10 contredit le code et AD-9 — **Moyenne**

- `MotionParam` (`MsgType = 26`, v6, story 2.4b) existe dans `core/protocol.h` et est cité par `epic-3-context.md`, mais **n’est pas** dans la liste fermée d’AD-10. La spine contredit donc le code existant au lieu de le ratifier, et AD-8 ne le mentionne pas non plus alors qu’il s’étend aux PNJ.
- AD-10 donne `Spawn{NetId, type, pose}` alors qu’AD-9 dit maintenant que « le GUID voyage dans le `Spawn` du PNJ ». Le code (`struct Spawn`) n’a ni GUID ni type PNJ (`kSpawnTypeHero = 0` seul). La forme du `Spawn` PNJ (champ GUID conditionnel au type ? message distinct ?) est donc un point de divergence entre 3.3 côté hôte et côté invité, et le faux invité Go.
- `HitRequest{NetId cible, dégâts}` (AD-9, AD-10) : avec `DoDamage(arme, cible, agresseur)`, c’est le jeu de l’hôte qui calcule les dégâts à partir de l’arme. Le champ `dégâts` est alors soit ignoré, soit en conflit (l’ancienne ligne d’AD-9 « l’hôte l’applique avec la fonction de dégâts du jeu » n’a pas été réconciliée).

**Correctif :** AD-10 : ajouter `MotionParam`, écrire `Spawn{NetId, type, pose, GUID si type = PNJ}` (ou la forme choisie) et `HitRequest{NetId cible}` (supprimer `dégâts`, ou dire explicitement qu’il est ignoré/indicatif et pourquoi). Rappeler qu’un changement de struct, pas seulement l’ajout d’un type, incrémente la version et les vecteurs.

### F4 — PV et mort des répliques chez l’invité : mécanisme non fixé, et règle trop large — **Moyenne**

AD-9 : « leur position, leur animation, leurs PV et leur mort viennent exclusivement des snapshots et des événements », puis sous « Dégâts côté hôte » : « Ne jamais modifier directement les PV d’un PNJ ». Lue littéralement, cette seconde phrase interdit à la story 3.3 d’appliquer les PV du snapshot à une réplique par `SetHitPoints`, seul moyen évident. Et la façon d’appliquer `Death{NetId}` d’un PNJ chez l’invité (`Kill` natif, avec scripts de mort, butin et XP de l’invité ? ragdoll seul ? `SetHitPoints(0)` ?) n’est pas décidée, alors que `Kill` sur une réplique déclencherait justement les scripts qu’AD-9 veut réserver à l’hôte.

**Correctif :** restreindre la phrase à l’hôte (« chez l’hôte, ne jamais… ») et ajouter une règle pour l’invité : PV de la réplique écrits par `SetHitPoints` (affichage seul), mort par le mécanisme X sans `Kill` natif. Si non tranché, ouvrir une OQ rattachée à 3.3 (butin et XP de l’invité compris, ou explicitement Deferred).

### F5 — Budget `Motion` des PNJ : décision orpheline et bloquante — **Moyenne**

AD-8 (« Portée ») et l’OQ 3 rayée disent que le budget est « fixé avant toute story `sync/npc` ». La story 3.3 **est** cette story. Le memlog ne le fixe pas (« inchangé, non mesuré ») et aucune OQ numérotée ne le porte plus : il est noyé dans une question marquée résolue. `epic-3-context.md` rappelle en plus que le trafic `MotionParam` de l’avatar est élevé.

**Correctif :** soit poser une valeur `[ASSUMPTION]` (par ex. N `Motion` + `MotionParam` par PNJ et par seconde, priorité au plus proche comme AD-7), soit créer une OQ 11 explicite, rattachée à la première tâche de 3.3, et retirer la mention de l’OQ 3 rayée.

### F6 — AD-2 : contenu d’`AnimFilter` pas mis à jour — **Moyenne**

AD-9 dit que l’ensemble des répliques du hook de routine « vit dans le même filtre que celui d’AD-2 ». Mais AD-2 décrit `AnimFilter` comme contenant des **PS d’animation** (et le code, `sync/anim_filter.h`, ne stocke que des `animation_ps`), alors que le hook de routine reçoit un `gCScriptRoutine_PS` (le « PS 0x24 » du spike). Un développeur ne sait pas s’il faut un second ensemble dans le filtre, une table entité → PS, ou un filtre séparé ; et la règle « une entité est retirée du filtre avant d’être oubliée » doit couvrir les deux. Par ailleurs AD-2 affirme « Leur seul état partagé est un objet unique » pour `PlayMotion` ↔ frame, ce qui reste vrai, mais l’exception du hook de routine ne dit pas s’il prend le verrou.

**Correctif :** dans AD-2, lister le contenu du filtre : PS d’animation du héros local, PS d’animation des répliques, **PS de routine des répliques** ; ajouter et retirer les deux ensemble, par entité. Préciser que le hook de routine (thread de la frame) consulte le filtre sous le même verrou.

### F7 — AD-3 : `Script_Game.dll` dans l’empreinte change le format de `Hello` — **Basse**

`Hello::fingerprint` est un `std::array<…, 4>` (`kFingerprintModules = 4`, `core/protocol.h`, `core/session.h`). Ajouter un 5ᵉ module modifie le `Hello`, donc la version du protocole et les vecteurs, ce qu’AD-3 ne dit pas. Par ailleurs le code du spike résout les symboles de `Script_Game.dll` comme **facultatifs** (`game/combat.h` : « facultatifs », `win/hooks.cpp` : « expérience inactive ») ; la règle « tout ou rien » s’applique aux stories produit, ce qui mérite d’être dit pour éviter qu’on reprenne le code de la sonde tel quel.

**Correctif :** une phrase dans AD-3 : « ajouter un module à l’empreinte change `Hello` et incrémente la version du protocole (AD-10) ; les symboles de `Script_Game.dll` du produit sont résolus en tout ou rien, comme les autres (le caractère facultatif est propre à la sonde) ».

### F8 — Textes anciens devenus faux ou périmés — **Basse**

- AD-8 « Non validé : le chemin par nom (`GetMotion`) et le descripteur reconstruit… La première tâche de la story avatar est un miroir local **par nom** » et l’`[ASSUMPTION]` sur le descripteur (« à établir dans la story avatar ») : l’Epic 2 est fusionnée et `game/motion_desc.h` documente les 7 scalaires lus (+0x00 à +0x1B). Ces lignes sont périmées ; à remplacer par « validé (Epic 2), voir `motion_desc.h` ».
- AD-2 `[ASSUMPTION : le hook est appelé sur le thread principal ; la première story le journalise]` : `AnimFilter` mesure déjà le thread (`hook_thread_varied`). À marquer vérifié ou à garder avec le résultat.
- AD-11 « Fin de monde = changement du pointeur **ou** d’un second signal… `[ASSUMPTION … à valider dans la première story de session]` » : le code (`core/session.cpp`) n’utilise que `player` changé ou `running` faux (`gCSession::IsGameRunning`). Ratifier ce qui est fait (pointeur + `IsGameRunning`) au lieu de laisser une hypothèse que le code a déjà tranchée.
- AD-11 interception : « agissent quand la victime est son héros » est ambigu chez l’**hôte**, où les mêmes détours sont installés mais où la mort de l’hôte doit rester native (elle termine la session, `HostDied`). Écrire « chez l’invité en session, quand la victime est son héros ; chez l’hôte, le héros local passe toujours à l’original ».
- AD-8 « Avatar » : « créé par `Entity::Spawn(template du héros local…)` » — avec AD-9, l’arme de l’avatar vient d’un « template de l’arme » dont la source n’est pas dite (voir F9).

### F9 — Deferred « Équipement de l’invité » laisse diverger 3.1 et 3.2 — **Basse à moyenne**

Le Deferred renvoie le format de `guest_equipment` à « la story FR-6 » (devenue 3.1). Or AD-9 fixe que l’arme de l’avatar est créée par `CreateItems(template de l’arme)` dans la story 3.2 : le format de l’équipement dans `Hello` (nom de template ? GUID de template ?) est précisément l’interface entre 3.1 et 3.2, et le faux invité Go doit le produire. Deux unités peuvent donc diverger sur un élément Deferred, ce que la grille interdit. S’y ajoute un trou non listé : l’arme **rangée/sortie** (spike, ligne « Q5 — arme rangée » : « à revoir en 3.2 ») et le **changement d’arme en cours de session**, qu’aucun type d’AD-10 ne transporte.

**Correctif :** sortir l’équipement du Deferred : fixer dans AD-5/AD-9 que l’équipement voyage comme une liste de noms de template (ou la forme choisie) et que l’arme ne change pas en cours de session pour J2 ; ouvrir une OQ « arme rangée » rattachée à 3.2, ou déclarer explicitement que l’avatar garde l’arme en main en permanence en J2.

### F10 — AD trop longs, décision et justification mêlées — **Basse (forme)**

AD-8 compte maintenant 11 puces dont l’historique de validation J1 (« Validé en local… », « Non validé… ») et les variantes écartées (`SetGoalPose`, « car l’avatar glisse… »). AD-11 contient « Écartés au spike » avec leurs causes (`ZS_RagDollDead`, T-pose, corps invisible), AD-9 la raison de l’interdiction d’`EnableClassProcessing`. Ce sont des justifications utiles mais qui appartiennent à `docs/re/spike-j2.md` ; dans la Rule, seul l’interdit compte.

**Correctif :** garder dans la Rule l’interdit en une ligne (« `SetGoalPose`, `Kill → Defeat`, `ZS_Unconscious` forcé et relève d’un mort sont interdits ; raisons : `docs/re/spike-j2.md` ») et déplacer l’historique de validation d’AD-8 dans un renvoi. Envisager de scinder AD-8 (rejeu d’animation) et le placement/solidité de l’avatar (nouvel AD ou sous-section d’AD-9).

## 3. Grille

| Critère | Évaluation |
| --- | --- |
| Fixe les vrais points de divergence de l’Epic 3, sans en oublier | **Non** : F1 (coups natifs côté invité), F2 (application de `HitOnGuest` ↔ interception), F4 (PV/mort des répliques), F3 (`Spawn` PNJ) manquent |
| Chaque Rule est applicable et empêche la divergence annoncée | Globalement oui pour les ajouts du spike ; AD-9 « Dégâts » trop large (F4), AD-2 filtre ambigu (F6) |
| Rien sous Deferred ne permet à deux unités de diverger | **Non** : équipement de l’invité (F9) |
| Ratifie le code existant | **Non** sur deux points : `MotionParam` absent d’AD-10, fin de monde (F3, F8) |
| Chaque dimension décidée, différée ou en question ouverte | Budget `Motion` PNJ orphelin (F5) ; arme rangée non listée (F9) ; butin/XP de l’invité sur un PNJ tué non dit (F4) |
| Cohérence interne après mise à jour | Contradictions AD-10 ↔ AD-9 (`Spawn`, `HitRequest`), AD-2 ↔ AD-9 (filtre), lignes AD-8/AD-11 périmées (F3, F6, F8) |
| Séparation décision / justification | Dégradée (F10) |

## 4. Ce qui est bien

- Les conclusions du spike sont reprises avec précision (symboles, offsets, rôles `self`/`other`, interdiction d’`EnableClassProcessing`), et chaque décision renvoie à une observation vérifiée.
- Les nouvelles exceptions d’AD-2 sont bornées (« ne fait que comparer », « appels synchrones listés ») et justifiées par une mesure de thread.
- Les questions restantes (OQ 8 à 10) sont rattachées à une story précise, et la table Capability → Architecture a suivi (`win/hooks`, `game/combat`).
- Le choix du corps cinématique plutôt que `SetGoalPose` est cohérent avec AD-8 (seule la réplication anime une réplique).

## 5. Ordre de correction suggéré

1. F1 + F2 (une seule décision « point d’accroche des coups » et « application de `HitOnGuest` »), avant de détailler 3.4/3.5/3.6.
2. F3, F4, F5, F6 avant la story 3.3.
3. F7, F8, F9, F10 au fil de l’eau.

# Revue « confrontation à la réalité » — spine J1–J2, mise à jour du 2026-10-20 (spike J2)

- **Objet :** `ARCHITECTURE-SPINE.md` (diff non commité du 2026-10-20 : AD-2, AD-3, AD-8, AD-9, AD-11, questions 3 à 10).
- **Lentille :** chaque décision engagée est-elle étayée par la réalité du projet, et non affirmée de mémoire ?
- **Sources confrontées :**
  - le code : `client/src/game/combat.cpp`, `client/src/sync/probe_j2.cpp`, `client/src/win/hooks.cpp`, `client/src/game/combat.h`, `client/src/core/version.*` ;
  - la doc RE `docs/re/spike-j2.md` (tableau « Réponses » et section « Conclusions ») ;
  - les DLL `risen_ori/bin` (exports par `objdump -p`, désassemblage ciblé par `llvm-objdump`, imports de `Script_Game.dll` par `llvm-readobj --coff-imports`).
- **Limite :** le `Script_Multiplayer.log` des 7 tests a été écrasé (8 lignes, aucune ligne J2). Les résultats en jeu ne sont donc connus que par le tableau de la doc RE.

## Verdict

**Globalement étayée, à corriger avant les stories 3.1 et 3.6.**

Tous les noms décorés cités existent dans le build, et la plupart des décisions s’appuient sur le tableau des résultats. Deux affirmations engagées dépassent pourtant ce qui a été vérifié :

- AD-8 affirme que l’avatar cinématique « bloque le héros local (vérifié) », alors que le tableau ne le montre pas. La recette décrite omet aussi des réglages qui étaient actifs pendant le test.
- AD-11 décrit une interception du coup mortel. Or le détour réellement testé force 1 PV à **chaque** appel, sans tester les PV, et le désassemblage montre que ce point est aussi celui de l’« assommé ».

Trois écarts de rang moyen concernent AD-9 et AD-11 : la clé GUID, la sémantique des dégâts et le chemin `HitOnGuest`.

## Ce qui est vérifié (aucune action)

| Affirmation de la spine | Preuve |
|---|---|
| Noms décorés cités | Tous présents dans les exports : `?PlayMotion@eCAnimation_PS…`, `?OnProcess@gCScriptRoutine_PS@@UEAAXXZ`, les 3 `?DownOrDeadBy…@@YA_JPEAVgCScriptProcessingUnit@@PEAX1_J@Z`, `?Kill@@…`, `?Defeat@@…`, `?GetOtherEntity@gCScriptProcessingUnit@@QEAAPEAVeCGeometryEntity@@XZ`, `?SetHitPoints@PSSkills…`, `?AddBodyFlag@PSRigidBody…`, `?EnableGravitation@PSCharacterMovement…`, `?SetPose@Entity@@QEAAXAEBVbCMatrix@@@Z`, `?SetPose@PSRigidBody@@QEAAXVbCMatrix@@@Z`, `?SetGoalPose@PSCharacterMovement…`, `?GetID@eCDynamicEntity…`, `?GetEntity@gCRuntimeInfo@@QEAAPEAVeCEntity@@AEBVbCPropertyID@@@Z`, `?DoDamage@PSDamage@@QEAA_NAEAVEntity@@0@Z`, `?DoDamage@gCDamage_PS…`, `?OnDamage@eCDynamicEntity…`, `?CreateItems@PSInventory…`, `?HoldRightStack@PSInventory…`, `?GetCombatMode@PSCombatSystem@@QEBA?AW4gECombatMode@@XZ`. |
| `EnableClassProcessing` est un booléen global (AD-9) | Désassemblage de `gCScriptRoutine_PS::EnableClassProcessing` et de `gCNPC_PS::EnableClassProcessing` : un seul `movb %dl, <statique>` puis `ret`. C’est bien un statique de classe. Que ce statique fige aussi le héros est une déduction raisonnable : le héros a lui aussi un PS de routine, cf. `ZS_Dead_LoopPlayer`. |
| `DownOrDeadBy*` : `other` = victime, et retour 0 sans rien faire si la victime a des PV (AD-11) | Désassemblage des trois variantes, de même structure. `r8` (`other`), ou `GetOtherEntity` s’il est nul, est la victime. On trouve ensuite `PSSkills::GetTotal(victime, 0)`, puis `jle` : si les PV sont > 0, la fonction rend 0. `self` porte le `PSDamage` (`PSDamage::GetAgressor(self)`) : c’est l’objet qui frappe (arme ou projectile), pas forcément l’agresseur. |
| Arme : `CreateItems` puis `HoldRightStack` ; `GetCombatMode` vaut 0 (AD-9) | Tableau, Q5 (1ᵉʳ test) et `probe_j2.cpp` `num1_weapon`. |
| GUID stable d’un rechargement à l’autre, aller-retour `GetEntity` (AD-9) | Tableau, Q3 (2ᵉ et 3ᵉ tests, 3 PNJ), pour un même PC. |
| IA figée PNJ par PNJ par le hook de routine (AD-9) | Tableau, Q3 Num4, et `on_routine_process`. |
| `DoDamage(arme, cible, agresseur)` retire des PV côté hôte (AD-9) | Tableau, Q3 Num5 : 150 → 60 PV, agresseur = copie ou héros. |
| `Kill → Defeat`, `ZS_Unconscious` forcé et relève d’un mort sont écartés (AD-11) | Tableau, Q7 (tests 3 à 6), en cohérence avec la spine. |
| `OnProcess` non bridé, à un seul thread (AD-2) | Hérité de J1, non modifié. |

## Constats

### C1 — HAUTE — AD-8 : « Il bloque alors le héros local (vérifié au spike J2) » n’est pas étayé par le tableau, et la recette omet des réglages actifs pendant le test

- **Spine (AD-8, Placement) :** en J2, l’avatar est un corps cinématique (`AddBodyFlag(0x81)`, `EnableGravitation(false)`), placé par `Entity::SetPose` puis `PSRigidBody::SetPose`. « Il bloque alors le héros local (vérifié au spike J2). » La question 6 est marquée **résolue**.
- **Tableau des résultats :**
  - mode 1 : « Héros bloqué par la copie : **non observé**. À compléter (2ᵉ test). »
  - mode 2 (cinématique) : « 0 échec de placement. **Peu observé en mouvement.** À compléter (2ᵉ test). »
  - Ces deux lignes n’ont jamais été complétées. Seule la section « Conclusions » affirme que le mode 2 « bloque le héros local ». Rien dans le tableau ne trace le test qui le montrerait.
- **Code (`probe_j2.cpp`, `num2_collision`) :** les modes sont **cumulatifs**. On ne passe au mode 2 qu’après le mode 1, qui applique `SetCollisionGroup(4)`, `EnableCollision(true)` et `SetDCCEnabled(true)`. La remise à 0 ne défait que le groupe. Le mode 2 testé, c’était donc « mode 1 + cinématique ». La spine ne retient que la partie cinématique.
- **Annexe :** la valeur `0x81` n’est pas identifiée sémantiquement. Le commentaire de `combat.h` dit seulement « `eERigidbody_Flag`, ajoutés par OU ». L’expression « corps cinématique » est une interprétation.
- **Correctif :**
  1. Ajouter au tableau une ligne datée qui observe le blocage du héros en mode 2. Sinon, rétrograder la phrase en `[ASSUMPTION : l’avatar cinématique bloque le héros ; première tâche de la story 3.1/3.2]` et rouvrir la question 6 en « partiellement résolue ».
  2. Écrire la recette complète dans AD-8 : `EnableCollision(true)` et `SetDCCEnabled(true)` (le groupe 4 était déjà en place), puis `AddBodyFlag(0x81)` et `EnableGravitation(false)`. Sinon, indiquer explicitement que ces préalables ne sont pas nécessaires, preuve à l’appui.
  3. Remplacer « corps cinématique » par « drapeaux `0x81` (0x80 | 0x1), sémantique non établie ».

### C2 — HAUTE — AD-11 : la règle d’interception doit tester les PV, et elle avale aussi l’« assommé »

- **Spine :** « Le détour le remet à 1 PV et rend 0 sans appeler l’original, comme le jeu le fait pour une victime qui survit. »
- **Désassemblage (`DownOrDeadByMeleeWeapon`, idem pour `Projectile` et `Inferno`) :** la fonction est appelée **après** l’application des dégâts, et elle teste elle-même `GetTotal(victime, 0) > 0`. Elle est donc appelée aussi sur des coups **non mortels**, et c’est elle qui trie.
- **Code testé (`probe_j2.cpp`, `intercept_down_or_dead`) :** le détour applique `SetHitPoints(1, fixer)` dès que la victime est le héros, **sans lire ses PV**. En session, un coup de 10 PV sur un héros qui en a 100 le ramènerait à 1 PV. Le compteur « coups mortels sur le héros » compte en réalité tous les appels. Le résultat « 10 coups mortels sur 10 annulés » (7ᵉ test) peut donc inclure des coups non mortels. Ce biais est invisible quand le héros est déjà à 1 PV.
- **Second problème :** quand les PV sont ≤ 0, la fonction a deux issues. Elle lit `PSNpc::PropertyDamageCalculationType` de la victime et de l’agresseur : le type 1 mène à l’« assommé » (`ZS_Unconscious`, ce que la doc RE dit aussi : « sinon `ZS_Unconscious` pour un coup qui assomme »), les autres à `Kill`. Ne pas appeler l’original supprime donc aussi les mises à terre légitimes. C’est le cas des duels à mains nues, qui peuvent porter des quêtes, même si le départ se fait en zone neutre.
- **Test :** seule la mêlée a été testée en jeu. `Projectile` et `Inferno` ne sont validés que statiquement, par la structure identique.
- **Correctif :** préciser la règle ainsi :
  - le détour lit les PV de la victime ;
  - si `PV > 0`, ou si la victime n’est pas le héros, il appelle l’original ;
  - sinon, il remet 1 PV et rend 0.
  - Ajouter `[ASSUMPTION]` ou une question ouverte : faut-il laisser passer l’issue « assommé » (type de calcul 1) ? À trancher en 3.6.
  - Noter que les variantes `Projectile` et `Inferno` sont vérifiées statiquement seulement.
  - Corriger la sonde dans la même story.

### C3 — MOYENNE — AD-9 et AD-11 : le chemin létal réel de l’invité (`HitOnGuest`) n’est pas couvert par l’interception

- Chez l’invité, les PNJ sont des répliques dont l’IA est figée (AD-9). Ils ne frappent donc pas nativement son héros. Ses dégâts arrivent par `HitOnGuest`, que l’invité applique « à son propre héros ».
- La spine ne dit pas **comment** ils sont appliqués (`DoDamage` avec quelle arme ? `SetHitPoints` ?). Elle ne dit pas non plus qu’un `HitOnGuest` mortel doit être plafonné à 1 PV, puis suivi de `Death{2}`.
- Avec `SetHitPoints`, `DownOrDeadBy*` n’est jamais appelé, et l’interception d’AD-11 ne sert pas. Avec `DoDamage`, il faut une arme côté invité.
- L’interception d’AD-11 ne couvre que les coups **natifs** (environnement, hôte s’il frappait), ce qui reste utile.
- **Correctif :** dans AD-9 ou AD-11, fixer l’application de `HitOnGuest` et le traitement de son cas mortel (plafond à 1 PV et `Death{2}`, sans passer par la mort native). Sinon, l’ajouter à la question 8, à trancher en 3.5 ou 3.6.

### C4 — MOYENNE — AD-9 : « GUID de 16 octets » et `GetEntity(GUID)` ont été testés avec le `bCPropertyID` complet de 20 octets, sur un seul PC

- `bCPropertyID` fait `0x14` octets (`combat.h` : `kPropertyIdSize = 0x14`). La sonde passe les 20 octets lus à `GetEntity`, dans le même processus.
- Le désassemblage montre deux comportements :
  - `bCPropertyID::operator==` ne compare que les 16 premiers octets ;
  - mais `g_GetHashValue<bCPropertyID>` vaut `(dword0 >> 3) + dword@0x10`, c’est-à-dire qu’il **utilise les octets 16 à 19**.
- Si `GetEntity` passe par une table hachée, un identifiant reconstruit à partir des 16 octets du réseau, avec une fin arbitraire, peut ne rien trouver. Le constructeur `bCPropertyID(const bCGuid&)` met `+0x10` à 0. On ignore si les entités stockent 0 à cet endroit, et le log est perdu.
- L’égalité du GUID **entre deux PC** qui chargent la même sauvegarde est déduite (stable au rechargement, donc probablement sérialisé). Elle n’a pas été observée.
- La question ouverte 10 affirme que les PNJ créés en cours de partie « reçoivent un GUID neuf de chaque côté ». Le tableau dit au contraire « restent à vérifier ».
- **Correctif :**
  - Préciser dans AD-9 que le récepteur reconstruit l’identifiant par `bCPropertyID(const bCGuid&)`. À défaut, transmettre les 20 octets.
  - Faire de la première tâche de la story 3.3 un aller-retour `GetEntity` sur un identifiant **reconstruit** à partir des 16 octets.
  - Marquer `[ASSUMPTION]` l’égalité entre PC, et formuler la question 10 comme une hypothèse.

### C5 — MOYENNE — AD-9 : la sémantique des dégâts dépasse ce qui a été observé

- « Ne jamais modifier directement les PV d’un PNJ : il n’y aurait ni réaction au coup, ni script de mort » n’a pas été testé. Le repli `SetHitPoints` de Num5 ne s’exécute que si `DoDamage` échoue, ce qui n’est jamais arrivé. Côté `DoDamage`, le tableau ne rapporte que la baisse des PV (150 → 60). Les questions « réaction au coup », « devient-il hostile ? » et « meurt-il normalement ? » du protocole sont restées sans réponse.
- `HitRequest{NetId, dégâts}` transporte un montant, mais `DoDamage` calcule les dégâts à partir du PS de l’arme : `(PS+0x40 + PS+0x44) × PS+0x48`. La sonde a dû forcer `SetDamageAmount(10)`, puis rétablir la valeur. La spine ne dit pas si le champ `dégâts` est utilisé, ni comment.
- **Correctif :**
  - Marquer la justification « réaction et script de mort » comme `[ASSUMPTION : à vérifier en 3.4 en tuant un PNJ par DoDamage]`.
  - Décider si `HitRequest.dégâts` est appliqué par `SetDamageAmount` sur l’arme de l’avatar (sauvegardé puis rétabli, comme dans la sonde) ou s’il est retiré du message.
- **Corollaire, à noter dans les questions ouvertes :** chez l’invité, une réplique figée « ne meurt pas normalement » (protocole, étape 7). Le moyen d’appliquer `Death{NetId}` à une réplique n’est pas établi.

### C6 — BASSE — Divers écarts de formulation

- **AD-2 :** la spine dit que les détours `DownOrDeadBy*` « tournent sur le thread d’`OnProcess` (vérifié en jeu) ». Seul le détour `Kill` enregistre son thread (`g_kill_thread` dans `intercept_kill`). Pour la mêlée, la preuve est transitive, puisque `DownOrDeadByMeleeWeapon` appelle `Kill`. Pour `Projectile` et `Inferno`, elle est inexistante. **Correctif :** écrire « vérifié pour `Kill`, appelé depuis `DownOrDeadByMeleeWeapon` », et journaliser le thread dans `intercept_down_or_dead`.
- **AD-3, chargement de `Script_Game.dll` :** « chargée par le jeu avant le mod » est affirmé comme un fait. Le code le marque `[ASSUMPTION]` (« ordre alphabétique ? », `combat.cpp:206`). Les tests montrent qu’il tient sous Proton, mais la cause n’est pas établie, et la règle « tout ou rien » rendrait le mod entier inactif si l’ordre changeait. **Correctif :** garder `[ASSUMPTION]`, ou résoudre `Script_Game.dll` paresseusement au premier `OnProcess` avec un monde chargé, ce qui supprime la dépendance à l’ordre.
- **AD-3, empreinte de version :** elle est dite couvrir `Game`, `Engine`, `Script`, `SharedBase` et `Script_Game`. Le code (`core/version.h`, `SupportedVersion`) ne prend que `Game.dll` et `Engine.dll`. Les offsets codés en dur du J2 ne sont pas listés comme couverts : types de PS `0x1f`, `0x24`, `0x29`, `0xd`, `0x15`, `0x5` ; `AIMode` à `+0xC0` ; proxy de l’unité de script à `+0x150` ; dégâts à `PS+0x40/0x44/0x48` ; drapeaux à `+0x90`. **Correctif :** écrire la règle comme une exigence (« doit couvrir… ; à étendre en 3.x »), et nommer ces offsets dans `docs/re/spike-j2.md` comme couverts par l’empreinte.
- **Question 9 :** « message de perte d’XP affiché à chaque coup mortel annulé ». Le tableau ne le rapporte qu’au 4ᵉ test (`Kill → Defeat`), pas au 7ᵉ (interception dans `DownOrDeadBy*`). **Correctif :** « observé avec `Kill → Defeat` ; à vérifier avec l’interception retenue ».
- **AD-11, `self` :** « `self` = ce qui frappe » est juste, mais c’est l’objet qui porte le `PSDamage` (arme ou projectile), et l’agresseur s’obtient par `PSDamage::GetAgressor(self)`. Utile pour la question 8 (`HitOnGuest{NetId source}`).
- **AD-9, réplique figée :** la manière de placer une réplique de PNJ chez l’invité (`MoveTo` ? cinématique ?) n’est fixée nulle part. AD-8 ne traite que l’avatar. À ajouter aux questions ouvertes avant la story `sync/npc`.

## Synthèse des correctifs proposés (sans modifier la spine ici)

1. **AD-8 :** rétrograder le blocage du héros en `[ASSUMPTION]`, ou compléter le tableau ; écrire la recette complète (collision et DCC du mode 1) ; ne plus qualifier `0x81` de « cinématique ».
2. **AD-11 :** conditionner l’interception à `PV ≤ 0`, appeler l’original sinon ; ouvrir une question sur l’issue « assommé » ; corriger `intercept_down_or_dead`.
3. **AD-9 et AD-11 :** fixer l’application de `HitOnGuest` chez l’invité et son plafond mortel.
4. **AD-9 :** reconstruire l’identifiant par `bCPropertyID(bCGuid)`, ou transmettre 20 octets ; marquer en `[ASSUMPTION]` l’égalité du GUID entre deux PC et la question 10.
5. **AD-9 :** marquer en `[ASSUMPTION]` la réaction au coup et le script de mort via `DoDamage` ; trancher l’usage de `HitRequest.dégâts`.
6. **Formulations à corriger :** thread de `DownOrDeadBy*`, ordre de chargement de `Script_Game.dll`, empreinte réelle, origine du message de perte d’XP.

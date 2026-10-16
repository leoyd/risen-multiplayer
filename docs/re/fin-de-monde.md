# Fin de monde et mort de l'hôte — signaux (build Steam x64, janvier 2023)

Story 1.5. Méthode : exports décorés et désassemblage (`llvm-readobj --coff-exports`,
`llvm-objdump -d`). Les trois signaux sont lus à chaque frame par `sync/session`
(`read_world`), depuis le hook `eCApplication::OnProcess`, via `game/entity` (sous SEH).

## Signaux

| Signal | Symbole | Module | Ce que fait le code |
|---|---|---|---|
| `player` | `?GetPlayer@Entity@@SA?AV1@XZ` | Script.dll | `Entity` (8 octets) par sret ; nul au menu |
| `running` | `?GetInstance@gCSession@@SAAEAV1@XZ` | Game.dll (RVA `0x974670`) | `mov rax, [g_session] ; ret` : renvoie le pointeur global, **nul possible** malgré le type référence |
| | `?IsGameRunning@gCSession@@QEBA_NXZ` | Game.dll (RVA `0x9745E0`) | `movzx eax, byte [rcx+0x110] ; ret` |
| `dead` | `?IsDead@Entity@@QEBA_NXZ` | Script.dll (RVA `0x209180`) | entité nulle → faux ; sinon `GetPropertySet(0x24)` (vtable `+0x198`) ; PS nul → faux ; sinon `[ps+0xC0] == 12` |

`game::game_running` vérifie l'instance avant d'appeler `IsGameRunning` (instance nulle :
`running = false`). Le pointeur du joueur passe à `core/session` comme entier opaque
(`uintptr_t`), jamais déréférencé hors de `game/` (AD-6).

## Règle retenue (`core/session`)

- **Monde chargé** = `player ≠ 0` et `running`.
- **Fin de monde** = en `Hosting`, `Connecting` ou `InSession`, le monde n'est plus chargé, ou
  `player` diffère du pointeur mémorisé à l'armement.
- **Mort de l'hôte** = rôle hôte, `Hosting` ou `InSession`, et `dead`.
- **Nouveau monde** (pour réarmer après une fin) : le monde a cessé d'être chargé, ou `player` a
  changé, à un moment quelconque depuis l'armement (y compris pendant le maintien de 1 s).
  **Repli** : `dead` qui passe de 1 à 0 compte aussi comme un nouveau monde. Après la mort de
  l'hôte, si le jeu recharge avec la même adresse de héros sans que `running` retombe, le héros
  revit, et c'est ce passage qui réarme la session (sinon elle resterait inactive jusqu'au
  redémarrage du jeu).
- Un hôte dont le héros est mort n'arme pas (il finirait aussitôt en « mort de l'hôte »).
- `dead` n'est lu que dans un monde chargé (`player ≠ 0` et `running`) : pas pendant la
  destruction du monde, où l'entité peut être à moitié détruite.

Le jeu peut redonner la même adresse au héros après un rechargement : `running` sert alors de
second signal. **Hypothèse à vérifier en jeu** : `running` retombe à 0 (ou `player` devient nul)
pendant tout chargement, même rapide. Si ce n'est pas le cas, le signal de remplacement
(`eCEntity::GetID` du héros) fera l'objet d'une story suivante.

## Résultats du test en jeu

Test du 2026-10-17 (Risen Steam x64 sous Proton, hôte, `log_level = debug`, faux invité en boucle
sur 127.0.0.1) : 3 chargements rapides en session, puis mort du héros en session.

**Réponses :**

1. **Chargement rapide : `running` reste à 1** dans toutes les frames observées. `OnProcess`
   ne tourne pas pendant le chargement, donc aucun état intermédiaire n'est vu. **`player`
   change d'adresse à chaque fois** (`0x1c5c3c00` → `0x1ef1ec80` → `0x1ed12160` → `0x1c29eba0`) :
   c'est ce signal qui détecte la fin de monde. Le second signal (`running`) ne sert donc pas
   pour le chargement rapide. L'adresse pourrait un jour être réutilisée ; `eCEntity::GetID`
   reste le recours documenté (voir `deferred-work.md`).
2. **Menu principal au lancement : `player = 0`, `running = 0`** (première ligne `signaux`).
   Le **retour au menu en cours de session n'a pas encore été observé en jeu**. Il est couvert
   par les tests de `core/session`.
3. **Mort : `dead` passe à 1 avec le même `player`**, avant tout rechargement.
   `SessionEnd{3}` part aussitôt. Le monde suivant (après l'écran de mort, environ 15 s) a une
   nouvelle adresse de joueur, `0x1d431510`, et la session se réarme.
4. **`OnProcess` s'arrête pendant un chargement rapide pendant environ 3 à 3,5 s** (plus aucun
   heartbeat de l'hôte n'arrive pendant ce temps, côté faux invité). La fin de monde n'est
   détectée qu'après le chargement, à la première frame du nouveau monde : le `SessionEnd{2}`
   part alors, **avant** le timeout de 5 s du pair. La marge est faible : un chargement plus
   long (machine lente, zone chargée) déclencherait un timeout chez le pair au lieu de
   « fin de monde ».

**Résultat :** 3 sessions distinctes (`conv` 0x3a705e03, 0xf8ad93ae, 0xc614ed3b, puis 0xbc72e496
et 0xb627f01b), chacune terminée par « fin de monde (2) » ou « mort de l'hôte (3) », reçue par
le faux invité. Réarmement immédiat après une fin de monde. Après la mort, réarmement au monde
suivant. Aucun crash.

```text
[01:35:19.573] DEBUG [sync] signaux : player=0x0000000000000000 running=0 dead=0 (état Inactive)
[01:35:29.697] DEBUG [sync] signaux : player=0x000000001c5c3c00 running=1 dead=0 (état Inactive)
[01:35:29.697] INFO  [sync] Inactive → Hosting : monde chargé, en attente d'un invité sur le port 27960
[01:35:41.965] INFO  [sync] Hosting → InSession : … conv=0x3a705e03 …
[01:36:02.012] DEBUG [sync] Heartbeat reçu (21)          ← dernier avant le chargement rapide
[01:36:05.105] DEBUG [sync] signaux : player=0x000000001ef1ec80 running=1 dead=0 (état InSession)
[01:36:05.105] INFO  [sync] SessionEnd envoyé : fin de monde (2)
[01:36:05.105] INFO  [sync] InSession → Ended : fin de monde ; retour au solo
[01:36:06.104] INFO  [sync] Ended → Inactive : transport arrêté ; réarmement immédiat
[01:36:06.104] INFO  [sync] Inactive → Hosting : monde chargé, en attente d'un invité sur le port 27960
[01:36:22.624] DEBUG [sync] signaux : player=0x000000001ed12160 running=1 dead=0 (état InSession)
[01:36:22.624] INFO  [sync] SessionEnd envoyé : fin de monde (2)
[01:36:40.231] DEBUG [sync] signaux : player=0x000000001c29eba0 running=1 dead=0 (état InSession)
[01:36:40.231] INFO  [sync] SessionEnd envoyé : fin de monde (2)
[01:38:22.469] DEBUG [sync] signaux : player=0x000000001c29eba0 running=1 dead=1 (état InSession)
[01:38:22.469] INFO  [sync] SessionEnd envoyé : mort de l'hôte (3)
[01:38:23.473] INFO  [sync] Ended → Inactive : transport arrêté ; nouvelle session au prochain monde chargé
[01:38:38.767] DEBUG [sync] signaux : player=0x000000001d431510 running=1 dead=0 (état Inactive)
[01:38:38.767] INFO  [sync] Inactive → Hosting : monde chargé, en attente d'un invité sur le port 27960
```

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

**À compléter** après le test manuel (procédure : `client/README.md`, « Fin de session en jeu »).
Recopier ici, avec `log_level = debug`, les lignes `[sync] signaux` et les transitions qui les
entourent, pour :

- trois chargements rapides en session avec le faux invité, puis retour au menu ;
- la mort du héros de l'hôte en session.

Questions à trancher :

1. `running` retombe-t-il pendant un chargement rapide ? `player` change-t-il d'adresse ?
2. `player` est-il nul au menu principal, et `running` faux ?
3. `dead` passe-t-il à 1 à la mort du héros, avant le rechargement automatique ?
4. `OnProcess` s'arrête-t-il pendant l'écran de chargement (et combien de temps) ?

```text
(lignes du log à coller ici)
```

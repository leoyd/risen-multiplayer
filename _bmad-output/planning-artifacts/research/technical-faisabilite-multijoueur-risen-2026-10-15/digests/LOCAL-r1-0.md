# Observation locale (lead, ce run) — inspection des binaires de la copie Steam installée

Observation primaire, pas une source web. Méthode : `file`, `objdump -p`, `strings -a` sur risen_ori/bin (2026-10-15).

- claim: Le build Steam 64 bits contient toujours le dossier `bin/scripts/` avec `Script_Game.dll`, PE32+ x86-64, qui exporte `ScriptInit` et importe Engine/FFC/Game/Script/SharedBase. | source: inspection locale (file, objdump -p) | confidence: high | class: version-compat
- claim: `Game.dll` (x64) contient les chaînes ASCII `*.dll` et `ScriptInit` → le chargeur de plugins script (énumération de DLL + GetProcAddress("ScriptInit")) est très probablement dans Game.dll et actif en 64 bits. Non vérifié en désassemblage ; non testé avec une DLL tierce. | source: inspection locale (strings) | confidence: medium | class: version-compat

## Ajout round 2 — exports retournant un objet par valeur (objdump -p, symboles `?` uniques)
- claim: Exports de méthodes membres (non statiques) retournant une classe par valeur (`?A[UV]` après le qualificatif `E[AB]`) : Engine 435 / 14 830, Game 162 / 10 856, Script 195 / 2 430, SharedBase 571 / 4 717. | confidence: high | class: version-compat
- claim: Parmi eux, des fonctions au cœur du MVP : `?GetPose@Entity@@QEBA?AVbCMatrix@@XZ` (Entity::GetPose → bCMatrix par valeur), `?GetPose@eCRigidBody_PS@@UEBA?AVbCMatrix@@XZ`, `?CalcMoveDir@gCCombatMoveMelee@@QEBA?AVbCVector@@...`. Ces appels tombent exactement dans le piège sret (MSVC : this=RCX, sret=RDX). | confidence: high | class: version-compat
- note: aucun compilateur croisé (mingw g++, clang, clang-cl) installé sur la machine → test empirique de l'ABI non réalisé.

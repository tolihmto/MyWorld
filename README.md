# SFML-MyWorld

Un éditeur/visualiseur de terrain isométrique en C++/SFML.

- Rendu isométrique avec ombres optionnelles et shading par normale.
- Édition de la carte par brosse (hausse/baisse/plat).
- Import/export ZIP (monde) et CSV (legacy), import progressif (non-bloquant).
- UI simple: boutons Générer, Grille, Import/Export.

## Structure du projet

- `src/main.cpp` — boucle principale, UI, entrées clavier/souris, import/export ZIP/CSV.
- `src/render.cpp`, `src/render.hpp` — projection 2D, wireframe, remplissage des cellules, shading/ombres.
- `src/iso.cpp`, `src/iso.hpp` — projection/déprojection isométrique paramétrable (`IsoParams`).
- `src/terrain.cpp` — génération procédurale (`terrain::generateMap`).
- `src/config.hpp` — constantes globales (taille de grille, fenêtre, bornes d’élévation, etc.).
- `assets/` — ressources (police `arial.ttf`, icônes import/export).
- `Makefile` — build multi-plateforme (Windows/Unix), cibles utiles.

## Prérequis

- SFML 2.6.1 (ou compatible). Sous Windows, chemin par défaut: `C:\SFML-2.6.1`.
- MinGW-w64 (g++). Sous MSYS2: `C:\msys64\mingw64`.
- GNU Make.

Le `Makefile` détecte automatiquement:

- `SFML_INC`, `SFML_LIB`, `SFML_BIN`
- `MINGW_BIN` (pour packager les DLLs)

Ajustez les variables en haut du `Makefile` si vos chemins diffèrent, ou exportez des variables d’environnement correspondantes.

## Construire et lancer

### Windows (MSYS2 MinGW-w64)

- Ouvrir le terminal **MSYS2 MinGW64** (icône "MSYS2 MinGW 64-bit").
- Installer les paquets nécessaires:

```sh
pacman -S --needed \
  mingw-w64-x86_64-gcc \
  mingw-w64-x86_64-make \
  mingw-w64-x86_64-sfml
```

Notes:
- Le Makefile utilise le compilateur de MSYS2: `C:\msys64\mingw64\bin\g++.exe`.
- Par défaut, `SFML_DIR` pointe vers `C:\SFML-2.6.1` (SDK SFML Windows). Vous pouvez le laisser ainsi ou l’ajuster si besoin.

Commande de build et d’exécution (séquence idéale):

```sh
mingw32-make clean
mingw32-make rebuild
mingw32-make package
mingw32-make run
```

- `rebuild` nettoie et recompile dans `build/`, lie vers `bin/game.exe`.
- `package` copie les DLLs nécessaires dans `bin/`.
- `run` lance `bin/game.exe` depuis `bin/`.

Unix (Linux/macOS avec SFML installé):

```sh
make
make run
```

## Cibles Make utiles

- `make build` — compile sans nettoyer.
- `make rebuild` — nettoie et recompile.
- `make run` — exécute l’appli.
- `make clean` — supprime `build/` et `bin/`.
- `make package` — copie `assets/` et les DLLs SFML/MinGW dans `bin/` pour redistribution.

## Contrôles

- **Souris**
  - Molette: zoom in/out.
  - Bouton du milieu: drag pour panner la vue.
  - Clic gauche: peindre pour **augmenter** l’élévation dans le rayon de la brosse.
  - Clic droit: peindre pour **diminuer** l’élévation.
  - Drag droit (en dehors de l’édition) : rotation (X) et tilt (Y) de la projection.
- **Clavier**
  - `G`: bascule le **mode procédural** (seed aléatoire à l’activation, OFF = terrain plat).
  - `F2`: activer/désactiver les ombres.
  - `F3`: afficher/masquer la grille (wireframe).
  - `R`: réinitialiser la caméra/projection (45°, pitch 1) et recentrer.
  - `W/A/S/D` ou flèches: pan de la vue.
  - `Z/Q/S/D` (AZERTY): alias de `W/A/S/D`.
  - Bouton "Re-seed": tire un nouveau seed aléatoire quand le mode procédural est actif.
  - Champ "Seed": cliquer puis saisir un entier, valider avec Entrée pour appliquer le seed.

Note: Un mode "aplatissement" de la brosse est disponible dans le code (plat à une hauteur cible) et s’active depuis l’UI; il agit sur la zone circulaire de la brosse.

## Import/Export ZIP (archive de monde)

- __Vue d’ensemble__
  - Import/export d’un monde complet sous forme d’archive `.zip`.
  - Les JSON sont optionnels (fallbacks gérés), les CSV des chunks sont extraits sur disque.
- __Structure de l’archive__
  - `world.json` — métadonnées:
    - `app` (string), `format` (int, version actuelle 2)
    - `seed` (int), `continents` (bool), `procedural` (bool), `water_only` (bool)
    - `saved_at` (string)
  - `painted.json` — couleurs de peinture par cellule:
    - `{ "cells": [ {"I":int, "J":int, "r":0..255, "g":..., "b":..., "a":...}, ... ] }`
  - `markers.json` — marqueurs utilisateur:
    - `{ "markers": [ {"I":int, "J":int, "label":"...", "r":..., "g":..., "b":..., "a":..., "icon":"..."}, ... ] }`
  - `colors.json` — historique de couleurs récentes:
    - `{ "colors": [ {"r":..., "g":..., "b":..., "a":...}, ... ] }` (jusqu’à 5)
  - `maps/seed_<seed>[_cont]/.../*.csv` — chunks exportés (fichiers CSV bruts)
- __Comportement à l’import__
  - Réinitialise l’état d’édition (peinture, marqueurs, overrides de chunks).
  - Lit `world.json` (si absent: conserve seed/mode actuels).
  - Lit `painted.json`, `markers.json`, `colors.json` si présents, sinon nettoie ces états.
  - Extrait tous les `maps/*.csv` sur disque (crée les dossiers si besoin), écrase les fichiers existants.
- __Comportement à l’export__
  - Sauvegarde d’abord tous les chunks modifiés.
  - Écrit `world.json` (format=2) + `painted.json` + `markers.json` + `colors.json`.
  - Ajoute tous les CSV déjà présents sous `maps/seed_<seed>[_cont]/` (récursif).
- __Notes__
  - Les fichiers dans l’archive sont stockés sans compression (stockage direct) pour la simplicité et la rapidité.
  - Les chemins sont sensibles à la casse et utilisent des `/` (style POSIX) dans l’archive.

## Import/Export CSV

- **Boutons** (en haut à droite):
  - Export: ouvre une boite de dialogue, écrit un CSV de `(GRID+1) x (GRID+1)` hauteurs.
  - Import: charge progressivement (quelques lignes par frame) pour éviter les saccades.
- Fichiers d’exemple: `map01.csv`, `map02.csv`, `map03.csv` à la racine.

Format: chaque ligne du CSV contient `GRID+1` entiers séparés par `,`, bornés par `cfg::MIN_ELEV..cfg::MAX_ELEV`.

## Détails de rendu

- Projection isométrique contrôlée par `IsoParams` (`rotDeg`, `pitch`).
- Shading Lambertien approximé via normale de cellule.
- Ombres projetées en 2D par marche discrète le long de la direction de la lumière (masque plein-grille).

## État des optimisations

- Les tentatives de cache de projection et de culling par fenêtre visible ont été **revertées** suite à des bugs rencontrés.
- Version actuelle: rendu et masque d’ombres sur l’ensemble de la grille à chaque frame (état stable).
- Pistes futures (à réintroduire prudemment):
  - Cache `map2d` avec invalidation sur édition/import/génération/changement d’iso.
  - Culling des boucles de remplissage/ombres via fenêtre d’indices dérivée de la vue.
  - Réutilisation de `sf::VertexArray` avec `reserve()` pour limiter les allocations.

## Mode procédural par chunks (expérimental)

- Le monde est découpé en **chunks 60x60** intersections (constante `cfg::CHUNK_SIZE`).
- Génération procédurale déterministe via un **FBM de value-noise** (`src/noise.*`) avec **seed global**.
- Les hauteurs sont échantillonnées en coordonnées monde (I,J), garantissant la **continuité aux frontières** de chunks.
- Intégration actuelle (mode passerelle): chaque frame, une fenêtre `(GRID+1)^2` est **repeuplée** depuis les chunks autour du centre de la vue. Le renderer reste inchangé.
- Cache de chunks avec une politique **LRU** simple, bornée par `cfg::MAX_CACHED_CHUNKS`.
- UI: bouton **Générer** ou touche **G** basculent le mode procédural. Quand OFF, la carte redevient **plate** (hauteurs=0). Une UI de seed dédiée est prévue.
  - Bouton **Re-seed** pour re-générer un seed aléatoire.
  - Champ **Seed** éditable (Entrée pour valider) pour fixer un seed déterministe.

## Dépannage

- Si l’exécutable ne démarre pas sous Windows, vérifiez que les DLLs de SFML et MinGW sont bien trouvées:
  - Utilisez `make package` pour copier automatiquement les DLLs dans `bin/`.
  - Assurez-vous que `SFML_BIN` et `MINGW_BIN` sont corrects (voir messages Makefile au build).
- Avertissement `NOMINMAX redefined`: sans conséquence (défini par l’outilchain et dans `main.cpp`).

## Capture d’écran

![Example de carte](assets/images/example.png)

Vue sur une carte d’example.

## Licence

Projet éducatif. Droits des assets/images selon leurs licences respectives.

[x] Faire un vrai LOD (Level of Detail) qui merge les cases entre elles à partir du moment où elles sont trop petites pour être visibles (dès qu'on voit la map de 300 cases par 300 cases). En temps normal vue du monde en mode infini sans brodures noirs, pas de limitation de 300 cases par 300 cases. Dézoom infini.
[x] Ajout de différent brush, carré (Chebyshev), cercle (Euclidienne), Manhattan (cross/diamond), nuage de points (gaussian).
[x] Ajout d'un slider pour la dureté du brush. Au max toutes les cases sont coloriées. Au min le brush est un point. Au milieu faire un dégradé entre les deux.
[x] Ajout de pouvoir labeliser des endroits avec un bicone de texte, on place un marqueur sur la map et on peut le nommer.
[x] Ajout d'un outil pipette de couleur.
[x] Corriger la sauvegarde par monde. Tous les chunks doivent être sauvegardés. Ainsi que la peinture. Et les modifications terrains.
[] Ajout d'un brush pour atténuer les hauteurs. Il doit garder les zone plate.
[] Ajout de la saisie de la taille du brush, pouvoir faire un brush de 1000x1000.
[] Ajout d'un outil pour peindre des lignes en pointillés.
[] Ajout d'une gimbal pour tourner la map de 90° par 90° sur les côtés, recalcule de la géométrie, des hauteurs et des ombres.
[] Ajout d'une echelle de taille de la map, noir et blanche, dynamique (1 case = 1 mètre) puis avec le merge LOD 1 case = 10 mètres puis 1 case = 100 mètres, ... et ainsi de suite.
[] Ajout d'un outil seau d'eau.
[] Ajout d'un overlay d'eau au niveau 0, et de la posibilité de pouvoir rajouter de l'eau en montagne (lac) calculé sur une zone de creux, plus on clique avec l'outil seau d'eau plus ça remplie le creux.

# Envoyer KenshiMP sur le Workshop Steam — pas à pas

Tout est prêt dans le dépôt ; il ne reste que ce qui demande **ton compte Steam**. Compte 20 à 30 min.
(Version détaillée en anglais : `UPLOAD.md`.)

## Ce qui est déjà prêt
| Quoi | Où |
|---|---|
| Le mod, prêt à envoyer | `Kenshi\mods\KenshiMP\` (copie exacte de `dist\KenshiMP\`, vérifiée par `package.bat` : « package OK ») |
| Titre | `KenshiMP — Co-op & PvP multiplayer` |
| Description anglaise (BBCode Steam) | `workshop/description_en.txt` |
| Description française | `workshop/description_fr.txt` |
| Image d'aperçu 512×512 | `workshop/preview.png` |
| Captures d'écran pour la page (4, sans curseur) | `workshop/screenshots/` |
| Notes de version | `workshop/changenotes.txt` |

## Étape 1 — Premier envoi, en caché
1. Ferme Kenshi s'il tourne. Lance Kenshi **depuis Steam** ; dans le lanceur, choisis **l'éditeur
   (FCS / Game Editor)**.
2. Ouvre le mod **KenshiMP** comme mod actif.
3. Clique **Steam Workshop** (en haut à gauche) et remplis :
   - Titre : `KenshiMP — Co-op & PvP multiplayer`
   - Description : colle tout le contenu de `workshop/description_en.txt`
   - Image : `workshop/preview.png`
   - Visibilité : **Hidden** (caché)
   - Note de version : colle `workshop/changenotes.txt`
4. Envoie. Steam ouvre (ou propose) la page de l'objet : accepte **l'accord du Workshop Steam** si
   on te le demande (sinon l'objet reste invisible).

## Étape 2 — Le numéro de l'objet (indispensable, une seule fois)
Le jeu trouve KenshiMP dans le dossier du Workshop grâce à ce numéro.
1. Sur la page de l'objet, l'adresse finit par `?id=<numéro>`. Copie ce numéro.
2. Donne-le-moi (ou écris-le seul dans `workshop/item_id.txt`, puis lance `package.bat` et copie
   `dist\KenshiMP` dans `Kenshi\mods\KenshiMP`).
3. Dans l'éditeur, refais **Steam Workshop** sur le même objet (mise à jour), avec la note
   « Workshop folder support ».
Sans cette étape, les abonnés n'auraient pas le multijoueur.

## Étape 3 — Habiller la page (depuis Steam, page de l'objet)
1. **Ajouter des images** : les fichiers de `workshop/screenshots/`.
2. **Description française** : « Modifier la description » → langue Français, colle
   `workshop/description_fr.txt` (si Steam ne le propose pas : une discussion épinglée
   « Version française »).
3. **Liens** (facultatif) : GitHub `https://github.com/xemou/KenshiMP`, RE_Kenshi sur Nexus.

## Étape 4 — Tester la version Workshop
1. Déplace `Kenshi\mods\KenshiMP` **hors** du dossier `mods` (sinon c'est la copie locale qui sert).
2. Abonne-toi à ton propre objet, laisse Steam le télécharger.
3. Lanceur → Mods → coche KenshiMP → lance une partie : le bouton **MULTIJOUEUR** doit être sous le
   menu titre, et `MyGUI.log` (dossier de Kenshi) doit contenir
   `Loading library ../../workshop/content/233860/<numéro>/KenshiMP_Loader.dll`.
4. Ouvre la fenêtre, **Héberger** : tout doit marcher comme avant.
5. Remets ton dossier local en place si tu veux (KenshiMP ne démarre qu'une fois, même avec les deux).

## Étape 5 — Rendre public
1. Le code source publié doit être celui de la DLL envoyée (licence GPL) : dis-moi « push » et je
   pousse le dépôt et l'étiquette `v18` sur GitHub.
2. Sur la page de l'objet : visibilité **Public**.
3. Note la date et le numéro en tête de la section v18 de `CHANGELOG.md` (ou demande-le-moi).

## Après la sortie
- Les rapports de bug arrivent avec le dossier `/rapport` (bouton RAPPORT DE BUG) : demande-le à
  chaque fois.
- Pour une mise à jour : je reconstruis, tu fais **Steam Workshop** → mettre à jour, avec une note de
  version (le numéro de version = celui du protocole, affiché dans le titre de la fenêtre).

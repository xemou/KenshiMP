# Test avec un ami par Steam (avant de passer l'objet en Public)

But : la première vraie partie entre deux PC. Le côté **client** n'a encore jamais été joué par un
humain (seulement par le bot), c'est le test le plus important. Comptez 1 h à 1 h 30.

## 0. Préparation (vous)
1. `package.bat` → doit finir par « package OK - KenshiMP v18 ». Puis `git tag v18` et
   `git push origin v18`.
2. Envoi sur le Workshop en **Amis uniquement** (voir `UPLOAD.md`). Notez le numéro de l'objet.
3. Retirez votre copie locale `Kenshi\mods\KenshiMP` et abonnez-vous vous-même à l'objet : vous
   testez exactement ce que votre ami reçoit.
4. Avant le rendez-vous, testez seul le rôle de **client** : `tests\test_client.bat record` puis
   `tests\test_client.bat` (le bot joue l'hôte). Tout ce qui casse là cassera chez votre ami.

## 1. Installation chez l'ami (chronométrez : c'est l'expérience d'un nouveau joueur)
1. Il s'abonne, ouvre le dossier `steamapps\workshop\content\233860\<numéro>` et lance
   **Enable KenshiMP.bat**.
2. Lanceur de Kenshi → onglet Mods → coche **KenshiMP** (même liste de mods que vous, même ordre).
3. Paramètres vidéo : **Sans bordure**.
4. Écran titre : le bouton **MULTIJOUEUR** est là, F4 ouvre la fenêtre, le titre affiche **v18**.
- Notez tout ce qui n'est pas clair : c'est ce qu'il faudra corriger dans le guide.

## 2. Connexion
| # | Action | Attendu |
|---|---|---|
| 1 | Vous : F4 → Héberger, chargez une partie | « Hébergé sur le port 47000 » |
| 2 | Vous : AMIS STEAM → INVITER à côté de l'ami | il reçoit l'invitation Steam |
| 3 | Lui : accepte, lance une **nouvelle partie** | message « Connecté », vous voyez « X a rejoint la partie » |
| 4 | Lui : attend ~20 s | rappel « Vous êtes loin de … : F4 > Aller » |
| 5 | Lui : F4 → **ALLER** sur votre ligne | son escouade arrive à côté de la vôtre, chat « X a rejoint vous » chez vous, pas d'alerte « triche » |
| 6 | Les deux | nom de l'autre affiché au-dessus de ses persos, en couleur ; ping affiché dans F4 |

Si l'invitation Steam ne marche pas : essayez par IP avec un VPN (Radmin VPN) pour séparer
« problème Steam » et « problème du mod ».

## 3. Jeu (10 min chacun)
| # | Action | Attendu |
|---|---|---|
| 1 | Marcher, courir côte à côte, entrer dans un bâtiment | mouvements fluides, pas de téléportations, bon étage |
| 2 | Chat (Entrée), `/joueurs` | messages des deux côtés, couleurs |
| 3 | Lui : ouvre un **coffre de ville** | même contenu que chez vous ; ce qu'il prend disparaît chez vous |
| 4 | Commerce avec un marchand de la ville (lui) | stock et argent du marchand identiques des deux côtés |
| 5 | Bouton **ÉCHANGE** entre vous deux | fenêtre d'échange des deux côtés, objets transférés une seule fois |
| 6 | Combat contre des PNJ ensemble | les PNJ meurent / tombent KO pareil chez les deux |
| 7 | **GUERRE**, un combat entre vous, puis **PAIX** | dégâts appliqués une fois, KO visible des deux côtés |
| 8 | Lui : casse une porte de ville ou tue un PNJ unique | changement visible chez vous (et reste après rechargement) |
| 9 | Construction d'un bâtiment chacun | visible chez l'autre |
| 10 | Vous : pause (Espace) | pause chez lui aussi |

## 4. Robustesse
| # | Action | Attendu |
|---|---|---|
| 1 | Lui : sauvegarde | enregistrée sous `<nom>_MP`, sa partie solo intacte |
| 2 | Lui : quitte (F4 → Quitter) puis rejoint | il retrouve le **même numéro** de joueur, ses persos réapparaissent une seule fois |
| 3 | Vous : rechargez votre sauvegarde | lui est resynchronisé (PNJ, heure, météo) sans redémarrer |
| 4 | Lui : coupe le Wi-Fi 20 s | « déconnecté », puis reconnexion automatique |
| 5 | Vous : quittez Kenshi, relancez, réhébergez | il revient avec le même numéro (`players.cfg`) |

## 5. Ce qu'il faut récupérer (les deux joueurs)
- **Dès qu'un problème arrive** : tapez `/rapport` dans le chat (ou F4 → RAPPORT DE BUG). Un dossier
  `KenshiMP_report_<date>` apparaît sur le Bureau, avec les journaux, les réglages et l'état de la
  session à ce moment-là. Notez en une phrase ce qui s'est passé.
- À la fin de la partie, un dernier `/rapport` chacun, même si tout s'est bien passé.
- Une capture d'écran si le problème est visuel.

Si tout passe : objet en **Public** (`UPLOAD.md`), date et numéro de l'objet dans `CHANGELOG.md`.

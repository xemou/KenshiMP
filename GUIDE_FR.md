# KenshiMP — Guide (français)

Mod multijoueur coopératif / compétitif pour Kenshi : chaque joueur a sa propre escouade, sa
faction et ses villes, et les factions des joueurs peuvent se faire la guerre. L'hôte possède le
monde partagé (PNJ, horloge, pause).

*Traduction de `GUIDE.md` ; en cas de différence, la version anglaise fait foi.*

## 1. Prérequis (chaque joueur)
- Kenshi sur **Steam**, tel que Steam l'installe (1.0.68) : **rien d'autre** (voir §2).
- Ou Kenshi Steam/GOG avec **RE_Kenshi 0.3.5** (RE_Kenshi fait tourner le jeu en 1.0.65). Les
  joueurs GOG ont besoin de RE_Kenshi : KenshiMP n'a pas encore sa propre table d'adresses pour
  l'exécutable GOG. Avec RE_Kenshi : **NVIDIA PhysX System Software 9.10.0513** installé et les DLL
  PhysX de Kenshi copiées à côté de `Kenshi\RE_Kenshi\Kenshi_x64.exe` (PhysXCore64,
  PhysXCooking64, PhysXDevice64, PhysXLoader64, physxcudart_20, cudart64_30_9, NxCharacter), sinon
  le jeu plante au lancement d'une partie.
- Les joueurs avec et sans RE_Kenshi peuvent jouer ensemble.
- **Exactement la même liste de mods**, dans le même ordre (l'hôte refuse sinon, voir
  `strict_mods`), et la **même version de KenshiMP**.

## 2. Installation
1. Copiez le dossier `dist\KenshiMP` dans `Kenshi\mods\` (ou abonnez-vous sur le Workshop Steam).
2. **Sans RE_Kenshi** : lancez une fois **`Enable KenshiMP.bat`** dans ce dossier (pour le Workshop :
   `steamapps\workshop\content\233860\<numéro de l'objet>`). Il ajoute le chargeur de KenshiMP aux
   plugins que charge le jeu (`Plugins_x64.cfg`, une copie de sauvegarde est gardée) ; les mises à
   jour du Workshop ne demandent ensuite rien de plus. `Disable KenshiMP.bat` le retire. Les
   sauvegardes solo ne sont pas touchées. Le chargeur écrit `KenshiMP_loader.log` dans le dossier
   de Kenshi (par exemple « not supported » sur une version du jeu inconnue : le jeu démarre alors
   simplement sans multijoueur). Avec RE_Kenshi installé, cette étape est inutile.
3. Dans le lanceur de Kenshi, onglet Mods : cochez **KenshiMP**.
4. Les réglages sont dans `%LOCALAPPDATA%\kenshi\KenshiMP\kenshimp.cfg` (créé au premier lancement
   à partir de la copie du mod ; voir §4). La fenêtre Multijoueur (F4) suffit dans la plupart des cas.
5. Conseillé : lanceur → Paramètres vidéo → **Sans bordure** (le plein écran exclusif met le jeu en
   pause sur Alt+Tab → votre escouade se fige pour les autres joueurs).

## 3. Jouer
**Le plus simple : la fenêtre Multijoueur.** Un bouton **MULTIJOUEUR** se trouve sous le menu de
l'écran titre et sous le menu pause (Échap), ou appuyez sur **F4** à tout moment. Le mod parle la
langue du jeu :
1. Remplissez **Nom** (votre nom) et **Faction** (le nom de votre faction vu par les autres).
2. **Hôte** : cliquez **Héberger**. La fenêtre affiche les adresses IP de ce PC (réseau local / VPN)
   à donner à vos amis ; lancez ensuite ou chargez une partie.
3. **Client** : entrez l'**adresse de l'hôte** (son IP), le **port** et le **mot de passe** s'il y en
   a un, cliquez **Rejoindre**, puis lancez une partie (une partie neuve dédiée au multijoueur est
   conseillée).
4. La fenêtre affiche l'état (connecté, ping, liste des joueurs) ou la raison d'un refus : mods
   manquants / en trop / dans un autre ordre, mauvais mot de passe, hôte injoignable…
5. **Quitter** termine la session (pas de reconnexion automatique). Vos choix sont enregistrés dans
   `kenshimp.cfg` : au prochain lancement, la connexion se refait toute seule.

**Par Steam (le plus simple par Internet, copies Steam de Kenshi) :** ni adresse IP, ni port à
ouvrir, ni VPN. L'hôte ouvre la fenêtre Multijoueur, appuie sur **Héberger**, puis **AMIS STEAM** et
**INVITER** à côté d'un ami (ou l'ami utilise **Rejoindre la partie** sur le profil Steam de l'hôte).
L'ami accepte l'invitation et le jeu se connecte par le réseau de Steam (en direct si possible,
sinon par les relais de Steam). Les copies GOG gardent la connexion par IP ci-dessous.

6. **Diplomatie et échanges** : une ligne par joueur avec **GUERRE / PAIX / ALLIÉ / ÉCHANGE**.
   Pour échanger, amenez un de vos persos à côté d'un des siens et cliquez **ÉCHANGE** ; l'autre
   joueur reçoit un message et clique **ACCEPTER** sur votre ligne. La fenêtre d'échange du jeu
   s'ouvre des deux côtés : glissez les objets d'un inventaire à l'autre. Échap termine l'échange.
   Les mêmes boutons apparaissent aussi dans l'écran Factions du jeu quand la faction d'un autre
   joueur est sélectionnée.

Tout peut aussi se régler à la main dans `kenshimp.cfg` :
- **Hôte** : `mode=host`, lancez une partie (nouvelle ou chargée). Le port TCP (47000 par défaut)
  doit être joignable : redirection de port sur votre box, ou VPN de jeu (ZeroTier, Tailscale,
  Radmin VPN — le plus simple). Autorisez Kenshi dans le pare-feu Windows quand il le demande.
- **Client** : `mode=join`, `address=<IP de l'hôte>`, lancez une partie. Il se connecte tout seul,
  et se reconnecte tout seul si la connexion tombe.
- **F9** : déclarer la guerre · **F10** : faire la paix — avec le joueur dont le perso est
  sélectionné, sinon avec tous les joueurs.
- **Entrée** : ouvrir le chat (Entrée pour envoyer, Échap pour annuler). Les derniers messages
  restent affichés au-dessus (le nom de chaque joueur dans sa couleur) puis s'effacent ; ouvrir le
  chat les réaffiche. Commandes : `/players` (liste), `/war <joueur|all>`, `/peace <joueur|all>`,
  `/ally <joueur|all>`, `/goto [joueur]` (ou `/aller`), `/rapport` (le début du nom suffit).
- **Rapport de bug** : `/rapport` dans le chat, ou **RAPPORT DE BUG** dans la fenêtre Multijoueur,
  crée un dossier sur votre Bureau (`KenshiMP_report_<date>`) avec les journaux, vos réglages (sans
  le mot de passe) et un résumé de la session. Compressez-le en zip et envoyez-le avec quelques mots
  sur ce qui s'est passé.
- **Rejoindre les autres** : votre partie garde votre escouade là où votre sauvegarde l'a laissée.
  **ALLER** à côté d'un joueur dans la fenêtre Multijoueur (ou `/aller <joueur>`, `/aller` seul =
  l'hôte) envoie vos persos sélectionnés (tous si aucun n'est sélectionné) à côté des persos de ce
  joueur. Impossible pendant qu'un d'eux se bat ou vers un joueur avec qui vous êtes en guerre ; les
  persos KO restent sur place. Un client qui arrive loin de l'hôte reçoit un rappel ~20 s après le
  chargement.
- **Objets** : vous pouvez fouiller le corps (KO/mort) d'un autre joueur ou d'un PNJ, commercer avec
  les marchands de l'hôte et déposer / prendre dans les coffres des autres joueurs. Le propriétaire
  valide (objet toujours là, preneur à moins de 30 unités) et l'objet n'existe jamais en double.
- **Coffres des villes** (coffres, rangements du monde) : dans le monde de l'hôte, ils ont le même
  contenu pour tout le monde, celui de l'hôte. Quand un client en ouvre un, sa copie est remplacée par
  celle de l'hôte, et ce qu'il prend ou dépose passe par l'hôte : un objet pris par un joueur
  disparaît pour tous.
- Vous ne pouvez pas porter ni mettre en cage le perso d'un autre joueur (le fouiller quand il est
  KO est permis ; en ville, les gardes y voient un vol sauf si vous êtes en guerre avec lui) ;
  porter un PNJ du monde le rend vôtre (l'hôte vous le cède).
- Impossible de construire à moins de 3 unités d'un bâtiment d'un autre joueur.
- La vitesse est bloquée à x1. Seul l'hôte peut mettre en pause (pour tout le monde).
- **Sauvegardes des clients** : elles vont dans `<nom>_MP` ; votre partie solo n'est pas touchée.
  L'hôte sauvegarde normalement : sa sauvegarde contient le monde partagé.
- **Numéros de joueur** : l'hôte donne un numéro à chaque joueur (il nomme sa faction et ses persos
  dans la sauvegarde de l'hôte). L'hôte les retient dans `players.cfg` (à côté de `kenshimp.cfg`) :
  un ami qui revient, même après un redémarrage de Kenshi chez l'hôte, retrouve le même numéro s'il
  garde le même **Nom**.

## 4. Options de `kenshimp.cfg`
| Option | Défaut | Rôle |
|---|---|---|
| mode | off | off / host / join (réglé par la fenêtre Multijoueur) |
| address, port | 127.0.0.1, 47000 | adresse de l'hôte (client), port TCP |
| name, faction | | nom affiché, nom de votre faction vu par les autres |
| relation | 0 | relation de départ entre joueurs (-100 guerre … 100 alliés) |
| ghost_ai | none | none = les persos des autres joueurs ne font que ce que fait leur propriétaire |
| sync_appearance / equipment / buildings | 1 | synchro de l'apparence, de l'équipement, des villes |
| npc_sync | 1 | le monde de l'hôte est partagé (les PNJ locaux du client sont coupés) |
| pause_sync | 1 | pause contrôlée par l'hôte |
| strict_mods | 1 | refuser les joueurs dont les mods diffèrent |
| auto_reconnect | 1 | reconnexion automatique du client |
| weather_sync | 1 | la météo de l'hôte s'impose à tous |
| town_sync | 1 | les états du monde de l'hôte (PNJ uniques tués / emprisonnés) et les changements de villes qu'ils entraînent (détruites, abandonnées, prises) s'appliquent à tous ; une ville changée montre sa nouvelle version quand sa zone se charge ; les portes de ville cassées ou réparées et les bâtiments de ville détruits par quiconque sont les mêmes pour tous |
| lobby_key | F4 | touche de la fenêtre Multijoueur (F1…F12 ; F6 est pris par le mod CheatMenu) |
| language | auto | langue du mod : auto (celle du jeu), fr ou en |
| show_players_on_map | 1 | escouades des autres joueurs sur la carte du monde (sauf en guerre) |
| ghost_no_collide | 0 | expérimental : les persos des autres joueurs ne sont plus poussés par les vôtres (moins de sauts de rattrapage dans une foule) ; pas encore essayé en jeu |
| load_sharing | 0 | (hôte, expérimental) partage de charge : un joueur à plus de 3000 unités de l'hôte fait tourner le monde autour de lui sur son propre PC et l'hôte cesse de simuler cette région ; revenu à moins de 2000 unités, l'hôte reprend la main (un seul monde partagé). Quand vous vous retrouvez, les PNJ autour du client sont remplacés par ceux de l'hôte |
| assault_hostility | 0 | comme les factions de Kenshi : un joueur dont les persos blessent les vôtres hors guerre perd de l'estime chez vous (-25 par agression, au plus une toutes les 3 s) ; sous zéro c'est la guerre |
| render_smoothing | 1 | modèle des autres joueurs dessiné sur le chemin lissé |
| password | (vide) | mot de passe de la partie (identique chez l'hôte et les clients) |
| debug_keys | 0 | tests seulement : F11 bâtiment, F8 horloge +3 h, F3 caméra sur un autre joueur, F1 attaquer le premier perso d'un autre joueur, F2 prendre le premier objet d'un inventaire ouvert, F5 recharger la sauvegarde « kmptest » ; journal coup par coup |

## 5. Dépannage
| Symptôme | Cause / solution |
|---|---|
| Plantage « PhysX start failure » au lancement d'une partie (avec RE_Kenshi) | installez PhysX 9.10.0513 + copiez les DLL PhysX (§1) |
| Pas de bouton MULTIJOUEUR, rien dans les journaux (sans RE_Kenshi) | relancez `Enable KenshiMP.bat` (après avoir déplacé le mod, ou si un autre outil a réécrit `Plugins_x64.cfg`) |
| `KenshiMP_loader.log` : « not supported without RE_Kenshi » | Kenshi a été mis à jour par Steam (nouvel exécutable) : attendez une mise à jour de KenshiMP, ou installez RE_Kenshi |
| Journal : « Incorrect address in KenshiLib::GetRealAddress » | DLL compilée sans /GL /LTCG (utilisez `package.bat`) |
| « connection … timed out / refused » | hôte non lancé, mauvaise IP, port fermé (box / pare-feu) → VPN de jeu |
| « Your mods must match the host's » | alignez la liste de mods (même ordre) ou `strict_mods=0` chez l'hôte |
| « KenshiMP version mismatch » | installez la même version de KenshiMP partout |
| L'escouade d'un joueur est figée | il a fait Alt+Tab en plein écran exclusif → mode sans bordure |
| « timed out (no data for 15 s) » | connexion perdue ; le client se reconnecte tout seul |
| Où lire les journaux | `Kenshi\RE_Kenshi_log.txt` (lignes « KenshiMP: », écrites avec ou sans RE_Kenshi) et `Kenshi\KenshiMP_loader.log` |

## 5b. Fluidité (comme un serveur de jeu)
- Les joueurs envoient leur état **20 fois par seconde** (tick à 20 Hz), horodaté avec leur propre
  horloge.
- Chaque machine rejoue les autres avec ~100-150 ms de retard (adapté à la gigue mesurée), en
  **interpolant** entre deux états, en **extrapolant** avec la vitesse si un paquet est en retard, et
  en **lissant les corrections** (pas de sauts). Validé hors-ligne : sur une connexion 40-190 ms avec
  des pics jusqu'à +400 ms, la vitesse apparente reste ≤ 8,1 u/s pour 6 u/s réels, sans image figée
  (ancienne méthode : sauts à 108 u/s, 84 % d'images figées).
- En jeu, le moteur fait **marcher / courir** le fantôme vers un point devant lui sur le chemin
  (vraies animations), et une petite correction par image comble l'écart restant. Mesuré avec le
  bot : écart moyen ~1-1,6 unité, 0-2 téléportations par 30 s (contre 248 avant).
- PNJ : la fréquence dépend de la distance au joueur le plus proche (10 Hz < 80, 5 Hz < 250, 2 Hz
  au-delà) et rien n'est envoyé pour un PNJ immobile (rafraîchi toutes les 2 s).
- Bande passante mesurée : ~2 Ko/s par joueur avec une petite escouade ; comptez ~20 Ko/s pour une
  escouade de 10 et ~20-40 Ko/s de PNJ par client en ville (l'hôte multiplie par le nombre de clients).
- **F7** affiche le ping, les débits entrant / sortant, le nombre de fantômes et les états sautés
  (aussi dans le journal toutes les 30 s, avec la qualité du suivi des fantômes).

## 6. Ce qui est synchronisé
Persos des joueurs (position, vitesse, apparence, équipement, statistiques, santé de chaque membre,
sang, KO/mort, combat avec animations, tâches visibles : construction, machines, lits, tourelles,
assis…), dégâts (calculés par l'attaquant, appliqués par le propriétaire de la victime), factions
des joueurs et guerre/paix, relations avec les factions du monde, villes (bâtiments, construction,
destruction, portes cassées/verrouillées, dégâts aux portes, démontage), PNJ et animaux du monde de
l'hôte autour de chaque joueur, horloge, pause, recrutement d'un PNJ par un client (il devient celui
du client), étage où se trouve chaque perso, **inventaires** (sacs des persos, **sacs à dos portés**,
coffres des joueurs, marchands et PNJ de l'hôte, objets au sol) avec transferts validés par le
propriétaire, **échange direct** entre joueurs, **faim**, **membres coupés / écrasés / prothèses**,
**météo** de chaque région, **chat**, **états du monde** de l'hôte (PNJ uniques tués ou emprisonnés)
et les **changements de villes** qu'ils entraînent, **portes de ville cassées** et **bâtiments de
ville détruits**, **primes** pour les crimes vus dans le monde de l'hôte.
Les ajouts marqués 🧪 dans `RISKS.md` ont été codés et testés hors-ligne (protocole, compilation,
imports) mais doivent encore être **validés en jeu**.

## 7. Limites connues (non synchronisé aujourd'hui)
- **Sac à dos posé au sol** : le sac est partagé, mais pas son contenu tant qu'il est par terre
  (videz-le d'abord, ou donnez-le par un échange).
- **Deux joueurs qui prennent le même objet dans la même seconde** : l'hôte n'en retire qu'un, le
  second garde une copie (rare).
- **Argent des marchands** : synchronisé avec le vrai marchand chez l'hôte pendant l'échange —
  nouveau, à confirmer en jeu.
- **Production** (fermes, mines, recherche) : simulée seulement chez le propriétaire.
- **Dialogues** avec un PNJ fantôme : joués localement chez le client.
- **Pas de changement d'hôte** : si l'hôte part, les clients retrouvent leur monde local.
- **Demi-tours serrés** : le fantôme peut avoir jusqu'à ~1 s de retard pendant que le moteur le
  retourne, puis il rattrape en glissant (téléportation seulement au-delà de 20 unités).
- **États** (ivresse, faim) : non copiés sur les fantômes.
- **Deux persos très proches** (< 2 unités) se gênent physiquement et le suivi est moins précis.

## 8. Risques futurs anticipés et protections en place
| Risque | Protection |
|---|---|
| Plantage d'un joueur / coupure réseau | détecté en 15 s, ses fantômes nettoyés, reconnexion automatique |
| Joueur lent / connexion saturée | les positions périmées sont sautées ; au-delà de 8 Mo en attente il est déconnecté (pas de fuite mémoire) |
| Données corrompues / NaN / valeurs absurdes / triche | positions, rotations, dégâts (plafonnés), statistiques validés ; nombre de fantômes plafonné par joueur |
| Versions ou mods différents | refus explicite à la connexion |
| L'hôte recharge une autre sauvegarde | détection, pointeurs du moteur invalidés, purge et resynchro complète |
| Fantômes restés dans une sauvegarde | purgés à chaque chargement |
| Client connecté pendant l'écran titre / un chargement | messages du monde ignorés puis resynchro une fois le monde prêt |
| Client qui abîme sa sauvegarde solo | sauvegardes redirigées vers `_MP` |
| Client qui met en pause seul (désynchro) | pause imposée par l'hôte |
| Client loin de l'hôte (zone non simulée) | l'hôte garde chargées les zones autour des clients |
| Recruter un fantôme (doublon) | PNJ : propriété transférée au client ; perso d'un joueur : bloqué |
| F9/F10 pressés dans une autre application | ignorés si Kenshi n'a pas le focus |
| Mise à jour de Kenshi / RE_Kenshi | le chargeur reconnaît l'exécutable du jeu et refuse un inconnu (le jeu démarre sans multijoueur, rien ne casse) ; une nouvelle table d'adresses se fait avec `tools/rva/make_table.ps1` ; l'horloge et la table des états du monde sont localisées dynamiquement |
| Antivirus qui bloque la DLL injectée | ajoutez une exception pour le dossier de Kenshi |

## 9. Tester sans second PC
Il faut **une copie de Kenshi par joueur** (le partage familial Steam ne permet pas de jouer au même
jeu en même temps, et Steam refuse deux instances sur un même PC). Pour tester seul, le bot
`tests\out\bot.exe` joue le second joueur : il copie votre perso à côté de vous (sans bras gauche,
pour tester les membres), reflète votre inventaire, écrit dans le chat et répond à vos messages,
donne puis reprend un objet, déclare la guerre puis fait la paix. Avec `--host-replay` il joue
l'hôte et rejoue un monde enregistré (PNJ, inventaires, météo, horloge).
- `tests\build_and_run.bat` : tests réseau hors-ligne (connexion, relais, dégâts, délais, client
  lent, trames corrompues, mods/versions, validation des données).
- `tests\build_bot.bat` puis `tests\out\bot.exe 127.0.0.1 47000 600 4` pendant que vous hébergez :
  un faux 2e joueur qui renvoie votre escouade décalée de 4 m (pour tester fantômes, combat, dégâts).
- `bot.exe 127.0.0.1 47000 170 4 stream.rec` enregistre le flux de PNJ de l'hôte ;
  `bot.exe --host-replay stream.rec 47000` le rejoue en hébergeant, avec Kenshi en `mode=join`
  (pour tester le côté client).

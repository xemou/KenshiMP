# KenshiMP — Guide (FR)

Mod multijoueur coopératif/compétitif pour Kenshi : chaque joueur a son escouade, sa faction et
ses villes ; les factions des joueurs peuvent se faire la guerre. L'hôte possède le monde partagé
(PNJ, heure, pause).

## 1. Prérequis (chaque joueur)
- Kenshi **Steam ou GOG 1.0.65 / 1.0.68** (pas de copie modifiée/crackée : RE_Kenshi refuse).
- **RE_Kenshi 0.3.5** installé avec son installateur.
- **NVIDIA PhysX System Software 9.10.0513** installé (sinon « PhysX start failure » au lancement
  d'une partie), et les DLL PhysX de Kenshi copiées à côté de `Kenshi\RE_Kenshi\Kenshi_x64.exe`
  (PhysXCore64, PhysXCooking64, PhysXDevice64, PhysXLoader64, physxcudart_20, cudart64_30_9,
  NxCharacter). Sans ça, crash au démarrage d'une partie.
- **Exactement la même liste de mods**, dans le même ordre (l'hôte refuse sinon, voir
  `strict_mods`), et la **même version de KenshiMP**.

## 2. Installation
1. Copier le dossier `dist\KenshiMP` dans `Kenshi\mods\`.
2. Dans le lanceur Kenshi, onglet Mods : cocher **KenshiMP**.
3. Les réglages sont dans `%LOCALAPPDATA%\kenshi\KenshiMP\kenshimp.cfg` (créé au premier lancement à partir de celui du mod ; voir §4). La fenêtre Multijoueur (F4) suffit dans la plupart des cas.
4. Conseillé : lanceur → Configuration vidéo → **Sans bordure** (le plein écran exclusif met le
   jeu en pause quand on fait Alt+Tab → votre escouade se fige chez les autres).

## 3. Jouer
**Le plus simple : la fenêtre Multijoueur.** Bouton **MULTIJOUEUR** sous le menu de l'écran titre et
sous le menu pause (Échap), ou touche **F4** à tout moment. Le mod parle la langue du jeu :
1. Remplir **Name** (votre nom) et **Faction** (nom de votre faction chez les autres).
2. **Hôte** : cliquer **Host**. La fenêtre affiche les adresses IP de ce PC (réseau local / VPN) à
   donner aux amis, puis lancer ou charger une partie.
3. **Client** : saisir **Host address** (IP de l'hôte), le **Port** et le **Password** éventuel,
   cliquer **Join**, puis lancer une partie (une partie neuve dédiée au multijoueur est conseillée).
4. La fenêtre indique l'état (connecté, ping, liste des joueurs) ou la raison d'un refus : mods
   manquants / en trop / ordre différent, mauvais mot de passe, hôte injoignable…
5. **Leave** quitte la session (sans reconnexion automatique). Les choix sont enregistrés dans
   `kenshimp.cfg` : au prochain lancement, la connexion se refait toute seule.
6. **Diplomatie et échange** : une ligne par joueur avec **GUERRE / PAIX / ALLIÉ / ÉCHANGER**.
   Pour échanger, amenez un de vos persos à côté d'un des siens et cliquez **ÉCHANGER** ; l'autre
   joueur reçoit un message et clique **ACCEPTER** sur votre ligne. La fenêtre d'échange du jeu
   s'ouvre chez vous deux : glissez les objets d'un inventaire à l'autre. Échap termine l'échange.

On peut aussi tout régler à la main dans `kenshimp.cfg` :
- **Hôte** : `mode=host`, lancer une partie (nouvelle ou chargée). Le port TCP (47000 par défaut)
  doit être ouvert : redirection de port sur la box, ou un VPN de jeu (ZeroTier, Tailscale,
  Radmin VPN — le plus simple). Autoriser Kenshi dans le pare-feu Windows à la première demande.
- **Client** : `mode=join`, `address=<IP de l'hôte>`, lancer une partie. Il se connecte tout
  seul, et se reconnecte tout seul si la connexion tombe.
- **F9** : déclarer la guerre · **F10** : faire la paix — au joueur dont un personnage est
  sélectionné, sinon à tous les joueurs.
- **Entrée** : ouvrir le chat (Entrée pour envoyer, Échap pour annuler). Les derniers messages restent
  affichés au-dessus (nom de chaque joueur dans sa couleur) puis s'effacent ; ouvrir le chat les réaffiche.
  Commandes (en français ou en anglais) :
  `/players` (liste), `/war <joueur|all>`, `/peace <joueur|all>`, `/ally <joueur|all>` (le début du
  nom suffit).
- **Objets** : on peut piller le corps (KO/mort) d'un autre joueur ou d'un PNJ, commercer avec les
  marchands de l'hôte et déposer/prendre dans les coffres des autres. Le propriétaire valide
  (objet encore présent, preneur à moins de 30 unités) et l'objet n'existe qu'une fois.
- On ne peut pas porter ou mettre en cage le personnage d'un autre joueur (le piller quand il est KO est permis ; en ville, les gardes le voient comme un vol, sauf si vous êtes en guerre avec lui) ;
  porter un PNJ du monde en fait le vôtre (l'hôte vous le cède).
- On ne peut pas construire à moins de 3 unités d'un bâtiment d'un autre joueur.
- La vitesse est bloquée à x1. Seul l'hôte peut mettre en pause (pour tout le monde).
- **Sauvegardes du client** : elles vont dans `<nom>_MP` ; votre partie solo n'est pas modifiée.
  L'hôte sauvegarde normalement : c'est sa sauvegarde qui contient le monde partagé.

## 4. Options de `kenshimp.cfg`
| Option | Défaut | Rôle |
|---|---|---|
| mode | host | off / host / join |
| address, port | 127.0.0.1, 47000 | adresse de l'hôte (client), port TCP |
| name, faction | | nom affiché, nom de votre faction chez les autres |
| relation | 0 | relation de départ entre joueurs (-100 guerre … 100 alliés) |
| ghost_ai | none | none = les persos des autres ne font que ce que fait leur propriétaire |
| sync_appearance / equipment / buildings | 1 | synchronisation de l'apparence, de l'équipement, des villes |
| npc_sync | 1 | monde de l'hôte partagé (les PNJ locaux du client sont désactivés) |
| pause_sync | 1 | pause contrôlée par l'hôte |
| strict_mods | 1 | refuser les joueurs dont les mods diffèrent |
| auto_reconnect | 1 | reconnexion automatique du client |
| weather_sync | 1 | météo de l'hôte imposée à tous |
| lobby_key | F4 | touche de la fenêtre Multijoueur (F1…F12 ; F6 est prise par le mod CheatMenu) |
| language | auto | langue du mod : auto (celle du jeu), fr ou en |
| show_players_on_map | 1 | escouades des autres joueurs sur la carte du monde (sauf en guerre) |
| ghost_no_collide | 0 | expérimental : les persos des autres joueurs ne sont plus poussés par vos persos (moins de sauts de rattrapage dans une foule) ; pas encore essayé en jeu |
| load_sharing | 0 | (hôte, expérimental) partage de charge : un joueur à plus de 3000 unités de l'hôte fait tourner le monde autour de lui sur son propre PC, l'hôte arrête de simuler cette région ; revenu à moins de 2000 unités, l'hôte reprend la main (un seul monde partagé). Quand vous vous rejoignez, les PNJ autour du client sont remplacés par ceux de l'hôte |
| render_smoothing | 1 | modèle des autres joueurs dessiné sur la trajectoire lissée |
| password | (vide) | mot de passe de la partie (identique chez l'hôte et les clients) |
| debug_keys | 0 | tests seulement : F11 bâtiment, F8 horloge +3 h, F3 caméra sur un autre joueur, F1 attaquer le 1er perso d'un autre joueur, F2 prendre le 1er objet d'un inventaire ouvert, F5 recharger la sauvegarde « kmptest » ; journal coup par coup |

## 5. Dépannage
| Symptôme | Cause / solution |
|---|---|
| Crash « PhysX start failure » en lançant une partie | installer PhysX 9.10.0513 + copier les DLL PhysX (§1) |
| Journal : « Incorrect address in KenshiLib::GetRealAddress » | DLL compilée sans /GL /LTCG (utiliser `package.bat`) |
| « connection … timed out / refused » | hôte pas lancé, mauvaise IP, port fermé (box/pare-feu) → VPN de jeu |
| « Your mods must match the host's » | aligner la liste de mods (même ordre) ou `strict_mods=0` chez l'hôte |
| « KenshiMP version mismatch » | installer la même version de KenshiMP partout |
| Escouade d'un joueur figée | il a fait Alt+Tab en plein écran exclusif → mode sans bordure |
| « timed out (no data for 15 s) » | connexion perdue ; le client se reconnecte seul |
| Où lire les logs | `Kenshi\RE_Kenshi_log.txt`, lignes « KenshiMP: » |

## 5 bis. Fluidité (comme un serveur de jeu)
- Les joueurs envoient leur état **20 fois/s** (tick 20 Hz), horodaté avec leur horloge.
- Chaque machine rejoue les autres avec ~100-150 ms de retard (adapté à la gigue mesurée) en
  **interpolant** entre deux états, **extrapole** avec la vitesse si un paquet est en retard, et
  **lisse les corrections** (pas de saut). Validé hors jeu : sur une connexion à 40-190 ms avec des
  pics à +400 ms, la vitesse apparente reste ≤ 8,1 u/s pour 6 u/s réels et aucune image figée
  (l'ancienne méthode : sauts à 108 u/s, 84 % d'images figées).
- En jeu, le moteur fait **marcher/courir** le fantôme vers un point devant lui sur la trajectoire
  (vraies animations), et une petite correction par image referme l'écart restant. Mesuré avec
  le bot : écart moyen ~1-1,6 unité, 0-2 téléportations par 30 s (contre 248 avant).
- PNJ : fréquence selon la distance au joueur le plus proche (10 Hz < 80, 5 Hz < 250, 2 Hz
  au-delà) et rien n'est envoyé pour un PNJ immobile (rafraîchi toutes les 2 s).
- Débit mesuré : ~2 Ko/s par joueur à escouade réduite ; compter ~20 Ko/s pour une escouade de
  10 et ~20-40 Ko/s de PNJ par client en ville (l'hôte multiplie par le nombre de clients).
- **F7** affiche ping, débit entrant/sortant, nombre de fantômes et états sautés (aussi dans le
  journal toutes les 30 s, avec la qualité de suivi des fantômes).

## 6. Ce qui est synchronisé
Personnages des joueurs (position, vitesse, apparence, équipement, stats, santé de chaque membre,
sang, KO/mort, combat avec animations, tâches visuelles : construire, machines, lits, tourelles,
s'asseoir…), dégâts (calculés par l'attaquant, appliqués par le propriétaire de la victime),
factions des joueurs et guerre/paix, relations avec les factions du monde, villes (bâtiments,
construction, destruction, portes cassées/verrouillées, dégâts aux portes, démontage), PNJ et
animaux du monde de l'hôte autour de chaque joueur, heure, pause, recrutement d'un PNJ par un
client (il passe chez ce client), étage où se trouve chaque perso, **inventaires** (sac des persos,
**sacs à dos portés**, coffres des joueurs, marchands et PNJ de l'hôte, objets posés au sol) avec
transferts validés par le propriétaire, **échange direct** entre joueurs, **faim**,
**membres coupés / écrasés / prothèses**, **météo** de chaque région, **chat**.
Les ajouts marqués 🧪 dans `RISQUES_MULTIJOUEUR.md` ont été codés et testés hors jeu (protocole,
compilation, imports) mais restent **à valider en jeu**.

## 7. Limites connues (non synchronisé aujourd'hui)
- **Sac à dos posé au sol** : le sac est partagé, mais pas ce qu'il contient tant qu'il est par terre (videz-le avant, ou donnez-le par un échange).
- **Deux joueurs qui pillent le même objet à la même seconde** : l'hôte n'en retire qu'un, le
  second pillard garde une copie (rare).
- **Argent des marchands** : synchronisé avec le vrai marchand chez l'hôte pendant l'échange — nouveauté à confirmer en jeu.
- **Production** (fermes, mines, recherche) : simulée chez le propriétaire seulement.
- **Dialogues** avec un PNJ fantôme : joués localement chez le client.
- **Pas de migration d'hôte** : si l'hôte quitte, les clients retrouvent leur monde local.
- **Demi-tours brusques** : le fantôme peut prendre jusqu'à ~1 s de retard le temps que le moteur
  le fasse tourner, puis rattrape en glissant (téléportation seulement au-delà de 20 unités).
- **États** (ivresse, faim) : non copiés sur les fantômes.
- **Deux personnages très proches** (< 2 unités) se gênent physiquement et le suivi est moins
  précis.

## 8. Risques futurs anticipés et parades en place
| Risque | Parade |
|---|---|
| Joueur qui plante / coupure réseau | détection en 15 s, nettoyage de ses fantômes, reconnexion automatique |
| Joueur lent / connexion saturée | les positions périmées sont sautées ; au-delà de 8 Mo en attente il est déconnecté (pas de fuite mémoire) |
| Données corrompues / NaN / valeurs absurdes / tricherie | validation de toutes les positions, rotations, dégâts (plafonnés), stats ; plafond de fantômes par joueur |
| Versions ou mods différents | refus explicite à la connexion |
| Hôte qui recharge une autre sauvegarde | détection, invalidation des pointeurs moteur, purge et resynchronisation complète |
| Fantômes restés dans une sauvegarde | purgés à chaque chargement |
| Client connecté pendant l'écran titre / un chargement | messages du monde ignorés puis resynchronisation quand le monde est prêt |
| Client qui abîmerait sa sauvegarde solo | sauvegardes redirigées vers `_MP` |
| Client qui met en pause seul (désynchronisation) | pause imposée par l'hôte |
| Client loin de l'hôte (zone non simulée) | l'hôte garde chargées les zones autour des clients |
| Recrutement d'un fantôme (doublon) | PNJ : transfert de propriété vers le client ; perso d'un joueur : bloqué |
| Touches F9/F10 pressées dans une autre application | ignorées si Kenshi n'a pas le focus |
| Mise à jour de Kenshi / RE_Kenshi | KenshiLib s'adapte par version ; l'horloge est localisée dynamiquement ; vérifier les logs après une mise à jour |
| Antivirus qui bloque la DLL injectée | ajouter une exception pour le dossier Kenshi |

## 9. Tester sans second PC
Il faut **une copie de Kenshi par joueur** (le partage familial Steam ne permet pas de jouer au même
jeu en même temps, et deux instances sur le même PC sont refusées par Steam). Pour tester seul, le
bot `tests\out\bot.exe` joue le second joueur : il copie votre perso à côté de vous (sans bras gauche,
pour tester les membres), recopie votre inventaire, écrit dans le chat et répond à vos messages,
donne puis reprend un objet, déclare la guerre puis fait la paix. Avec `--host-replay` il joue l'hôte
et rejoue un monde enregistré (PNJ, inventaires, météo, heure).
- `tests\build_and_run.bat` : tests réseau hors jeu (connexion, relais, dégâts, timeouts, client
  lent, trames corrompues, mods/versions, validation des données).
- `tests\build_bot.bat` puis `tests\out\bot.exe 127.0.0.1 47000 600 4` pendant que vous hébergez :
  un faux 2e joueur qui renvoie votre escouade décalée de 4 m (tester fantômes, combat, dégâts).
- `bot.exe 127.0.0.1 47000 170 4 flux.rec` enregistre le flux de PNJ de l'hôte ;
  `bot.exe --host-replay flux.rec 47000` le rejoue en hébergeant, Kenshi en `mode=join`
  (tester le côté client).

# KenshiMP — Impression « native » : point par point

État au 01/10/2026. ✅ fait et vu en jeu · 🧪 fait, pas encore vu en jeu · 💡 solution proposée (pas encore faite)

## 1. Ce que l'autre joueur voit de vous

| Point | État | Solution |
|---|---|---|
| Fantôme qui se fige puis se téléporte | ✅ | Cause : le mode « hors écran » de Kenshi (mises à jour grossières des persos hors caméra). Les fantômes des joueurs en sont exemptés. Téléportation seulement si le fantôme est vraiment perdu. Parcours extrême rejoué : 1980 → 5 téléportations, figé 62 % → 0 %. |
| Retard au départ d'un sprint (25-50 unités) | ✅ | Coup de pouce le long du chemin réel du propriétaire (jamais à travers un mur) quand le moteur accélère trop lentement. Mesuré : écart moyen 10,5 → 6 unités, 4 téléportations sur tout le parcours extrême. |
| Animations des tâches | 🧪 | Liste élargie (médecin, réparation de robot, machines à remplir/vider/débloquer, mines). Une tâche n'est copiée que si le propriétaire est immobile (sinon le fantôme restait planté). Le fantôme est libéré quand la tâche ou le combat se termine. |
| Nom du joueur sur ses persos | ✅ | Le fantôme s'appelle « Nom [Pseudo] » et son étiquette de nom est affichée. |
| Prothèses robotiques | ✅ | Vu en jeu : le bras droit du fantôme apparaît comme prothèse (barre bleue dans la santé), appliquée une seule fois. |
| États : faim, encombrement | ✅ | La faim du propriétaire est envoyée avec les statistiques et appliquée au fantôme (vu en jeu dans le journal). L'encombrement découle de l'inventaire déjà recopié. Kenshi n'a pas d'ivresse. |
| Contenu des sacs à dos | ✅ | Le sac porté est un conteneur de plus (`CONTAINER_BACKPACK`) : son contenu est recopié sur le fantôme et un objet pris dedans est retiré chez le propriétaire (vu en jeu, sac « Refitted Oil Drum Backpack »). Remis en place quand le fantôme est rhabillé. |
| PNJ de l'hôte hors caméra | 🧪 | PNJ à moins de 150 unités de vos persos exemptés du mode hors écran (combats et poursuites fluides), les autres restent économiques. |

## 2. Interface intégrée au jeu

| Point | État | Solution |
|---|---|---|
| Bouton MULTIJOUEUR sous le menu pause | ✅ | Vu en jeu, même style que les boutons du jeu. |
| Autres joueurs sur la carte | ✅ | Escouades ajoutées à la carte du jeu, retirées pendant une guerre et remises à la paix (vérifié en jeu : affichée → cachée à la guerre → réaffichée à la paix). Option `show_players_on_map`. |
| Diplomatie | ✅ | Dans la fenêtre Multijoueur : une ligne par joueur, état actuel, boutons GUERRE / PAIX / ALLIÉ / ÉCHANGER (vu en jeu). 💡 Plus tard : les mêmes boutons dans l'écran Factions du jeu (accroche de `FactionsScreen` : modifier un écran du jeu, plus fragile ; gain surtout esthétique). |
| Liste des joueurs | ✅ | Fenêtre Multijoueur (F4, menu pause, écran titre) : joueurs, ping, diplomatie. |
| Arrivées / départs / guerres dans le chat | ✅ | Lignes grises dans l'historique (vu en jeu : « Bot a rejoint la partie », « Bot vous a déclaré la guerre ! »). |
| Échap ferme la fenêtre | ✅ | Échap ferme la fenêtre Multijoueur sans ouvrir le menu pause du jeu derrière (vu en jeu). |
| Langue | ✅ | Celle du jeu (vu en jeu en français). |

## 3. Commerce et objets

| Point | État | Solution |
|---|---|---|
| Objet posé au sol par un autre joueur | ✅ | Apparaît chez vous ; le ramasser retire l'original chez son propriétaire (vu en jeu, chaîne complète). |
| Argent des marchands | 🧪 | Montant du marchand envoyé par l'hôte, appliqué à l'ouverture de l'échange, gains/paiements renvoyés au vrai marchand. |
| Échange direct entre joueurs | ✅ | Bouton ÉCHANGER sur la ligne du joueur (fenêtre Multijoueur, F4) quand un de vos persos est à côté d'un des siens ; l'autre reçoit un message et clique ACCEPTER. La fenêtre d'échange du jeu s'ouvre des deux côtés (vu en jeu) ; chaque objet déplacé passe par les transferts validés. Fermer la fenêtre (Échap) termine l'échange des deux côtés. |

## 4. Connexion

| Point | État | Solution |
|---|---|---|
| Message pendant la connexion | 🧪 | « Connexion à l'hôte... » au chargement si la connexion n'est pas encore établie, puis « Connecté » (message + chat). |
| Reconnexion après coupure | ✅ | Même emplacement, fantôme recréé (vu en jeu). Corrigé : la relation (guerre/paix/alliance) est retenue par nom et restaurée au retour (avant : remise à la valeur par défaut). |
| Partie réelle sur Internet | 💡 | Séance à prévoir entre deux PC (VPN type Radmin/Tailscale ou redirection du port TCP 47000). Seule vraie inconnue réseau restante. |

## 5. Monde partagé

| Point | État | Solution |
|---|---|---|
| Villes du monde (destructions, prises) | 💡 | L'hôte envoie l'état des villes chargées autour des joueurs (bâtiments détruits, propriétaire de la ville) ; le client l'applique et coupe ses propres changements de villes, comme pour les PNJ. Gros chantier. |
| Primes et crimes entre joueurs | 💡 | Quand un joueur est volé ou agressé par un autre, envoyer l'événement au propriétaire de la victime pour qu'il crée la prime chez lui (sa faction et sa ville). |
| Migration d'hôte | 💡 | Si l'hôte part, le client au plus petit numéro devient hôte : il garde sa partie, les autres se reconnectent à lui (adresse connue de tous). Les PNJ deviennent les siens. Simple côté réseau ; le monde de référence change (celui du nouvel hôte). |

## Ordre proposé pour la suite
1. Restent à voir en jeu : argent des marchands, message de connexion (côté client), animations de tâches, PNJ proches.
2. Diplomatie dans l'écran Factions du jeu (esthétique).
3. Partie Internet réelle ; villes du monde ; primes ; migration d'hôte.

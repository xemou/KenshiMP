# KenshiMP — Registre des risques multijoueur

Légende : ✅ traité (et comment c'est vérifié) · 🧪 codé et testé hors jeu, **à valider en jeu** · 🟡 partiel / atténué · ❌ non traité (parade prévue)
« testé hors jeu » = `tests\net_test.cpp` · « vu en jeu » = observé avec le bot.

## 1. Réseau et connexion
| # | Problème | État | Parade |
|---|---|---|---|
| 1 | Hôte derrière une box sans redirection de port / CGNAT (4G, fibre partagée) | 🟡 | Doc : redirection du port TCP ou VPN de jeu (Radmin, ZeroTier, Tailscale). ❌ Serveur relais public : à faire si besoin. |
| 2 | Pare-feu Windows bloque Kenshi | 🟡 | Doc (autoriser à la 1re demande) ; message d'erreur explicite côté client. |
| 3 | Mauvaise IP / hôte pas lancé | ✅ | Message clair ; reconnexion automatique toutes les 10 s. |
| 4 | Latence élevée, gigue, pics | ✅ | Horodatage + interpolation + extrapolation + lissage (simulation : ≤ 8,1 u/s pour 6 réels avec pics +400 ms, 0 image figée). |
| 5 | Blocage « tête de file » de TCP en cas de perte de paquets | 🟡 | Rendu tamponné 60-300 ms absorbe les courts blocages. ❌ Passer les positions en UDP (gros chantier) si Internet réel le demande. |
| 6 | Connexion lente / débit montant faible de l'hôte | ✅ | États périmés sautés au-delà de 512 Ko en attente ; déconnexion propre à 8 Mo (testé hors jeu). LOD PNJ, envoi des PNJ immobiles supprimé. |
| 7 | Coupure / plantage d'un joueur | ✅ | Détection en 15 s (testé hors jeu), fantômes nettoyés, reconnexion auto. |
| 8 | Joueur qui revient change de numéro → factions/relations mélangées | ✅ | Même nom = même emplacement pendant la session (testé hors jeu). |
| 9 | Inconnu qui rejoint la partie | ✅ | Option `password=` (refus explicite, testé hors jeu). |
| 10 | Versions de mod / protocole différentes | ✅ | Refus avec message (testé hors jeu). |
| 11 | Mods ou version de Kenshi différents | ✅ | Liste des mods + version du jeu comparées ; refus ou avertissement (`strict_mods`, testé hors jeu et vu en jeu). |
| 12 | Plus de 8 joueurs | ✅ | Refus « server full ». |
| 13 | L'hôte quitte | 🟡 | Clients prévenus, reviennent à leur monde local. ❌ Migration d'hôte : non prévue. |
| 14 | Veille / hibernation d'un PC (horloge qui saute) | ✅ | Timeout puis reconnexion ; synchro d'horloge par fenêtre glissante. |
| 15 | Deux jeux sur le même port / même PC | ✅ | Message « port already used ». |

## 2. Triche, abus, données corrompues
| # | Problème | État | Parade |
|---|---|---|---|
| 16 | Paquets corrompus / malveillants | ✅ | Décodeurs bornés, fuzzing 20 000 paquets sans crash (testé hors jeu). |
| 17 | NaN / positions hors carte / valeurs absurdes | ✅ | Validation de toutes les positions, rotations, stats, dégâts (testé hors jeu). |
| 18 | Usurpation des entités d'un autre joueur | ✅ | Chaque ID porte son propriétaire ; l'hôte fixe l'expéditeur. |
| 19 | Dégâts gonflés | ✅ | Plafond par coup (500 par type). |
| 20 | Dégâts « à distance » en mêlée (client modifié) | ✅ | Coup refusé si l'attaquant est à plus de 30 unités de la victime. |
| 21 | Démolir la ville d'un ami | ✅ | Dégâts / démontage de bâtiments seulement entre joueurs hostiles (relation < 0). |
| 22 | Voler un PNJ de l'hôte par un faux recrutement | ✅ | L'hôte vérifie que le client est à moins de 60 unités du PNJ. |
| 23 | Inonder un joueur de personnages | ✅ | 400 fantômes max par joueur. |
| 24 | Recruter le personnage d'un autre joueur | ✅ | Bloqué. |
| 25 | Speedhack / téléportation de ses propres persos | 🧪 | L'hôte surveille les positions annoncées (horloge de l'émetteur, donc sans fausse alerte due au réseau) : vitesse > 80 u/s sur 0,5 s ou saut > 50 unités → avertissement dans le journal et à l'écran (une fois par minute et par joueur). Coureur rapide (45 u/s) non signalé (testé hors jeu). Pas de blocage : modèle coopératif. |
| 26 | Déclaration de guerre par erreur (touche dans une autre appli) | ✅ | F9/F10 ignorées si Kenshi n'a pas le focus. |

## 3. Cohérence du monde
| # | Problème | État | Parade |
|---|---|---|---|
| 27 | Chacun a sa sauvegarde, les mondes divergent | 🟡 | Monde de l'hôte partagé (PNJ, heure) ; conseil : clients sur une partie neuve/dédiée. ❌ Transfert de l'état des villes du monde. |
| 28 | Le client abîme sa sauvegarde solo | ✅ | Sauvegardes client redirigées vers `<nom>_MP`. |
| 29 | Fantômes enregistrés dans une sauvegarde | ✅ | Purgés à chaque chargement. |
| 30 | L'hôte recharge une autre sauvegarde en cours de session | ✅ | Détection, caches invalidés, purge, resynchronisation. Vu en jeu : chargement d'une sauvegarde avec un joueur connecté, pas de plantage, fantôme recréé. |
| 31 | Connexion pendant l'écran titre / un chargement | ✅ | Messages du monde ignorés puis demande de resynchronisation. |
| 32 | Heure différente | ✅ | Horloge de l'hôte imposée (vu en jeu). |
| 33 | Météo différente | ✅ | L'hôte diffuse la météo de chaque région active ; les clients l'appliquent et ne tirent plus la leur (`weather_sync`). Vu en jeu : 2 régions envoyées par l'hôte, « weather synced » côté client. |
| 34 | Pause d'un seul joueur | ✅ | Seul l'hôte met en pause, pour tous. |
| 35 | Vitesse accélérée | ✅ | Bloquée à x1. |
| 36 | PNJ différents chez chacun | ✅ | PNJ de l'hôte répliqués, population locale du client désactivée (vu en jeu côté hôte). |
| 37 | Client loin de l'hôte (zone non simulée) | ✅ | L'hôte garde chargées les zones des clients. 🧪 Option `load_sharing=1` : au-delà de 3000 unités, le client simule lui-même ses environs et l'hôte arrête de simuler cette région (moins de CPU pour l'hôte) ; en deçà de 2000, retour au monde partagé de l'hôte. |
| 38 | Fantôme créé dans une zone non chargée (création / destruction en boucle) | ✅ | Création différée tant qu'il est à plus de 2 500 unités de nos persos. |
| 39 | Objets : inventaires, coffres, sol, butin | ✅ | Inventaires, coffres, PNJ et pillage : vus en jeu. Objets au sol : l'objet posé par un autre joueur apparaît chez vous, le ramasser le retire chez son propriétaire (vu en jeu). Sacs à dos portés : contenu recopié, objets pris dedans retirés chez le propriétaire (vu en jeu). 🟡 Contenu d'un sac posé au sol non partagé. |
| 40 | Commerce entre joueurs / avec un marchand fantôme | 🧪 | Entre joueurs : bouton ÉCHANGER / ACCEPTER, fenêtre d'échange du jeu des deux côtés, transferts validés (vu en jeu). Inventaires des PNJ de l'hôte recopiés (vu en jeu). Argent du marchand (codé, testé hors jeu) : l'hôte envoie l'argent de l'escouade de chaque PNJ ; à l'ouverture de la fenêtre d'échange, la copie du marchand reçoit ce montant ; ce qu'il gagne ou paie pendant l'échange est appliqué au vrai marchand chez l'hôte. |
| 39b | Transfert refusé (trop loin, quelqu'un a été plus rapide) | 🧪 | Le propriétaire renvoie une annulation : l'objet pris est retiré, l'objet donné est rendu, la copie du conteneur revient à son dernier état connu (objets au sol compris, l'objet refusé est reposé). Message au joueur. |
| 41 | Porter un corps, prisonniers, cages, esclaves entre joueurs | ✅ | Porter/mettre en cage un perso d'un autre joueur : bloqué. Le piller est autorisé (objets validés par son propriétaire). Sur un PNJ de l'hôte : il devient le vôtre. |
| 42 | Tourelles d'un joueur qui tirent sur un autre | 🟡 | Tourelles servies par un fantôme : ses tirs passent par le même filtre que les persos fantômes (fenêtre de 4 s, cf. #54). |
| 43 | Production, fermes, recherche | 🟡 | Simulées chez le propriétaire uniquement (les autres voient les bâtiments). |
| 44 | Constructions superposées de deux joueurs | 🧪 | Plan refusé et retiré s'il est à moins de 3 unités d'un bâtiment d'un autre joueur (message). |
| 45 | Intérieurs / étages des bâtiments | 🧪 | Étage transmis avec la position ; téléportation sur le bon étage. |
| 46 | Membres coupés, membres robotiques, états (ivresse, faim) | ✅ | Vu en jeu : bras gauche coupé chez le propriétaire → coupé sur son fantôme. Prothèses : vues en jeu. Faim : envoyée avec les stats et appliquée au fantôme (vu dans le journal). Kenshi n'a pas d'ivresse. |
| 47 | Captifs d'un client supprimés par la désactivation des PNJ locaux | 🧪 | Nettoyage : ignore les persos portés, esclaves, et PNJ pris par le client. |
| 48 | Relations avec les factions du monde | ✅ | Table synchronisée vers la faction miroir. |
| 49 | Nom d'un emplacement joueur réutilisé | ✅ | Faction miroir renommée. |
| 50 | Dialogues avec un PNJ fantôme | 🟡 | Joués localement ; recrutement = transfert de propriété. |
| 51 | Primes / crimes | 🟡 | Locaux au joueur ; ses relations sont reportées chez l'hôte. |
| 52 | Guerre « tout le monde contre tout le monde » uniquement | ✅ | F9/F10 visent le joueur dont un perso est sélectionné (liste de sélection du moteur) ; chat `/war`, `/peace`, `/ally`. Vu en jeu : « You declared war on Bot ». |

## 4. Combat et rendu
| # | Problème | État | Parade |
|---|---|---|---|
| 53 | Double dégât (le coup compte chez les deux) | ✅ | L'attaquant calcule, le propriétaire de la victime applique ; coups des fantômes neutralisés. Revérifié en jeu le 30/09 (journal coup par coup : coup du fantôme sur notre perso = 0 dégât, notre coup transmis à son propriétaire). |
| 54 | Projectiles des fantômes | 🧪 | Chaque tir d'un fantôme est compté sur sa cible pendant son temps de vol (distance / vitesse du tir, 0,8 à 4 s) ; une flèche qui touche consomme un tir, celle d'un vrai archer arrivée en même temps n'est plus attribuée au fantôme. Un coup n'est jugé qu'une fois. |
| 55 | Fantôme qui se fige / se téléporte | ✅ | Cause principale trouvée et corrigée : hors du champ de la caméra, Kenshi simule les personnages en mode « hors écran » (mises à jour grossières) ; les fantômes des joueurs en sont exemptés. Plus : téléportation seulement si le fantôme est vraiment perdu (bloqué ou à plus de 150 unités), délai de stabilisation après une téléportation, avance proportionnelle à la vitesse, vitesse calée sur celle du propriétaire, horloge précise, tâche/combat relâchés. Mesuré en jeu sur un parcours enregistré rejoué à l'identique : parcours extrême (sprints à 70 u/s en zigzag) 1980 → 5 téléportations en 6,5 min, fantôme figé 62 % → 0 % ; parcours normal figé 32 % → 0,2 %. |
| 56 | Retard aux demi-tours brusques | ✅ | Couche de rendu vérifiée en jeu : le modèle est dessiné sur la trajectoire lissée (position appliquée conservée par le moteur). |
| 57 | Deux corps très proches se gênent | 🧪 | Option expérimentale `ghost_no_collide=1` (désactivée par défaut) : les fantômes ne sont plus poussés par vos persos. À essayer en jeu. |
| 58 | Écart entre le modèle affiché et le corps (clic, sélection) | 🟡 | Couche de rendu limitée à 6 unités et seulement en déplacement. |
| 59 | FPS très différents entre joueurs | ✅ | Tout est basé sur le temps réel, pas sur les images. |

## 5. Performance et exploitation
| # | Problème | État | Parade |
|---|---|---|---|
| 60 | Trop de PNJ à envoyer | ✅ | Rayon d'intérêt 600, LOD 10/5/2 Hz, rien pour les immobiles. |
| 61 | Fuites mémoire sur de longues sessions | ✅ | Tables nettoyées. Mesuré en jeu : 10 min de session à deux, +170 Mo (flux du monde de Kenshi), handles stables, aucune déconnexion. |
| 62 | Journal inondé | ✅ | Messages agrégés et limités ; diagnostics détaillés seulement avec `debug_keys=1`. |
| 63 | Plein écran exclusif (Alt+Tab met le jeu en pause) | ✅ | Doc : mode sans bordure. |
| 64 | Mise à jour de Kenshi / RE_Kenshi | 🟡 | KenshiLib s'adapte ; horloge localisée dynamiquement ; vérifier les imports (script) et les logs après mise à jour. |
| 65 | Autres mods qui modifient les mêmes fonctions | 🟡 | KenshiLib gère les accroches multiples ; incompatibilités possibles à tester au cas par cas. |
| 66 | Antivirus qui bloque la DLL | 🟡 | Doc : exception pour le dossier Kenshi. |
| 67 | Chat | ✅ | Entrée ouvre la boîte (décidé à l'appui : l'Entrée qui valide une fenêtre du jeu, ex. sauvegarde, ne l'ouvre pas), Entrée envoie, Échap annule ; touches de Kenshi inactives pendant la saisie. Vu en jeu. |
| 69 | Connexion compliquée (IP, port, fichier de config) | ✅ | Fenêtre Multijoueur (bouton de l'écran titre / F4) : Host/Join/Leave, IP locales affichées, raisons de refus lisibles (mods manquants / en trop / ordre), config enregistrée (vu en jeu : hôte, client, refus, saisie). |
| 70 | Conflit de touches avec d'autres mods (CheatMenu utilise F6) | ✅ | Touche configurable `lobby_key` (F4 par défaut). |
| 71 | Le mod « fait mod » (textes anglais, fenêtres à part) | ✅ | Textes dans la langue du jeu (vu en jeu : fenêtre en français), historique du chat coloré (vu en jeu), nom/faction pré-remplis (vu en jeu), bouton MULTIJOUEUR sous le menu titre et sous le menu pause (🧪 menu pause). |
| 72 | On ne voit pas où sont les autres joueurs | ✅ | Leurs escouades sont ajoutées à la carte du monde du jeu (mêmes repères que les escouades), sauf en guerre (brouillard). Option `show_players_on_map`. Vérifié en jeu : affichée, cachée à la déclaration de guerre, réaffichée à la paix. |
| 68 | Distribution de la même version à tous | 🟡 | Vérifiée à la connexion ; distribuer le dossier `dist\KenshiMP` zippé. |

## Priorités proposées pour la suite
1. Valider en jeu les ajouts 🧪 : objets au sol, argent des marchands, alerte de vitesse (le bot sait
   poser un objet au sol à 45 s et répondre au ramassage).
2. Partie réelle à deux PC sur Internet (VPN ou redirection de port).
3. Contenu des sacs à dos (#39). UDP ou relais selon les retours sur Internet (#1, #5).
4. Migration d'hôte (#13), transfert de l'état des villes du monde (#27).

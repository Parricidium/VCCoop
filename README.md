<p align="center">
  <img src="docs/img/logo.png" width="360" alt="Grand Theft Auto: Vice City COOP">
</p>

<p align="center">
  <a href="https://store.rockstargames.com/fr/game/buy-grand-theft-auto-the-trilogy"><img src="https://img.shields.io/badge/Buy%20GTA%20Vice%20City%20legitimately-Rockstar%20Store-FCAF17?style=for-the-badge&logo=rockstargames&logoColor=black" alt="Buy GTA Vice City on the Rockstar Store"></a>
</p>
<p align="center">
  <b>This mod needs a legitimately owned copy of Grand Theft Auto: Vice City.</b><br>
  <a href="https://store.rockstargames.com/fr/game/buy-grand-theft-auto-the-trilogy">Buy it on the Rockstar Store</a> (also on Steam). No game data is included here.
</p>

<p align="center">
  <a href="https://github.com/Parricidium/VCCoop/releases/latest"><img src="https://img.shields.io/github/v/release/Parricidium/VCCoop?label=Download&style=for-the-badge" alt="Download the latest release"></a>
  <a href="https://ko-fi.com/parricidium"><img src="https://img.shields.io/badge/Ko--fi-Support%20me-FF5E5B?style=for-the-badge&logo=ko-fi&logoColor=white" alt="Support me on Ko-fi"></a>
</p>

# VCCoop — the Vice City story in co-op, 2 to 4 players

A co-op mod for **Grand Theft Auto: Vice City** (PC, version 1.0). One player hosts and plays the
story; up to three friends join them in the same city and the same missions: same characters, cars,
cutscenes, texts, markers and timers, and their shots, punches and explosions count. Everything
outside the missions is shared too: traffic and pedestrians around the host, police, weather, money
and wanted level.

It also modernises the 2002 game, all optional: borderless widescreen at desktop resolution, a free
mouse camera in vehicles with GTA V-style aiming, longer draw distance, a custom menu look, and
**shared mods**: car and building models dropped in a folder are loaded by the game and sent to the
other players by themselves.

*[Version française plus bas.](#version-française)*

> You need your own copy of GTA Vice City (Steam, downgraded to 1.0). No game data is provided here.

---

## Contents

- [Download and install](#download-and-install)
- [Playing](#playing)
- [Features](#features)
- [Shared mods](#shared-mods)
- [Custom interface](#custom-interface)
- [Network](#network)
- [Known issues](#known-issues)
- [Building](#building)
- [Credits and license](#credits-and-license)
- [Version française](#version-française)

---

## Download and install

1. Every player needs GTA Vice City with `gta-vc.exe` in **version 1.0** (3 088 896 bytes). The
   Steam version must be downgraded with a community *downgrader*.
2. Download `VCCoop-<version>.zip` from the [releases](https://github.com/Parricidium/VCCoop/releases).
3. Unzip everything into the game's folder, next to `gta-vc.exe` (tip: make a copy of the game folder
   for co-op). The mod is a `dinput8.dll` proxy: nothing else in the game is modified.
4. Set your nickname: COOP menu > **Nickname** (or in `vccoop.ini`). Nickname, host address, port and outfit
   are kept in `vccoop-joueur.ini` (not in the package): updating the mod no longer resets them.

**Launcher**: start `VCCoop.exe` (in the game folder). It checks that `gta-vc.exe` is the 1.0 (the Steam,
GOG and 1.1 exes are refused; "Change exe" picks another one), looks for a new version on GitHub **every time
it starts** and installs it by itself (your `vccoop.ini` settings and `vccoop-joueur.ini` are kept), then
**Host** opens a **lobby** (game port, 7790 by default) and **Join** enters it: player list (ready, ping), the
host's shared mods downloaded beforehand with everyone's %, *New game* or *Load* one of the host's saves, and
**Start** launches everyone's game straight into the session (**Join in game** if the host is already playing;
**Play** = the game's COOP menu, no lobby). The **Outfit** tab picks your in-game outfit (the F7 list) with a turning
3D preview and portraits, also shown next to each name in the lobby; the models are read from your own game files.
Its **Video / Rendering / Effects / Co-op** tabs change the `vccoop.ini` settings (the same as the
game's COOP menu; Rendering and Effects only with Direct3D 9, host settings tagged HOST).
It stays on screen until the game window shows up. English unless Windows is in French
(`Langue=fr|en` under `[Lanceur]` in `vccoop-launcher.ini` to force it). Its background is `VCCoop/interface/launcher.png`
(1000×620, transparency supported: the window takes the image's shape).

**Updating**: the launcher does it; by hand, unzip the new version over the old one. Everybody must run
the same version (the network protocol is checked when joining).

Saves and settings of the co-op copy stay in the game folder (`SauvegardesLocales=1`), apart from your
solo game.

## Playing

Start `gta-vc.exe` normally. Main menu > **COOP**:

| | |
|---|---|
| **Host a game** | Opens the lobby with the list of connected players. **New game** or **Load a game**: the guests follow you by themselves (your save is sent to them). |
| **Join** | **Address** (the host's IP: Enter, type or paste with **Ctrl+V**, Enter), then **Connect**. You wait in the lobby and enter the game when the host does. |
| **Nickname** | Your name, set before hosting or joining. |
| **Options** | **Coop options**: friendly fire, shared money, nicknames, keep weapons after death. **Video options**: draw distance (100 to 400 %), anti-aliasing (2x to 8x, taken at the next launch), anisotropic filtering, sun shadows and their quality, modern water, dynamic lights, light shadows, moon shadows, modern renderer (next launch). Video options are set here or in game with **Esc > COOP**; the host also has *Coop options* in the lobby and in game. |

`VCCoop - Heberger.cmd` / `VCCoop - Rejoindre.cmd` go straight to hosting / joining. Without COOP the
game stays single player. **Esc > COOP** in game shows the same page.

The host plays the missions as in single player. Guests are placed next to the host at the start and
the end of each story mission; "go there" and "get in that car" objectives can be completed by any
player, and guests see the destination markers.

### Keys

| Key | |
|---|---|
| **F** / **G** | Near another player's vehicle: take the first free seat (wheel or passenger) instead of jacking them, with the full animation. As a passenger, F or G to get out. |
| **F7** | Choose your outfit: Tommy, story characters, any pedestrian. Left / Right to browse, Enter to keep, Backspace to cancel. Kept for the next games; missions that dress Tommy override it for their duration. |
| **Tab** (held) | Player list: health, armour, ping, distance. |
| **B** | Put a meeting point where you look (seen by everybody, in your colour); B again to remove it. |
| **Mouse** (vehicle) | Orbits the camera around the vehicle (back behind after 2.5 s). **Right click** with a pistol or SMG: aim at the centre of the screen, left click fires — driver or passenger. The mouse no longer steers. `CameraLibre=0` for the original camera. |
| **F6** | First person view from Tommy's head, on foot and in any vehicle (hands on the wheel, dashboard, bike fairing); the mouse turns the head in a vehicle. F6 again for the normal camera. `ToucheVue` changes the key, `VuePremierePersonne=0` disables it (also in Video options). |
| **Passenger** | Look left / right and shoot like the driver (pistols, SMGs). |

Each player has a colour (blue = host, orange, green, purple): a blip on the radar and the map, and
their nickname above their head (`AfficherPseudos=0` to hide).

## Features

- **The story in co-op**: missions run on the host and are mirrored to the guests — mission
  characters and vehicles, cutscenes, fades, camera, texts and help messages, radar blips and
  markers, pickups, objects (briefcases, bombs...), on-screen timers and counters, the area of the city
  or interior being shown. Story progress is sent to the guests as missions are passed, and a guest
  who joins later receives the host's save and the current state.
- **Combat**: player-vs-player damage (punches, bullets, explosions) if friendly fire is on;
  bullets and punches on the host's characters count on the host, even on "only damaged by player"
  mission targets; a guest's police hurts the other players for real; grenades, molotovs and rockets
  are replayed on every machine; other players' shots are visible, including drive-by; a dead player
  stays down and no longer blocks the way.
- **Vehicles**: every vehicle driven by a player is shared — position and physics interpolated,
  steering, wheel spin, radio station of the driver, colours, visible damage (doors, panels, lights,
  tyres), wrecks, and health lost to another player's bullets. Players get in and out with the game's
  own animations, door included, crawling out of an overturned car included.
- **Shared world, merged**: the host populates the area around them (110 m, their generation range);
  beyond it each guest populates their own part of the city and shares it with nearby players, so
  everyone sees the same pedestrians, traffic and parked vehicles (PCJ-600 and other fixed spawns
  included). Duplicates are removed as they spawn, off-screen, never in front of a player.
- **Police**: shared wanted level. Near the host, only the host's police exists (`PoliceHote=1`): its cops
  that are not chasing the host go after the nearest wanted guest and shoot at them for real; guests see
  them. A guest far from the host has their own police. Crimes by a guest on the host's pedestrians give
  stars.
- **Death**: a dead or busted guest reappears at the nearest hospital / police station, keeping
  weapons and money (`GarderArmes`), or next to the host (`ReapparitionHote=1`).
- **Side missions**: a guest can run taxi, ambulance, firefighter, vigilante and pizza missions on
  their side, in the right vehicle; the host's own side missions are not mirrored either. Same for
  the checkpoint challenges (PCJ Playground, Trial by Dirt, Test Track, Cone Crazy, Chopper
  Checkpoint, RC races, Checkpoint Charlie): everyone runs their own. The checkpoint rings of story
  missions (boat race, Kickstart...) are shown to the guests too.
- **Ragdolls**: a character who is killed or hit by a car collapses and tumbles like a real body (hit
  but alive, they get back up afterwards); the pose is computed by the character's owner and is the
  same for everyone. `CorpsMous=0` (or Coop options) brings back the original animations.
- **What the others do, seen and heard**: smashed props (boxes, lamp posts, windows), crash sounds,
  engine, tyres and sirens of their vehicles, animated getting in and out (players and characters),
  passers-by fleeing an armed guest, police cars chasing a wanted guest (2 stars and up) near the host.
- **Properties per player**: a guest can buy safehouses and businesses with their own money
  (the purchase mission runs on their side); the host's purchases stay the host's, the icon remains
  on sale for the others. A guest's money, weapons and properties are kept in
  `joueur-<nickname>.ini` next to `vccoop.ini` and restored after every load of the host's save.
- **Reconnection**: a guest who loses the connection comes back by themselves, without reloading, and
  receives the full state again; a reliable stream that gets out of step is detected within seconds and
  resynchronised (mission rewards received during a loading screen are kept). A player who quits
  disappears at once on the other machines, with their vehicles.
- **Widescreen**: borderless fullscreen at desktop resolution by default, real aspect ratio (16:9,
  21:9, 32:9) with a wider field of view, HUD and menus in proportion (`Fenetre`, `GrandEcran`).
- **Video options** (all local, `0` = original rendering): draw distance (`DistanceAffichage`, 100 to
  400 %), anti-aliasing (`Anticrenelage`, MSAA 2x/4x/8x), anisotropic filtering 16x + trilinear
  (`FiltrageAnisotrope`), **modern renderer** (`Rendu=9`, default) and **sun shadows** (`OmbresSoleil`,
  quality `OmbresResolution` 2048 / 4096 / 8192).
- **Population**: pedestrians and traffic spawn further away and stay while you are in the area, whether you
  look at them or not (`ZonePopulation`, 100 to 200 %, the host's value counts); more of them at once
  (`DensitePopulation`, 50 to 300 %). The game's pools are doubled (280 characters, 220 vehicles).
- **Modern renderer**: VCCoop contains its own Direct3D 8 → Direct3D 9 bridge (no third-party wrapper, nothing
  to install). The game draws exactly as before, and VCCoop then works in Direct3D 9 with HLSL shaders
  (compiled at launch by Windows' own `d3dcompiler_47.dll`). `Rendu=8` goes back to the original Direct3D 8.
- **Sun shadows** (modern renderer): 4 cascades (sharp up close, up to ~220 m) rendered from the game's
  own sun (`CTimeCycle`) in the same frame, a soft 25-tap filter, a single full-screen pass (no more
  double-drawn geometry flickering), strength from the game's time cycle (hour and weather) and its fog.
  Buildings just outside the view still cast their shadow (no more popping when the camera turns).
  With `Rendu=8`, the older shadow map on the Direct3D 8 device is used.
- **Modern water** (`EauModerne`, modern renderer): the game's water surfaces are redrawn with a VCCoop shader —
  turquoise in the shallows and deep blue-green offshore (from the real depth under each pixel), the sea floor
  seen through with refraction, the sky of the time cycle reflected with Fresnel, the sun's glint, per-pixel
  waves and foam along the shores. Seabirds and boats on the horizon are kept. With `RefletsEau` the piers,
  boats, palm trees and buildings are mirrored in the water (planar reflection, fading along the shores).
- **Dynamic lights** (`LumieresDynamiques`, modern renderer): every light the game registers (street lamps,
  neons, headlights, explosions, fire, muzzle flashes) now lights the scenery per pixel up to 150 m, not only
  the characters and vehicles within 22 m. The 4 most important lights near the camera (`OmbresLumieres`)
  cast shadows: street lamps down around them, headlights forward (pedestrians, cars, objects). Every car and
  bike with its lights on gets a real headlight beam (the game only painted a light patch on the ground).
- **Moon shadows** (`OmbresLune`): at night the game's moon (visible from 0:00 to 6:00, veiled by clouds,
  rain and fog) casts soft bluish shadows, weaker than the sun's.
- **Ambient occlusion** (`OcclusionAmbiante`, modern renderer): corners, the foot of walls, palm trees and
  characters, and the underside of cars get a soft contact shade (screen-space, edge-aware blur, fades out
  after 90 m).
- **Image effects** (Video options > Image effects): SMAA anti-aliasing (MIT-licensed SMAA 2.8), bloom (neons,
  sun and headlights glow softly), colour grading (`Etalonnage`: Original, Vice — "Miami 80s" colours that
  follow the time of day —, Film) and adaptive sharpening (`Nettete`).
- **Street lamps, neons and signs cast real light** at night (`LampadairesEclairent`), with shadows for the
  closest ones; their painted ground patch is removed. **Soft particles** (`ParticulesDouces`): smoke and
  explosions no longer cut sharply against the ground and walls.
- **Atmosphere** (Video options > Atmosphere): wet roads in the rain (neon and headlight reflections, puddles,
  ripples) and shiny interior floors, sun rays, palms and trees swaying in the wind, headlight beams in rain
  and fog, distance haze that follows the time of day, reflections on car bodies, indirect light.
- **30 fps by default** (`ImagesParSeconde`): above 30 the original game misbehaves (vehicle entry).
- **ASI loader built in**: `.asi` mods from the game folder, `scripts\` and `plugins\` are loaded
  (`ChargerASI=0` to skip). Do not install the Ultimate ASI Loader (same `dinput8.dll` name).
- **Logs**: `vccoop.log` for the current game and one file per game in `logs\` (last 50).

## Shared mods

`ModsPartages=1` (default). Drop your mods in `VCCoop\mods\`, in any sub-folders you like — the
folder names are only there for you:

```
VCCoop\mods\my ferrari\cheetah.dff
VCCoop\mods\my ferrari\cheetah.txd
VCCoop\mods\my ferrari\handling.cfg
```

Only the file names matter:

- `name.dff` / `name.txd` (also `.col`, `.ifp`) replace the game's model `name`: a car, a weapon, a
  character or a building (`cheetah`, `infernus`, `colt45`...).
- `handling.cfg` (or `handling.txt`, `*.handling`): handling lines in Vice City's `handling.cfg`
  format; each line replaces the game's line that starts with the same vehicle name (`CHEETAH ...`).
- `carcols.dat`: colour lines (`cheetah, 1,1, 2,2 ...`), same rule.

How it works: at every game start a `vccmods.img` is built at the game's root from these files and
loaded with priority over `gta3.img`. The game's own files are never modified: empty the folder and
everything is back to stock. The host sends its folder to the guests automatically (TCP, same port as
the game); a guest only downloads what they miss, the lobby shows the progress and the game cannot
start until everybody is up to date. Guests load exactly the host's files, so everybody has the same
models.

Limits: replacements only (no new vehicle IDs), 64 MB per file, model file names of 23 characters
at most (extension included). GTA V `handling.xml` files are not Vice City's format: use the
`handling.cfg` line the mod author provides.

## Custom interface

- Menu text: near-black with a pink outline, smaller and sharper than the original (`StyleMenus`,
  `CouleurTexteMenus`, `CouleurContourMenus`, `CouleurSelectionMenus`, `TailleTexteMenus`); the save
  pages use white text on the background with a pink selection bar.
- Images in `VCCoop\interface\`: `fond_menu.png` (menu background, 1920×1080 or 2560×1440),
  `logo.png` (512×512, transparent), `chargement1.jpg`, `chargement2.jpg`... (loading screens,
  picked at random), `fermeture.jpg` (image shown when quitting, instead of "Greetings from Vice City").
  Shown at their real size without stretching; see the folder's `LISEZMOI.txt`.

## Network

The host listens on **UDP 7790** (`Port` in `vccoop.ini`) and serves the shared mods on the same
port in **TCP**. Over the Internet the host forwards both on their router, or everybody uses a VPN
(Radmin VPN, ZeroTier, Hamachi). Guests set the host's address in the COOP menu.

## Known issues

Test build. A report with the `logs\` files of **both** players helps a lot.

1. **Vehicle damage caused by a non-owner**: bullets are sent to the owner, but body damage from
   collisions with a copy is not; fire is not synchronised.
2. **Getting in from far away**: the entry animation (door, seat, bike jump) plays for drivers and
   passengers, cars and bikes, when the player is next to the door; a player who presses the key from
   farther than 2.5 m is seated directly.
3. **Followers (Lance...) follow the host**: for escort missions, travel together.
4. **A guest who reloaded their game a different number of times than the host** can, rarely, get a
   wrong script variable from a full sync (crash risk being investigated).
5. **Only the host plays story missions**; guests cannot start one.
6. **No kick and no password** for the lobby (by choice for now).
7. **Mods**: `.col` and `.ifp` replacements are untested; map additions (new buildings) and added
   vehicles are not supported.
8. **Properties**: a property the host owns in a save that a guest loads is owned by the guest too
   (the game's save holds it); a guest's purchase on a property that is still on sale in the host's
   save can be bought again after a reload (it costs again).
9. **The game's own AI ignores guests**: enemies, gangs and the host's police only target the host;
   a pedestrian hit by a guest does not fight back.

## Building

Visual Studio 2022 Build Tools (x86, `/MT`), no other dependency: `build.cmd` compiles `src\*.cpp`
into `build\dinput8.dll`. `dist\make-release.ps1 -Version <v>` builds and packages the zip.
`run\` holds the two-instance test scripts (`run\make-testinstances.ps1` creates them under
`D:\Games\COOPTEST`); `re\` the Ghidra headless helpers used to read the game.

The game executable and any decompiled code are **not** in this repository.

## Credits and license

- Vice City 1.0 addresses: read with Ghidra, cross-checked with [plugin-sdk](https://github.com/DK22Pac/plugin-sdk)
  (DK22Pac).
- Grand Theft Auto and Vice City are trademarks of Rockstar Games / Take-Two Interactive. This is an
  unofficial, non-commercial fan project, not affiliated with them; you need your own copy of the game.
- License: not chosen yet (private repository).

If you enjoy it, you can [support me on Ko-fi](https://ko-fi.com/parricidium). ❤️

---

# Version française

Un mod coopératif pour **Grand Theft Auto: Vice City** (PC, version 1.0). Un joueur héberge et joue
l'histoire ; jusqu'à trois amis le rejoignent dans la même ville et les mêmes missions : mêmes
personnages, voitures, cinématiques, textes, marqueurs et minuteurs, et leurs tirs, coups et
explosions comptent. Hors mission aussi, tout est partagé : passants et circulation autour de l'hôte,
police, météo, argent et niveau de recherche.

Le mod modernise aussi le jeu de 2002, tout est optionnel : plein écran fenêtré à la résolution du
bureau, caméra libre à la souris en véhicule avec visée façon GTA V, distance d'affichage allongée,
menus personnalisés, et **mods partagés** : des modèles de voitures ou de bâtiments déposés dans un
dossier sont chargés par le jeu et envoyés tout seuls aux autres joueurs.

> Il faut posséder une copie légitime de GTA Vice City (rétrogradée en 1.0) : [l'acheter sur le Rockstar Store](https://store.rockstargames.com/fr/game/buy-grand-theft-auto-the-trilogy) (aussi sur Steam). Aucun fichier du jeu n'est fourni.

## Téléchargement et installation

1. Chaque joueur a besoin de GTA Vice City avec `gta-vc.exe` en **version 1.0** (3 088 896 octets).
   La version Steam se rétrograde avec un *downgrader* de la communauté.
2. Télécharger `VCCoop-<version>.zip` dans les [releases](https://github.com/Parricidium/VCCoop/releases).
3. Tout décompresser dans le dossier du jeu, à côté de `gta-vc.exe` (conseil : faire une copie du
   dossier du jeu pour la coop). Le mod est un `dinput8.dll` : rien d'autre n'est modifié.
4. Mettre son pseudo : menu COOP > **Pseudo** (ou dans `vccoop.ini`).

**Lanceur** : lancer `VCCoop.exe` (dans le dossier du jeu). Il vérifie que `gta-vc.exe` est bien le 1.0 (les
exe Steam, GOG et 1.1 sont refusés ; « Changer d'exe » en choisit un autre), cherche une nouvelle version sur
GitHub **à chaque démarrage** et l'installe tout seul (réglages de `vccoop.ini` et `vccoop-joueur.ini`
gardés). **Héberger** ouvre un **salon** (port du jeu, 7790 par défaut) et **Rejoindre** y entre : liste des
joueurs (prêt, ping), mods partagés de l'hôte téléchargés avant la partie avec le % de chacun, *Nouvelle partie* ou
*Charger* une sauvegarde de l'hôte, puis **Lancer** démarre le jeu de chacun, directement en partie (**Rejoindre en
jeu** si l'hôte joue déjà ; **Jouer** = menu COOP du jeu, sans salon). L'onglet **Tenue** choisit ta tenue en jeu (la liste de F7) avec un aperçu
3D qui tourne et des portraits, repris à côté de chaque pseudo dans le salon ; les modèles sont lus dans les fichiers de
ton jeu. Ses onglets **Vidéo / Rendu / Effets / Coop** modifient les réglages de `vccoop.ini` (les mêmes que le
menu COOP du jeu ; Rendu et Effets seulement en Direct3D 9, réglages d'hôte marqués HÔTE).
Il reste affiché jusqu'à l'apparition de la fenêtre du jeu. En français si Windows est en
français, sinon en anglais (`Langue=fr|en` dans `[Lanceur]` de `vccoop-launcher.ini` pour forcer). Son fond est
`VCCoop/interface/launcher.png` (1000×620, transparence prise en charge : la fenêtre prend la forme de l'image).

**Mise à jour** : le lanceur s'en charge ; à la main, décompresser la nouvelle version par-dessus. Tout le monde doit avoir la même
version (vérifiée à la connexion). Les sauvegardes et réglages de la copie coop restent dans le
dossier du jeu (`SauvegardesLocales=1`), à part du solo. Le pseudo, l'adresse de l'hôte, le port et la tenue
sont gardés dans `vccoop-joueur.ini` (absent du paquet) : une mise à jour ne les efface pas.

## Jouer

Lancer `gta-vc.exe` normalement. Menu principal > **COOP** :

| | |
|---|---|
| **Créer une partie** | Ouvre le salon avec la liste des joueurs connectés. **Nouvelle partie** ou **Charger une partie** : les invités suivent tout seuls (la sauvegarde leur est envoyée). |
| **Rejoindre** | **Adresse** (IP de l'hôte : Entrée, taper ou coller avec **Ctrl+V**, Entrée), puis **Se connecter**. On attend dans le salon et on entre en jeu avec l'hôte. |
| **Pseudo** | Votre nom, à régler avant de créer ou rejoindre. |
| **Options** | **Options coop** : tir ami, argent partagé, pseudos, garder ses armes après la mort. **Options vidéo** : distance d'affichage (100 à 400 %), anticrénelage (2x à 8x, pris au prochain lancement), filtrage anisotrope, ombres du soleil et leur qualité, eau moderne, lumières dynamiques, ombres des lumières, ombres de la lune, rendu moderne (au prochain lancement). Les options vidéo se règlent ici ou en jeu par **Échap > COOP** ; l'hôte a aussi *Options coop* dans le salon et en jeu. |

`VCCoop - Heberger.cmd` / `VCCoop - Rejoindre.cmd` vont droit à l'hébergement / la connexion. Sans
passer par COOP, le jeu reste en solo. **Échap > COOP** en jeu montre la même page.

L'hôte joue les missions comme en solo. Les invités sont posés à côté de lui au début et à la fin de
chaque mission d'histoire ; les objectifs « aller là », « monter dans cette voiture » peuvent être
remplis par n'importe quel joueur, et les invités voient les marqueurs de destination.

### Touches

| Touche | |
|---|---|
| **F** / **G** | Près du véhicule d'un autre joueur : prendre la première place libre (volant ou passager) au lieu de le lui voler, avec l'animation complète. Passager, F ou G pour descendre. |
| **F7** | Choisir sa tenue : Tommy, personnages de l'histoire, n'importe quel passant. Gauche / Droite pour parcourir, Entrée pour garder, Retour pour annuler. Gardée pour les parties suivantes ; les missions qui habillent Tommy la remplacent le temps de la mission. |
| **Tab** (maintenu) | Liste des joueurs : santé, gilet, ping, distance. |
| **B** | Poser un point de rendez-vous là où on regarde (vu de tous, dans sa couleur) ; B à nouveau pour le retirer. |
| **Souris** (véhicule) | Fait tourner la caméra autour du véhicule (retour derrière au bout de 2,5 s). **Clic droit** avec un pistolet ou une mitraillette : visée au centre de l'écran, clic gauche pour tirer, conducteur comme passager. La souris ne dirige plus la voiture. `CameraLibre=0` pour la caméra d'origine. |
| **F6** | Vue à la première personne depuis la tête de Tommy, à pied et dans tous les véhicules (mains sur le volant, tableau de bord, carénage de moto) ; la souris tourne la tête en véhicule. F6 à nouveau : caméra normale. `ToucheVue` change la touche, `VuePremierePersonne=0` la désactive (aussi dans Options vidéo). |
| **Passager** | Regarder à gauche / à droite et tirer comme le conducteur (pistolets, mitraillettes). |

Chaque joueur a sa couleur (bleu = hôte, orange, vert, violet) : un point sur le radar et la carte, et
son pseudo au-dessus de la tête (`AfficherPseudos=0` pour le cacher).

## Fonctionnalités

- **L'histoire en coop** : les missions tournent chez l'hôte et sont reproduites chez les invités :
  personnages et véhicules de mission, cinématiques, fondus, caméra, textes et messages d'aide,
  marqueurs radar, pickups, objets (mallettes, bombes...), minuteurs et compteurs à l'écran, zone
  affichée (ville ou intérieur). L'avancement de l'histoire part aux invités au fil des missions ; un
  invité qui arrive plus tard reçoit la sauvegarde de l'hôte et l'état courant.
- **Combat** : dégâts entre joueurs (coups, balles, explosions) si le tir ami est activé ;
  balles et coups sur les personnages de l'hôte comptent chez lui, même sur les cibles de mission
  « blessées seulement par le joueur » ; la police d'un invité blesse vraiment les autres joueurs ;
  grenades, cocktails et roquettes rejoués chez tout le monde ; tirs des autres joueurs visibles,
  drive-by compris ; un joueur mort reste au sol et ne bloque plus le passage.
- **Véhicules** : tout véhicule conduit par un joueur est partagé : position et physique
  interpolées, volant, rotation des roues, station de radio du conducteur, couleurs, dégâts visibles
  (portes, panneaux, phares, pneus), épaves, et santé perdue sous les balles d'un autre joueur. Montée
  et descente avec les animations du jeu, portière comprise, sortie en rampant d'une voiture
  retournée comprise.
- **Monde partagé, fusionné** : l'hôte peuple la zone autour de lui (110 m, sa portée de génération) ;
  au-delà, chaque invité peuple son coin de ville et le partage avec les joueurs proches : tout le monde
  voit les mêmes passants, la même circulation et les mêmes voitures garées (PCJ-600 et autres
  emplacements fixes compris). Les doublons sont retirés à leur apparition, hors écran, jamais sous les
  yeux d'un joueur.
- **Police** : niveau de recherche commun. Près de l'hôte, seule sa police existe (`PoliceHote=1`) : ses
  policiers qui ne poursuivent pas l'hôte prennent en chasse l'invité recherché le plus proche et lui
  tirent dessus pour de vrai ; les invités les voient. Un invité loin de l'hôte a sa propre police. Les
  crimes d'un invité sur les passants de l'hôte donnent des étoiles.
- **Mort** : un invité mort ou arrêté réapparaît à l'hôpital / au commissariat le plus proche, avec ses
  armes et son argent (`GarderArmes`), ou près de l'hôte (`ReapparitionHote=1`).
- **Missions secondaires** : un invité peut jouer taxi, ambulance, pompiers, justicier et pizzas de son
  côté, dans le bon véhicule ; celles de l'hôte ne sont pas reproduites non plus. Pareil pour les défis
  à checkpoints (PCJ Playground, Trial by Dirt, Test Track, Cone Crazy, Chopper Checkpoint, courses RC,
  Checkpoint Charlie) : chacun fait les siens. Les cercles à traverser des missions de l'histoire (course
  de bateaux, Kickstart...) s'affichent aussi chez les invités.
- **Corps mous** : un personnage tué ou percuté par une voiture s'effondre et roule comme un vrai corps
  (percuté mais vivant, il se relève ensuite) ; la pose est calculée par le propriétaire du personnage,
  identique chez tout le monde. `CorpsMous=0` (ou Options coop) pour les animations d'origine.
- **Ce que font les autres, vu et entendu** : décor cassé (cartons, lampadaires, vitres), bruits de choc,
  moteur, pneus et sirène de leurs véhicules, montées et descentes animées (joueurs et personnages),
  passants qui fuient un invité armé, voitures de police aux trousses d'un invité recherché (2 étoiles et
  plus) près de l'hôte.
- **Immeubles par joueur** : un invité peut acheter planques et commerces avec son propre argent (la
  mission d'achat tourne chez lui) ; les achats de l'hôte restent à l'hôte, l'icône reste à vendre
  pour les autres. Argent, armes et immeubles d'un invité sont gardés dans `joueur-<pseudo>.ini` à
  côté de `vccoop.ini` et remis après chaque chargement de la sauvegarde de l'hôte.
- **Reconnexion** : un invité qui perd la connexion revient tout seul, sans recharger, et reçoit à
  nouveau l'état complet ; un flux fiable désynchronisé est détecté en quelques secondes et
  resynchronisé (l'argent des missions reçu pendant un chargement est gardé). Un joueur qui quitte
  disparaît tout de suite chez les autres, avec ses véhicules.
- **Grand écran** : plein écran fenêtré à la résolution du bureau par défaut, vrai format (16:9,
  21:9, 32:9) avec un champ de vision élargi, HUD et menus en proportion (`Fenetre`, `GrandEcran`).
- **Options vidéo** (toutes locales, `0` = rendu d'origine) : distance d'affichage
  (`DistanceAffichage`, 100 à 400 %), anticrénelage (`Anticrenelage`, MSAA 2x/4x/8x), filtrage
  anisotrope 16x + trilinéaire (`FiltrageAnisotrope`), **rendu moderne** (`Rendu=9`, par défaut) et
  **ombres du soleil** (`OmbresSoleil`, qualité `OmbresResolution` 2048 / 4096 / 8192).
- **Population** : passants et voitures naissent plus loin et restent tant qu'on est dans la zone, qu'on les
  regarde ou non (`ZonePopulation`, 100 à 200 %, c'est la valeur de l'hôte qui compte) ; plus nombreux en même
  temps (`DensitePopulation`, 50 à 300 %). Les réserves du jeu sont doublées (280 personnages, 220 véhicules).
- **Rendu moderne** : VCCoop contient son propre pont Direct3D 8 → Direct3D 9 (aucun wrapper tiers, rien à
  installer). Le jeu dessine exactement comme avant, puis VCCoop travaille en Direct3D 9 avec des shaders HLSL
  (compilés au lancement par `d3dcompiler_47.dll`, fourni avec Windows). `Rendu=8` revient au Direct3D 8 d'origine.
- **Ombres du soleil** (rendu moderne) : 4 cascades (nettes de près, jusqu'à ~220 m) calculées depuis le
  soleil du jeu (`CTimeCycle`) dans la même image, filtre doux à 25 échantillons, une seule passe plein écran
  (plus de doubles dessins qui clignotent), intensité selon le cycle du jour du jeu (heure et météo) et son
  brouillard. Les bâtiments juste hors du champ projettent aussi leur ombre (plus d'apparition en tournant la
  caméra). Avec `Rendu=8`, l'ancienne carte d'ombre sur le Direct3D 8 du jeu reste utilisée.
- **Eau moderne** (`EauModerne`, rendu moderne) : les surfaces d'eau du jeu sont redessinées par un shader de
  VCCoop — turquoise en eau peu profonde et bleu-vert au large (selon la vraie profondeur sous chaque pixel), le
  fond visible par réfraction, le ciel du cycle du jour reflété (Fresnel), le reflet du soleil, des vagues par
  pixel et de l'écume sur les rives. Les oiseaux et bateaux à l'horizon restent. Avec `RefletsEau`, les quais,
  bateaux, palmiers et immeubles se reflètent dans l'eau (reflet plan, qui s'efface sur les rives).
- **Lumières dynamiques** (`LumieresDynamiques`, rendu moderne) : toutes les lumières du jeu (lampadaires,
  néons, phares, explosions, feux, tirs) éclairent maintenant le décor par pixel jusqu'à 150 m, et plus
  seulement les personnages et véhicules à moins de 22 m. Les 4 lumières les plus importantes près de la caméra
  (`OmbresLumieres`) projettent des ombres : les lampadaires tout autour vers le bas, les phares vers l'avant
  (passants, voitures, objets). Chaque voiture et moto phares allumés a un vrai faisceau (le jeu ne peignait
  qu'une tache lumineuse au sol).
- **Ombres de la lune** (`OmbresLune`) : la nuit, la lune du jeu (visible de 0 h à 6 h, voilée par les nuages,
  la pluie et le brouillard) projette des ombres douces et bleutées, plus faibles que celles du soleil.
- **Occlusion ambiante** (`OcclusionAmbiante`, rendu moderne) : les coins, le pied des murs, des palmiers et des
  personnages, le dessous des voitures reçoivent une ombre de contact douce (calculée à l'écran, flou qui
  respecte les bords, s'efface après 90 m).
- **Effets d'image** (Options vidéo > Effets d'image) : anticrénelage SMAA (SMAA 2.8, licence MIT), éclat (néons,
  soleil et phares débordent en lumière douce), étalonnage (`Etalonnage` : Original, Vice — couleurs « Miami 80 »
  qui suivent l'heure du jeu —, Film) et netteté adaptative (`Nettete`).
- **Réverbères, néons et enseignes éclairent vraiment** la nuit (`LampadairesEclairent`), avec ombres pour les
  plus proches ; leur tache peinte au sol est retirée. **Particules douces** (`ParticulesDouces`) : fumée et
  explosions ne coupent plus net contre le sol et les murs.
- **Ambiance** (Options vidéo > Ambiance) : routes mouillées sous la pluie (reflets des néons et des phares,
  flaques, ondes) et sols brillants des intérieurs, rayons de soleil, palmiers et arbres au vent, faisceaux des
  phares dans la pluie et le brouillard, brume au loin qui suit l'heure, reflets sur les carrosseries, lumière
  indirecte.
- **30 images/s par défaut** (`ImagesParSeconde`) : au-dessus, le jeu d'origine a des bogues (montée
  en véhicule).
- **Chargeur ASI intégré** : les mods `.asi` du dossier du jeu, de `scripts\` et de `plugins\` sont
  chargés (`ChargerASI=0` pour ne pas les charger). Ne pas installer l'Ultimate ASI Loader (même nom
  `dinput8.dll`).
- **Journaux** : `vccoop.log` pour la partie en cours et un fichier par partie dans `logs\` (50
  derniers).

## Mods partagés

`ModsPartages=1` (par défaut). Déposez vos mods dans `VCCoop\mods\`, dans les sous-dossiers que vous
voulez — leur nom ne sert qu'à vous y retrouver :

```
VCCoop\mods\ma ferrari\cheetah.dff
VCCoop\mods\ma ferrari\cheetah.txd
VCCoop\mods\ma ferrari\handling.cfg
```

Seul le nom des fichiers compte :

- `nom.dff` / `nom.txd` (et `.col`, `.ifp`) remplacent le modèle `nom` du jeu : voiture, arme,
  personnage ou bâtiment (`cheetah`, `infernus`, `colt45`...).
- `handling.cfg` (ou `handling.txt`, `*.handling`) : lignes de conduite au format du `handling.cfg`
  de Vice City ; chaque ligne remplace celle du jeu qui commence par le même nom de véhicule
  (`CHEETAH ...`).
- `carcols.dat` : lignes de couleurs (`cheetah, 1,1, 2,2 ...`), même règle.

Fonctionnement : à chaque lancement de partie, un `vccmods.img` est fabriqué à la racine du jeu à
partir de ces fichiers et chargé en priorité sur `gta3.img`. Les fichiers du jeu ne sont jamais
modifiés : videz le dossier et tout redevient d'origine. L'hôte envoie son dossier aux invités tout
seul (TCP, même port que la partie) ; un invité ne télécharge que ce qui lui manque, le salon montre
la progression et la partie ne peut pas démarrer tant que tout le monde n'est pas à jour. Les invités
chargent exactement les fichiers de l'hôte : tout le monde a les mêmes modèles.

Limites : remplacements seulement (pas de nouveaux identifiants de véhicule), 64 Mo par fichier,
noms de fichiers de modèle de 23 caractères au plus (extension comprise). Les `handling.xml` de GTA V
ne sont pas au format de Vice City : prendre la ligne `handling.cfg` fournie par l'auteur du mod.

## Interface personnalisée

- Texte des menus : presque noir à contour rose, plus petit et plus net que l'original
  (`StyleMenus`, `CouleurTexteMenus`, `CouleurContourMenus`, `CouleurSelectionMenus`,
  `TailleTexteMenus`) ; les pages de sauvegardes ont un texte blanc sur le fond et une barre de
  sélection rose.
- Images dans `VCCoop\interface\` : `fond_menu.png` (fond des menus, 1920×1080 ou 2560×1440),
  `logo.png` (512×512, transparent), `chargement1.jpg`, `chargement2.jpg`... (écrans de chargement,
  tirés au hasard), `fermeture.jpg` (image en quittant le jeu, à la place de « Greetings from Vice City »).
  Affichées à leur vraie taille sans déformation ; voir le `LISEZMOI.txt` du dossier.

## Réseau

L'hôte écoute sur **UDP 7790** (`Port` dans `vccoop.ini`) et sert les mods partagés sur le même port
en **TCP**. Par Internet, l'hôte ouvre les deux sur sa box, ou tout le monde passe par un VPN (Radmin
VPN, ZeroTier, Hamachi). Les invités règlent l'adresse de l'hôte dans le menu COOP.

## Bugs connus

Version de test. Un rapport avec les fichiers `logs\` des **deux** joueurs aide beaucoup.

1. **Dégâts de véhicule faits par un non-propriétaire** : les balles sont envoyées au propriétaire,
   pas la tôle froissée par un choc contre une copie ; le feu n'est pas synchronisé.
2. **Montée de loin** : l'animation de montée (portière, assise, saut sur la moto) se joue pour
   conducteurs et passagers, voitures et motos, quand le joueur est à côté de la porte ; s'il appuie
   à plus de 2,5 m, il est posé directement.
3. **Les personnages qui suivent (Lance...) suivent l'hôte** : pour ces missions, venez ensemble.
4. **Un invité qui a rechargé sa partie un nombre de fois différent de l'hôte** peut, rarement,
   recevoir une mauvaise variable de script lors d'une synchro complète (risque de plantage à
   l'étude).
5. **Seul l'hôte lance les missions d'histoire.**
6. **Pas de kick ni de mot de passe** pour le salon (choix, pour l'instant).
7. **Mods** : remplacements `.col` et `.ifp` non testés ; ajouts de carte (nouveaux bâtiments) et
   véhicules ajoutés non pris en charge.
8. **Immeubles** : un immeuble que l'hôte possède dans la sauvegarde qu'un invité charge appartient
   aussi à l'invité (c'est la sauvegarde du jeu qui le porte) ; un achat de l'invité sur un immeuble
   encore à vendre dans la sauvegarde de l'hôte peut être racheté après un rechargement (il coûte à
   nouveau).
9. **L'IA du jeu ignore les invités** : ennemis, gangs et police de l'hôte ne visent que l'hôte ; un
   passant frappé par un invité ne riposte pas.

## Compiler

Visual Studio 2022 Build Tools (x86, `/MT`), aucune autre dépendance : `build.cmd` compile
`src\*.cpp` en `build\dinput8.dll`. `dist\make-release.ps1 -Version <v>` compile et assemble le zip.
`run\` contient les scripts de test à deux instances (`run\make-testinstances.ps1` les crée sous
`D:\Games\COOPTEST`) ; `re\` les outils Ghidra en ligne de commande qui ont servi à lire le jeu.

L'exécutable du jeu et tout code décompilé ne sont **pas** dans ce dépôt.

## Crédits et licence

- Adresses de Vice City 1.0 : lues avec Ghidra, recoupées avec [plugin-sdk](https://github.com/DK22Pac/plugin-sdk)
  (DK22Pac).
- Grand Theft Auto et Vice City sont des marques de Rockstar Games / Take-Two Interactive. Projet de
  fan, non officiel, non commercial, sans lien avec eux ; il faut posséder le jeu.
- Licence : pas encore choisie (dépôt privé).

Si le mod vous plaît, vous pouvez [me soutenir sur Ko-fi](https://ko-fi.com/parricidium). ❤️

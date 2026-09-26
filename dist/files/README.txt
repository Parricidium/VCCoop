VCCoop - GTA Vice City story missions in co-op (2 to 4 players)
===============================================================

Test build. The host plays the story; the other players join them in the
missions: they see the same characters, vehicles, cutscenes, text and radar
blips, and their shots count on the host.

REQUIREMENTS
------------
- GTA Vice City for PC with gta-vc.exe version 1.0 (3,088,896 bytes).
  The Steam version must be downgraded to 1.0 (community "downgrader").
  Every player needs their own copy of the game in 1.0.
- A connection between players: a virtual LAN is easiest (Radmin VPN,
  ZeroTier, Hamachi). Otherwise the host forwards UDP port 7790 and shares
  their public IP.

INSTALL
-------
1. Copy every file of this zip into the game folder (next to gta-vc.exe).
   Tip: use a separate copy of the game folder for co-op.
2. Open vccoop.ini and set your nickname ("Pseudo").

PLAY
----
- Start the game normally (gta-vc.exe). In the main menu: COOP.
- The host picks "Host a game", then confirms the new game.
- The others set "Address" (the host's IP: Enter, type it, Enter) and their
  "Nickname", then "Join": once connected, they confirm the new game.
- Without going through COOP, the game stays single player.
- Shortcuts are still there: "VCCoop - Heberger.cmd" / "VCCoop - Rejoindre.cmd".
- The host plays the missions as in single player. Guests are placed next to
  the host at the start and the end of every mission.
- F key (enter vehicle) next to another player's vehicle: you take the first
  free seat (driver or passenger) instead of pulling them out. As a passenger,
  F gets you out. The G key does the same.
- Each player has a colour (blue = host, orange, green, purple): a dot on the
  radar and map, and their name above their head (AfficherPseudos=0 in
  vccoop.ini to hide names).
- F7: choose your outfit (Tommy's, story characters, any pedestrian).
  Left / Right to browse, Enter to keep, Backspace to cancel. It is kept for
  the next sessions; missions that dress Tommy replace it, as in single player.
- COOP > Host a game: lobby with the connected players, then "New game" or
  "Load a game". Guests (COOP > Join) wait in the lobby and follow the host
  by themselves once he is in game (same save, or new game).
- Draw distance (COOP menu, 100 to 400 %): details, pedestrians and vehicles
  visible farther away (DistanceAffichage=200 by default).
- Tab (hold): player list (health, armour, ping, distance).
- B: set a meeting point where you look (everyone sees it, in your colour);
  B again removes it.
- Passenger: look left / right and fire, like the driver (pistols, SMGs).
- In a vehicle, the mouse orbits the camera around it (it goes back behind
  after 2.5 s). Right click with a pistol or SMG: aim at the screen centre,
  left click to fire, as driver or passenger. The mouse no longer steers.
  CameraLibre=0 for the original camera, SensibiliteCamera=100 (percent).
- The radio is the driver's (chosen station, or off).
- Side missions (taxi, ambulance, fire truck, vigilante, pizza): a guest can
  play them on their side in the right vehicle.
- Mission money goes to everyone (ArgentPartage), police stars are shared
  (RecherchePartagee).
- A guest who dies or is busted respawns at the nearest hospital / police
  station, keeping weapons and money (GarderArmes).
- Network drop: the guest reconnects by itself, without reloading.
- COOP menu: friendly fire, shared money, names, keep weapons.
- Players can hurt each other (punches, bullets, cars): set TirAmi=0 in the
  host's vccoop.ini to prevent it.

SHARED WORLD
------------
When a guest is near the host (under 150 m), they see the same pedestrians and
traffic. Farther away (over 210 m), they get their own city around them.

DISPLAY
-------
By default the game runs borderless fullscreen at your desktop resolution,
in your screen's real aspect ratio (16:9, 21:9, 32:9): a wider view with no
stretching, and the HUD keeps its proportions. Settings in vccoop.ini
(Fenetre, GrandEcran). Menus keep their proportions, centred.

CUSTOM INTERFACE
----------------
Menu text is white with a pink outline, smaller and sharper (StyleMenus,
CouleurTexteMenus, CouleurContourMenus, TailleTexteMenus in vccoop.ini). Menu
background, logo and loading screens: put your images in VCCoop\interface
(fond_menu.png, logo.png, chargement1.jpg, chargement2.jpg... 1920x1080 or
2560x1440 for full-screen images, 512x512 transparent PNG for the logo).

KNOWN LIMITS (test build)
-------------------------
- Only the host can start missions. "Go there" / "get in that car" goals can
  be met by any player, and guests see the destination markers. A character
  who follows the player (Lance...) follows the host: go together on those.
- No friendly fire between players. Other players' gunfire is shown for
  bullet weapons (not grenades or rockets yet).
- Saves: to resume, the host loads their save (Start Game > Load, or Pause >
  Load): it is sent to the guests, who load it automatically from their slot
  8 (reserved for co-op: its previous content is replaced). A guest who joins
  later gets it too. The host's story progress then reaches the guests
  mission after mission.
- If something goes wrong, vccoop.log (in the game folder) helps; each
  session's log is also kept in the logs folder (last 50).

OTHER MODS (.asi)
-----------------
VCCoop uses dinput8.dll, the same name as the "Ultimate ASI Loader": don't
install it, VCCoop loads the .asi files from the game folder, scripts\ and
plugins\ itself (ChargerASI=0 to turn this off). Avoid mods that already do
what VCCoop does (widescreen, frame limiter) and mods that change missions:
the host and the guests must have the same ones.

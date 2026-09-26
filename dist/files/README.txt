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
- Players can hurt each other (punches, bullets, cars): set TirAmi=0 in the
  host's vccoop.ini to prevent it.

SHARED WORLD
------------
When a guest is near the host (under 120 m), they see the same pedestrians and
traffic. Farther away (over 170 m), they get their own city around them.

DISPLAY
-------
By default the game runs borderless fullscreen at your desktop resolution,
in your screen's real aspect ratio (16:9, 21:9, 32:9): a wider view with no
stretching, and the HUD keeps its proportions. Settings in vccoop.ini
(Fenetre, GrandEcran). Menus keep their proportions, centred.

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
- If something goes wrong, vccoop.log (in the game folder) helps.

OTHER MODS (.asi)
-----------------
VCCoop uses dinput8.dll, the same name as the "Ultimate ASI Loader": don't
install it, VCCoop loads the .asi files from the game folder, scripts\ and
plugins\ itself (ChargerASI=0 to turn this off). Avoid mods that already do
what VCCoop does (widescreen, frame limiter) and mods that change missions:
the host and the guests must have the same ones.

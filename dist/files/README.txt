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
- G key: ride as a passenger in another player's vehicle (or get out).

KNOWN LIMITS (test build)
-------------------------
- Only the host can start missions; mission conditions (go there, get in
  that car...) are checked on the host.
- No friendly fire between players. Other players' gunfire is shown for
  bullet weapons (not grenades or rockets yet).
- No save sharing yet: for now everyone starts a new game together. The
  host's story progress is passed to guests mission after mission.
- If something goes wrong, vccoop.log (in the game folder) helps.

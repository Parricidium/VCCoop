// Correctifs du code du jeu (adresses de gta-vc.exe 1.0).
#include "util.h"
#include "vccoop.h"
#include <string.h>

// N'applique un correctif que si les octets attendus sont bien la : une adresse fausse ne doit rien casser.
static bool Expect(uintptr_t addr, const char *what, const uint8_t *bytes, size_t n)
{
    if (memcmp((void *)addr, bytes, n) == 0) return true;
    Log("correctif '%s' ignore : octets inattendus en 0x%06X", what, (unsigned)addr);
    return false;
}

// Retour du focus (WM_SETFOCUS, 0x4A4FD0) : le jeu ouvre le menu pause (m_bStartUpFrontEndRequested, menu +0x12).
// En coop le monde ne s'arrete pas pour autant et l'invite se retrouvait au menu apres un Alt+Tab : seulement hors
// coop.
static void __cdecl FocusBackMenu()
{
    if (!CoopNetworkStarted()) *(bool *)(0x869630 + 0x12) = true;
}

void InstallGamePatches()
{
    static const uint8_t focusMenu[] = { 0xC6, 0x05, 0x42, 0x96, 0x86, 0x00, 0x01 };
    if (Expect(0x4A4FFC, "menu au retour du focus", focusMenu, sizeof(focusMenu)))
        PatchCall(0x4A4FFC, (void *)FocusBackMenu, sizeof(focusMenu));

    // WinMain (0x5FFAB0) : sans le premier plan (ForegroundApp 0x6D59FC == 0) la boucle principale
    // s'endort dans WaitMessage. En coop, aucune instance ne doit se figer quand elle perd le focus :
    // on retire le "jz" qui mene a l'attente.
    static const uint8_t jzWait[] = { 0x0F, 0x84, 0x23, 0x05, 0x00, 0x00 };
    if (Expect(0x5FFF87, "boucle au premier plan", jzWait, sizeof(jzWait)))
        PatchNop(0x5FFF87, sizeof(jzWait));

    // Machine a etats de WinMain (gGameState 0x9B5F08) : 0 demarrage, 1-2 logo, 3-4 intro, 5 init,
    // 6-7 menu, 8-9 partie. WinMain remet l'etat a 0 lui-meme ; on passe plutot par le drapeau 0x974BFC
    // qu'il teste avant chaque video (etats 1, 2 et 4) : a 1, logo et intro sont sautes. Sans lui, une
    // instance sans premier plan reste bloquee sur la video (DirectShow est en pause hors focus).
    if (g_cfg.skipIntro) {
        *(int *)0x974BFC = 1;
        Log("videos d'intro sautees");
    }

    // psSelectDevice (0x600DA0), premier lancement (pas de gta_vc.set) : le jeu exige un mode 640x480
    // en 16 bits et affiche "Cannot find 640x480 video mode" s'il n'y en a pas (Windows 11 ne propose
    // pas toujours le 16 bits). On retire la condition sur la profondeur (jnz apres cmp [esp+8],0x10).
    static const uint8_t jnzDepth[] = { 0x75, 0x09 };
    if (Expect(0x600E93, "mode 640x480 16 bits", jnzDepth, sizeof(jnzDepth)))
        PatchNop(0x600E93, sizeof(jnzDepth));
}

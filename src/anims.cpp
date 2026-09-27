// Animations d'action recopiees : l'expediteur envoie ses animations en cours hors marche (id, groupe, instant),
// le destinataire les pose sur la copie du personnage et les retire quand l'original ne les a plus.
#include "util.h"
#include "vccoop.h"
#include "game.h"
#include "anims.h"
#include <math.h>
#include <string.h>

using namespace game;

// Marcher, courir, sprinter, attendre, demarrer, s'arreter : ids 0 a 6 des groupes de marche (gerees par l'etat de
// deplacement, SetMoveAnim).
static bool Locomotion(int id) { return id >= 0 && id <= 6; }
static void SetAnimTime(void *assoc, float t) { ((void(__thiscall *)(void *, float))0x401700)(assoc, t); }

static void *FindAnim(void *clump, int id)
{
    for (void *a = FirstAssoc(clump); a; a = NextAssoc(a)) if (Field<int16_t>(a, 0x2C) == id) return a;
    return NULL;
}

static bool Mirrored(const AnimMirror &m, int id)
{
    for (int i = 0; i < m.count; i++) if (m.ids[i] == id) return true;
    return false;
}

void CollectAnimSlots(void *ped, AnimSlot *out, int n)
{
    for (int i = 0; i < n; i++) out[i].id = -1;
    for (void *a = FirstAssoc(Field<void *>(ped, 0x4C)); a; a = NextAssoc(a)) {
        int id = Field<int16_t>(a, 0x2C);
        float blend = Field<float>(a, 0x18);
        // Seulement les animations partielles (drapeau 0x10) encore vivantes. Une animation de tout le corps (assise
        // en voiture, attente...) recopiee sur un pantin remplacait sa marche, et une animation en fondu negatif est
        // deja en train de mourir : recopiee, elle mourait aussi chez les autres et la liste des animations du pantin
        // se vidait (plantage 0x403ED2 du moteur, cf. EnsureLiveAnim).
        if (Locomotion(id) || blend < 0.05f || Field<float>(a, 0x1C) < 0.0f || !(Field<uint16_t>(a, 0x2E) & 0x10)) continue;
        AnimSlot s = { (int16_t)id, (uint8_t)Field<int16_t>(a, 0xE), (uint8_t)(blend >= 1.0f ? 255 : blend * 255.0f),
                       Field<float>(a, 0x20) };
        for (int i = 0; i < n; i++) {   // tri par visibilite (insertion)
            if (out[i].id >= 0 && out[i].blend >= s.blend) continue;
            AnimSlot t = out[i]; out[i] = s; s = t;
            if (s.id < 0) break;
        }
    }
}

bool ApplyActionAnims(void *ped, const AnimSlot *slots, int n, AnimMirror &m)
{
    void *clump = Field<void *>(ped, 0x4C);
    bool action = false;
    for (int k = 0; k < n; k++) {
        const AnimSlot &a = slots[k];
        if (a.id < 0 || Locomotion(a.id) || !AnimAvailable(a.group, a.id)) continue;
        void *assoc = FindAnim(clump, a.id);
        if (assoc && Field<float>(assoc, 0x1C) < 0.0f && !(Field<uint16_t>(assoc, 0x2E) & 0x10)) continue;   // meurt : on ne la reprend pas
        if (!assoc) {
            assoc = BlendAnimation(clump, a.group, a.id, 8.0f);
            if (!assoc) continue;
            // Pas partielle (tout le corps) : elle a fondu la marche / l'attente ; on la retire aussitot et on laisse
            // EnsureLiveAnim remettre une base (n'arrive plus depuis que l'expediteur ne les envoie plus).
            if (!(Field<uint16_t>(assoc, 0x2E) & 0x10)) { Field<float>(assoc, 0x1C) = -1000.0f; Field<uint16_t>(assoc, 0x2E) |= 0x4; continue; }
            Field<uint16_t>(assoc, 0x2E) |= 0x8;   // gardee quand la marche change (SetMoveAnim la supprimait, elle clignotait)
            SetAnimTime(assoc, a.time);
            if (!Mirrored(m, a.id) && m.count < 6) m.ids[m.count++] = a.id;
            if (g_cfg.logScripts) { static int logged; if (logged++ < 40) Log("animations : %p joue %d (groupe %d) a %.2f s", ped, a.id, a.group, a.time); }
        } else if (fabsf(Field<float>(assoc, 0x20) - a.time) > 0.3f) {
            SetAnimTime(assoc, a.time);
        }
        if (!(Field<uint16_t>(assoc, 0x2E) & 0x10)) action = true;   // pas "partielle" : tout le corps
    }
    // Celles que l'original n'a plus : effacees en douceur.
    for (int i = 0; i < m.count;) {
        bool still = false;
        for (int k = 0; k < n; k++) still |= slots[k].id == m.ids[i];
        if (still) { i++; continue; }
        void *assoc = FindAnim(clump, m.ids[i]);
        if (assoc && Field<float>(assoc, 0x1C) >= 0.0f) Field<float>(assoc, 0x1C) = -8.0f;
        m.ids[i] = m.ids[--m.count];
    }
    if (action) { m.inAction = true; return true; }
    // Fin de l'action : SetMoveAnim ne fait rien si l'etat de deplacement n'a pas change (memorise en +0x250) ; on
    // l'oblige a remettre la marche ou l'attente, sinon le personnage restait fige.
    if (m.inAction) { m.inAction = false; Field<int>(ped, 0x250) = -1; }
    return false;
}

// Nombre d'associations qui ne sont pas en train de disparaitre.
static int LiveAnims(void *ped)
{
    int n = 0;
    for (void *a = FirstAssoc(Field<void *>(ped, 0x4C)); a; a = NextAssoc(a)) if (Field<float>(a, 0x1C) >= 0.0f) n++;
    return n;
}

void ClearLocalReactions(void *ped, AnimMirror &m)
{
    int live = LiveAnims(ped);
    for (void *a = FirstAssoc(Field<void *>(ped, 0x4C)); a; a = NextAssoc(a)) {
        int id = Field<int16_t>(a, 0x2C);
        bool reaction = (id >= 13 && id <= 44) || (id >= 137 && id <= 140) || (id >= 144 && id <= 149);
        if (!reaction || Field<float>(a, 0x1C) < 0.0f || Mirrored(m, id)) continue;
        if (live <= 1) break;   // derniere animation vivante : on la garde (EnsureLiveAnim remettra la base)
        Field<float>(a, 0x1C) = -1000.0f;
        Field<uint16_t>(a, 0x2E) |= 0x4;   // supprimee une fois a zero (sinon elle restait morte dans la liste)
        live--;
        m.inAction = true;   // la marche sera remise a la fin
    }
}

// Le moteur plante (0x403ED2, FrameUpdateCallBack) des qu'un personnage n'a plus AUCUNE association d'animation :
// toutes en fondu negatif avec suppression dans la meme mise a jour. Si plus rien ne vit, on remet l'attente.
void EnsureLiveAnim(void *ped)
{
    if (LiveAnims(ped) > 0) return;
    BlendAnimation(Field<void *>(ped, 0x4C), Field<int>(ped, 0x1F4), 3, 8.0f);   // ANIM_IDLE_STANCE
    static int logged;
    if (logged++ < 20) Log("animations : %p n'avait plus aucune animation, attente remise", ped);
}

// Filet cote moteur : les deux callbacks de mise a jour d'image (RpAnimBlendClumpUpdateAnimations -> ForAllFrames,
// avec ou sans extraction de vitesse) lisent nodes[0] sans test ; avec une liste vide ils plantent en 0x403ED2.
typedef void(__cdecl *FrameCb_t)(void *frame, void *updateData);
static FrameCb_t o_FrameCbVel, o_FrameCb;
static void __cdecl h_FrameCbVel(void *frame, void *ud) { if (!((void **)ud)[1]) return; o_FrameCbVel(frame, ud); }
static void __cdecl h_FrameCb(void *frame, void *ud) { if (!((void **)ud)[1]) return; o_FrameCb(frame, ud); }

void InstallAnimGuards()
{
    static const uint8_t vel[] = { 0x53, 0x56, 0x57, 0x55, 0x83, 0xEC, 0x50 };
    static const uint8_t plain[] = { 0x53, 0x56, 0x57, 0x55, 0x83, 0xEC, 0x48 };
    o_FrameCbVel = (FrameCb_t)MakeDetour(0x4042D0, vel, sizeof(vel), (void *)h_FrameCbVel);
    o_FrameCb = (FrameCb_t)MakeDetour(0x403700, plain, sizeof(plain), (void *)h_FrameCb);
    Log("animations : garde-fou pose sur les mises a jour d'image (liste vide)");
}

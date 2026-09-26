// Animations d'action recopiees : l'expediteur envoie ses animations en cours hors marche (id, groupe, instant),
// le destinataire les pose sur la copie du personnage et les retire quand l'original ne les a plus.
#include "util.h"
#include "vccoop.h"
#include "game.h"
#include "anims.h"
#include <math.h>

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
        if (Locomotion(id) || blend < 0.05f) continue;
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
        if (!assoc) {
            assoc = BlendAnimation(clump, a.group, a.id, 8.0f);
            if (!assoc) continue;
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

void ClearLocalReactions(void *ped, AnimMirror &m)
{
    for (void *a = FirstAssoc(Field<void *>(ped, 0x4C)); a; a = NextAssoc(a)) {
        int id = Field<int16_t>(a, 0x2C);
        bool reaction = (id >= 13 && id <= 44) || (id >= 137 && id <= 140) || (id >= 144 && id <= 149);
        if (!reaction || Field<float>(a, 0x1C) < 0.0f || Mirrored(m, id)) continue;
        Field<float>(a, 0x1C) = -1000.0f;
        m.inAction = true;   // la marche sera remise a la fin
    }
}

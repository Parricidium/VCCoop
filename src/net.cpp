#include <winsock2.h>
#include <ws2tcpip.h>
#include "util.h"
#include "vccoop.h"
#include "net.h"
#include "conditions.h"
#include "mirror.h"
#include <string.h>
#include <stdlib.h>

NetPlayer g_players[MAX_PLAYERS];
int g_localId = -1;
void (*g_onWorld)(const MsgWorld &w);
void (*g_onState)(const MsgState &s);
void (*g_onVehicle)(const MsgVehicle &v);
void (*g_onVehRemove)(uint32_t id);
void (*g_onPed)(const MsgPed &p);
void (*g_onPedRemove)(uint8_t owner, uint32_t handle);

// Messages "d'entite" : traites localement et, chez l'hote, relayes aux autres invites.
static bool HandleEntityMsg(const uint8_t *buf, int len)
{
    if (buf[0] == MSG_VEHICLE && len >= (int)sizeof(MsgVehicle)) {
        if (g_onVehicle) g_onVehicle(*(const MsgVehicle *)buf);
        return true;
    }
    if (buf[0] == MSG_VEH_REMOVE && len >= (int)sizeof(MsgVehRemove)) {
        if (g_onVehRemove) g_onVehRemove(((const MsgVehRemove *)buf)->id);
        return true;
    }
    // Personnages : de l'hote (ses passants, ses personnages de mission) ou d'un invite (sa police).
    if (buf[0] == MSG_PED && len >= (int)sizeof(MsgPed)) {
        if (g_onPed) g_onPed(*(const MsgPed *)buf);
        return true;
    }
    if (buf[0] == MSG_PED_REMOVE && len >= (int)sizeof(MsgPedRemove)) {
        const MsgPedRemove *r = (const MsgPedRemove *)buf;
        if (g_onPedRemove) g_onPedRemove(r->owner, r->handle);
        return true;
    }
    return false;
}

static SOCKET g_sock = INVALID_SOCKET;
static sockaddr_in g_hostAddr;               // invite : adresse de l'hote
static sockaddr_in g_peerAddr[MAX_PLAYERS];  // hote : adresse de chaque invite
static uint32_t g_lastHello;
void (*g_onReliable)(int from, const uint8_t *data, int len);
void (*g_onJoin)(int peer);
bool g_peerRejoin[MAX_PLAYERS];
void (*g_onNotice)(const char *fr, const char *en, int player);
static bool g_everAccepted;   // invite : deja accepte par l'hote pendant cette session
static uint32_t NewSession()
{
    // L'octet bas sert de generation au flux fiable : jamais le meme que la session precedente.
    static uint32_t prev;
    uint32_t s;
    do s = (GetTickCount() * 2654435761u) ^ (GetCurrentProcessId() << 7) ^ ((uint32_t)rand() << 3) ^ (uint32_t)rand();
    while ((uint8_t)s == (uint8_t)prev);
    prev = s;
    return s;
}
uint32_t g_netMuteUntil, g_netMuteSendUntil;   // autotests : coupure totale / envoi seulement
uint16_t g_myPing;
void (*g_onRdv)(const MsgRdv &r);      // autotest : simule une coupure (rien n'entre ni ne sort)

// --- Flux fiables : un par pair (hote : un par invite ; invite : un seul, vers l'hote, indice 0) ---
enum { RL_QUEUE = 2048 };   // la mission 0 fait reproduire ~400 commandes d'un coup
struct RlOut { uint32_t seq; uint16_t len; uint8_t data[MAX_RELIABLE_PAYLOAD]; };
struct RlStream {
    RlOut queue[RL_QUEUE];   // anneau des messages non acquittes
    uint32_t nextSeq;        // prochain numero a attribuer (commence a 1)
    uint32_t acked;          // plus grand numero acquitte (cumulatif)
    uint32_t expected;       // reception : prochain numero attendu
    uint32_t lastResend;
    uint32_t stuckSince;     // emission : depuis quand le meme numero est renvoye sans acquittement
    uint32_t gapSince;       // reception : depuis quand on recoit des numeros au-dela de celui attendu
};
static RlStream g_rl[MAX_PLAYERS];

static void SendTo(const sockaddr_in &to, const void *data, int len);
static void RlReset(RlStream &r) { memset(&r, 0, sizeof(r)); r.nextSeq = 1; r.expected = 1; }
// Un invite qui charge une sauvegarde ou gele un instant ne presente plus d'image pendant plusieurs secondes ; les
// messages de vie partent d'un fil a part (NetKeepAlive), le delai n'a donc plus a etre court.
enum { TIMEOUT_MS = 15000 };
// Generation du flux fiable : les 8 bits bas de la session de l'invite (MsgHello.session), connue des deux cotes. Un
// paquet ou un accuse d'une autre generation (ancien flux, avant une reconnexion) est ignore sans acquitter : avant,
// un accuse perime faisait croire a l'hote que les premiers messages du nouveau flux etaient livres, et l'invite
// attendait pour toujours le n1 (plus aucune commande de mission, ni argent, ni variables : le bug de JD du 26/09).
static uint32_t g_session;    // invite : numero de la connexion en cours (nouveau a chaque reconnexion)
static uint32_t g_peerSession[MAX_PLAYERS];   // hote : session de chaque invite
static uint8_t RlGen(int peer) { return (uint8_t)(g_cfg.host ? g_peerSession[peer] : g_session); }
enum { RL_HDR = 6 };   // type, generation, seq (4)
static void Disconnect(int i);

bool NetIsHost() { return g_cfg.host; }

bool NearOtherPlayer(const float *p, uint8_t area, float r)
{
    for (int i = 0; i < MAX_PLAYERS; i++) {
        const NetPlayer &g = g_players[i];
        if (i == g_localId || !g.connected || !g.state.inGame || g.state.area != area || GetTickCount() - g.lastStateAt > 3000) continue;
        float dx = g.state.pos[0] - p[0], dy = g.state.pos[1] - p[1], dz = g.state.pos[2] - p[2];
        if (dx * dx + dy * dy + dz * dz < r * r) return true;
    }
    return false;
}

bool NearAnyGuest(const float *p, uint8_t area, float r)
{
    for (int i = 1; i < MAX_PLAYERS; i++) {
        const NetPlayer &g = g_players[i];
        if (!g.connected || !g.state.inGame || g.state.area != area || GetTickCount() - g.lastStateAt > 3000) continue;
        float dx = g.state.pos[0] - p[0], dy = g.state.pos[1] - p[1], dz = g.state.pos[2] - p[2];
        if (dx * dx + dy * dy + dz * dz < r * r) return true;
    }
    return false;
}

static void SendTo(const sockaddr_in &to, const void *data, int len)
{
    if ((g_netMuteUntil && GetTickCount() < g_netMuteUntil) || (g_netMuteSendUntil && GetTickCount() < g_netMuteSendUntil)) return;
    sendto(g_sock, (const char *)data, len, 0, (const sockaddr *)&to, sizeof(to));
}

void NetStop()
{
    if (g_sock == INVALID_SOCKET) return;
    NetSendBye();
    SOCKET s = g_sock;
    g_sock = INVALID_SOCKET;
    closesocket(s);
    for (int i = 0; i < MAX_PLAYERS; i++) g_players[i].connected = false;
    for (auto &r : g_rl) RlReset(r);
    g_localId = -1;
    WSACleanup();
    Log("reseau : arrete (salon ferme ou deconnexion)");
}

static const sockaddr_in *PeerAddr(int peer);

static void RlSendOne(int peer, const RlOut &o)
{
    uint8_t pkt[RL_HDR + MAX_RELIABLE_PAYLOAD];
    pkt[0] = MSG_RELIABLE;
    pkt[1] = RlGen(peer);
    memcpy(pkt + 2, &o.seq, 4);
    memcpy(pkt + RL_HDR, o.data, o.len);
    if (const sockaddr_in *a = PeerAddr(peer)) SendTo(*a, pkt, RL_HDR + o.len);
}

static void RlPush(int peer, const void *data, int len)
{
    RlStream &r = g_rl[peer];
    if (r.nextSeq - r.acked > RL_QUEUE) {
        // Perdre un message au milieu d'un flux ordonne le casse pour de bon (trou invisible) : on coupe le pair,
        // il reviendra avec une nouvelle session et tout lui sera renvoye.
        Log("reseau : file fiable pleine pour %d, coupure pour resynchroniser", peer);
        Disconnect(peer);
        return;
    }
    if (g_cfg.logScripts && ((const uint8_t *)data)[0] == 4) Log("reseau : fiable n%u vers %d, %d octets", r.nextSeq, peer, len);
    RlOut &o = r.queue[r.nextSeq % RL_QUEUE];
    o.seq = r.nextSeq++;
    o.len = (uint16_t)len;
    memcpy(o.data, data, len);
    RlSendOne(peer, o);
}

static void RlResend(int peer, uint32_t now)
{
    RlStream &r = g_rl[peer];
    if (r.acked + 1 >= r.nextSeq) { r.stuckSince = 0; return; }
    if (!r.stuckSince) r.stuckSince = now;
    // Le meme premier message renvoye depuis 10 s sans accuse : le flux du pair est perdu (ou il n'accuse pas).
    // Hote : on lui demande de se reconnecter (nouvelle session, etat complet renvoye). Invite : l'hote ne repond
    // pas a nos messages fiables, on refrappe a la porte.
    if (now - r.stuckSince > 10000) {
        Log("reseau : flux fiable vers %d bloque (n%u sans accuse depuis 10 s)", peer, r.acked + 1);
        r.stuckSince = now;
        if (g_cfg.host) { uint8_t m = MSG_RESYNC; if (const sockaddr_in *a = PeerAddr(peer)) SendTo(*a, &m, 1); }
        else Disconnect(0);
        return;
    }
    if (now - r.lastResend < 150) return;
    r.lastResend = now;
    int sent = 0;
    for (uint32_t s = r.acked + 1; s < r.nextSeq && sent < 32; s++, sent++) RlSendOne(peer, r.queue[s % RL_QUEUE]);
}

// Reception : n'accepte que le numero attendu (les autres seront renvoyes), acquitte toujours ; une autre
// generation est ignoree sans accuse.
static void RlReceive(int peer, const uint8_t *buf, int len)
{
    if (len < RL_HDR + 1 || len - RL_HDR > MAX_RELIABLE_PAYLOAD) return;
    RlStream &r = g_rl[peer];
    if (buf[1] != RlGen(peer)) return;
    uint32_t seq, now = GetTickCount();
    memcpy(&seq, buf + 2, 4);
    if (g_cfg.logScripts && buf[RL_HDR] == 4) Log("reseau : fiable n%u (attendu %u) type %d, %d octets", seq, r.expected, buf[RL_HDR], len - RL_HDR);
    if (seq == r.expected) {
        r.expected++;
        r.gapSince = 0;
        if (g_onReliable) g_onReliable(peer, buf + RL_HDR, len - RL_HDR);
    } else if (seq > r.expected) {
        // On ne recoit plus que la suite : le message attendu ne viendra jamais (ancien flux, file pleine chez
        // l'autre). Invite : on repart d'une nouvelle session, l'hote renverra tout.
        if (!r.gapSince) r.gapSince = now;
        else if (now - r.gapSince > 5000) {
            Log("reseau : flux fiable de %d desynchronise (attendu n%u, recu n%u depuis 5 s)", peer, r.expected, seq);
            r.gapSince = 0;
            if (!g_cfg.host) { Disconnect(0); return; }
            uint8_t m = MSG_RESYNC;
            if (const sockaddr_in *a = PeerAddr(peer)) SendTo(*a, &m, 1);
        }
    }
    uint8_t ack[RL_HDR] = { MSG_ACK, RlGen(peer) };
    uint32_t a = r.expected - 1;
    memcpy(ack + 2, &a, 4);
    if (const sockaddr_in *addr = PeerAddr(peer)) SendTo(*addr, ack, RL_HDR);
}

static void RlAck(int peer, const uint8_t *buf, int len)
{
    if (len < RL_HDR || buf[1] != RlGen(peer)) return;
    uint32_t a;
    memcpy(&a, buf + 2, 4);
    if (a > g_rl[peer].acked && a < g_rl[peer].nextSeq) { g_rl[peer].acked = a; g_rl[peer].stuckSince = 0; }
}

void NetSendReliableTo(int peer, const void *data, int len)
{
    if (g_sock == INVALID_SOCKET || !g_cfg.host || peer <= 0 || peer >= MAX_PLAYERS || !g_players[peer].connected) return;
    if (len <= MAX_RELIABLE_PAYLOAD) RlPush(peer, data, len);
}

void NetSendReliable(const void *data, int len)
{
    if (g_sock == INVALID_SOCKET || g_localId < 0 || len > MAX_RELIABLE_PAYLOAD) return;
    if (g_cfg.host) {
        for (int i = 1; i < MAX_PLAYERS; i++) if (g_players[i].connected) RlPush(i, data, len);
    } else {
        RlPush(0, data, len);
    }
}

bool NetStart()
{
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { Log("reseau : WSAStartup a echoue"); return false; }
    for (auto &r : g_rl) RlReset(r);
    srand(GetTickCount());
    g_session = NewSession();
    g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    u_long nb = 1;
    ioctlsocket(g_sock, FIONBIO, &nb);
    int bufSize = 1 << 20;   // rafales (sauvegarde partagee ~200 Ko, mission 0) : les tampons par defaut en perdaient
    setsockopt(g_sock, SOL_SOCKET, SO_RCVBUF, (const char *)&bufSize, sizeof(bufSize));
    setsockopt(g_sock, SOL_SOCKET, SO_SNDBUF, (const char *)&bufSize, sizeof(bufSize));

    sockaddr_in local = {};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = INADDR_ANY;
    local.sin_port = htons(g_cfg.host ? (u_short)g_cfg.port : 0);
    if (bind(g_sock, (sockaddr *)&local, sizeof(local)) != 0) {
        Log("reseau : impossible d'ouvrir le port %d (erreur %d)", g_cfg.port, WSAGetLastError());
        return false;
    }
    if (g_cfg.host) {
        g_localId = 0;
        g_players[0].connected = true;
        Log("reseau : hote sur le port UDP %d", g_cfg.port);
    } else {
        addrinfo hints = {}, *res = NULL;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        if (getaddrinfo(g_cfg.address, NULL, &hints, &res) != 0 || !res) {
            Log("reseau : adresse '%s' introuvable", g_cfg.address);
            return false;
        }
        g_hostAddr = *(sockaddr_in *)res->ai_addr;
        g_hostAddr.sin_port = htons((u_short)g_cfg.port);
        freeaddrinfo(res);
        Log("reseau : invite, connexion a %s:%d", g_cfg.address, g_cfg.port);
    }
    return true;
}

static const sockaddr_in *PeerAddr(int peer)
{
    if (g_cfg.host) return peer > 0 && peer < MAX_PLAYERS && g_players[peer].connected ? &g_peerAddr[peer] : NULL;
    return &g_hostAddr;
}

static bool SameAddr(const sockaddr_in &a, const sockaddr_in &b)
{
    return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
}

// Etats de joueur arrives dans le desordre (UDP) : on ne garde que les plus recents. Un numero bien plus petit
// que le dernier vient d'un jeu relance (le compteur repart de 1).
static bool StaleState(NetPlayer &p, const MsgState &s)
{
    if (p.connected && s.seq <= p.lastSeq && p.lastSeq - s.seq < 100000) return true;
    p.lastSeq = s.seq;
    return false;
}

static void Disconnect(int i)
{
    g_players[i].connected = false;
    // L'hote a disparu (ou notre flux est perdu) : on refrappe avec une nouvelle session et un flux fiable neuf, en
    // meme temps que l'hote remettra le sien a zero en voyant cette session.
    if (!g_cfg.host && i == 0) { g_localId = -1; g_session = NewSession(); RlReset(g_rl[0]); }
}

static void HostReceive(const uint8_t *buf, int len, const sockaddr_in &from)
{
    int id = -1;
    for (int i = 1; i < MAX_PLAYERS; i++)
        if (g_players[i].connected && SameAddr(g_peerAddr[i], from)) id = i;

    if (buf[0] == MSG_HELLO && len >= 2 && len < (int)sizeof(MsgHello)) {   // version plus ancienne (message plus court)
        MsgFull f = { MSG_FULL, 2, NET_VERSION };
        SendTo(from, &f, sizeof(f));
        Log("reseau : un invite d'une autre version (%d) frappe a la porte", buf[1]);
        return;
    }
    if (buf[0] == MSG_HELLO && len >= (int)sizeof(MsgHello)) {
        const MsgHello *h = (const MsgHello *)buf;
        // Invite deja connu qui refrappe avec une nouvelle session : il a perdu le contact (coupure d'un seul cote)
        // et repart d'un flux fiable neuf ; on fait pareil, sinon plus aucun message fiable ne passerait.
        if (id >= 0 && h->session != g_peerSession[id]) {
            g_peerSession[id] = h->session;
            RlReset(g_rl[id]);
            g_players[id].lastSeq = 0;
            g_peerRejoin[id] = h->rejoin;
            Log("reseau : %s (joueur %d) se reconnecte, flux fiable remis a zero", g_players[id].state.name, id);
            if (g_onNotice) g_onNotice("est de retour", "is back", id);
            if (g_onJoin) g_onJoin(id);
        }
        if (id < 0 && h->version == NET_VERSION)
            for (int i = 1; i < MAX_PLAYERS && id < 0; i++)
                if (!g_players[i].connected) {
                    id = i;
                    memset(&g_players[i], 0, sizeof(g_players[i]));
                    g_peerAddr[i] = from;
                    g_players[i].connected = true;
                    g_peerSession[i] = h->session;
                    RlReset(g_rl[i]);
                    lstrcpynA(g_players[i].state.name, h->name, sizeof(g_players[i].state.name));
                    g_peerRejoin[i] = len >= (int)sizeof(MsgHello) && h->rejoin;
                    Log("reseau : %s %s (joueur %d)", g_players[i].state.name, g_peerRejoin[i] ? "revient" : "entre", i);
                    if (g_onNotice) g_onNotice(g_peerRejoin[i] ? "est de retour" : "a rejoint la partie", g_peerRejoin[i] ? "is back" : "joined the game", i);
                    if (g_onJoin) g_onJoin(i);
                }
        if (id < 0) {
            MsgFull f = { MSG_FULL, (uint8_t)(h->version == NET_VERSION ? 1 : 2), NET_VERSION };
            SendTo(from, &f, sizeof(f));
            static uint32_t lastLog;
            if (GetTickCount() - lastLog > 5000) { lastLog = GetTickCount(); Log("reseau : %s refuse (%s, version %d)", h->name, f.reason == 1 ? "partie pleine" : "version differente", h->version); }
            return;
        }
        g_players[id].lastSeen = GetTickCount();
        MsgWelcome w = { MSG_WELCOME, (uint8_t)id };
        SendTo(from, &w, sizeof(w));
        return;
    }
    if (id < 0) return;
    g_players[id].lastSeen = GetTickCount();
    if (buf[0] == MSG_STATE && len >= (int)sizeof(MsgState)) {
        MsgState s = *(const MsgState *)buf;
        s.id = (uint8_t)id;
        if (StaleState(g_players[id], s)) return;
        g_players[id].state = s;
        g_players[id].lastStateAt = GetTickCount();
        if (g_onState) g_onState(s);
        for (int i = 1; i < MAX_PLAYERS; i++)   // relais aux autres invites
            if (i != id && g_players[i].connected) SendTo(g_peerAddr[i], &s, sizeof(s));
    } else if (buf[0] == MSG_RDV && len >= (int)sizeof(MsgRdv)) {
        MsgRdv r = *(const MsgRdv *)buf;
        r.player = (uint8_t)id;
        if (g_onRdv) g_onRdv(r);
        for (int i = 1; i < MAX_PLAYERS; i++)   // relais aux autres invites
            if (i != id && g_players[i].connected) SendTo(g_peerAddr[i], &r, sizeof(r));
    } else if (buf[0] == MSG_PING && len >= (int)sizeof(MsgPing)) {
        MsgPing p = *(const MsgPing *)buf;
        p.type = MSG_PONG;
        SendTo(g_peerAddr[id], &p, sizeof(p));
    } else if (buf[0] == MSG_RELIABLE) {
        RlReceive(id, buf, len);
    } else if (buf[0] == MSG_ACK) {
        RlAck(id, buf, len);
    } else if (HandleEntityMsg(buf, len)) {
        for (int i = 1; i < MAX_PLAYERS; i++)   // relais aux autres invites
            if (i != id && g_players[i].connected) SendTo(g_peerAddr[i], buf, len);
    } else if (buf[0] == MSG_BYE) {
        Log("reseau : joueur %d parti", id);
        Disconnect(id);
        MsgBye b = { MSG_BYE, (uint8_t)id };
        for (int i = 1; i < MAX_PLAYERS; i++)   // les autres invites n'attendent pas le delai
            if (i != id && g_players[i].connected) SendTo(g_peerAddr[i], &b, sizeof(b));
    }
}

static void GuestReceive(const uint8_t *buf, int len, const sockaddr_in &from)
{
    if (!SameAddr(from, g_hostAddr)) return;
    g_players[0].lastSeen = GetTickCount();
    switch (buf[0]) {
    case MSG_RELIABLE:
        RlReceive(0, buf, len);
        break;
    case MSG_RDV:
        if (len >= (int)sizeof(MsgRdv) && g_onRdv) g_onRdv(*(const MsgRdv *)buf);
        break;
    case MSG_PONG:
        if (len >= (int)sizeof(MsgPing)) {
            uint32_t rtt = GetTickCount() - ((const MsgPing *)buf)->time;
            g_myPing = (uint16_t)(rtt > 9999 ? 9999 : rtt);
        }
        break;
    case MSG_ACK:
        RlAck(0, buf, len);
        break;
    case MSG_RESYNC:
        if (g_localId >= 0) { Log("reseau : l'hote demande une resynchronisation"); Disconnect(0); }
        break;
    case MSG_WELCOME:
        if (len >= (int)sizeof(MsgWelcome) && g_localId != ((const MsgWelcome *)buf)->id) {
            g_localId = ((const MsgWelcome *)buf)->id;
            g_players[0].connected = true;
            Log("reseau : accepte par l'hote, joueur %d", g_localId);
            if (g_everAccepted && g_onNotice) g_onNotice("reconnecte a l'hote", "reconnected to the host", 0);
            g_everAccepted = true;
        }
        break;
    case MSG_FULL: {
        static uint32_t lastLog;
        const MsgFull *f = (const MsgFull *)buf;
        bool version = len >= (int)sizeof(MsgFull) && f->reason == 2;
        if (GetTickCount() - lastLog > 5000) {
            lastLog = GetTickCount();
            if (version) Log("reseau : l'hote refuse : version differente (la sienne %d, la notre %d)", f->hostVersion, NET_VERSION);
            else Log("reseau : l'hote refuse : partie pleine");
            if (g_onNotice) g_onNotice(version ? "version differente de l'hote : meme zip VCCoop pour tous" : "partie pleine",
                                       version ? "different version from the host: same VCCoop zip for everybody" : "game is full", 0);
        }
        break;
    }
    case MSG_STATE:
        if (len >= (int)sizeof(MsgState)) {
            const MsgState *s = (const MsgState *)buf;
            if (s->id < MAX_PLAYERS && s->id != g_localId) {
                NetPlayer &p = g_players[s->id];
                if (!p.connected) Log("reseau : %s (joueur %d) est la", s->name, s->id);
                if (StaleState(p, *s)) break;
                p.connected = true;
                p.state = *s;
                p.lastSeen = p.lastStateAt = GetTickCount();
                if (g_onState) g_onState(*s);
            }
        }
        break;
    case MSG_WORLD:
        if (len >= (int)sizeof(MsgWorld) && g_onWorld) g_onWorld(*(const MsgWorld *)buf);
        break;
    case MSG_VEHICLE:
    case MSG_VEH_REMOVE:
        HandleEntityMsg(buf, len);
        break;
    case MSG_PED:
        HandleEntityMsg(buf, len);
        break;
    case MSG_MARKER:
        OnMarker(buf, len);
        break;
    case MSG_CORONA:
        OnCorona(buf, len);
        break;
    case MSG_PED_REMOVE:
        HandleEntityMsg(buf, len);
        break;
    case MSG_TIMERS:
        MirrorOnTimers(buf, len);
        break;
    case MSG_BYE:
        if (len >= (int)sizeof(MsgBye) && ((const MsgBye *)buf)->id < MAX_PLAYERS && ((const MsgBye *)buf)->id != g_localId) {
            int who = ((const MsgBye *)buf)->id;
            Log("reseau : joueur %d parti", who);
            if (g_onNotice) g_onNotice(who == 0 ? "l'hote a quitte la partie" : "a quitte la partie", who == 0 ? "the host left the game" : "left the game", who);
            Disconnect(who);
        }
        break;
    }
}

void NetPoll()
{
    if (g_sock == INVALID_SOCKET) return;
    uint8_t buf[1500];
    sockaddr_in from;
    int fromLen;
    for (;;) {
        fromLen = sizeof(from);
        int len = recvfrom(g_sock, (char *)buf, sizeof(buf), 0, (sockaddr *)&from, &fromLen);
        if (len <= 0) break;
        if (g_netMuteUntil && GetTickCount() < g_netMuteUntil) continue;
        if (g_cfg.host) HostReceive(buf, len, from);
        else GuestReceive(buf, len, from);
    }

    uint32_t now = GetTickCount();
    if (g_cfg.host) { for (int i = 1; i < MAX_PLAYERS; i++) if (g_players[i].connected) RlResend(i, now); }
    else if (g_localId >= 0) RlResend(0, now);
    static uint32_t lastPing;
    if (!g_cfg.host && g_localId >= 0 && now - lastPing > 2000) {
        lastPing = now;
        MsgPing p = { MSG_PING, now };
        SendTo(g_hostAddr, &p, sizeof(p));
    }
    if (!g_cfg.host && g_localId < 0 && now - g_lastHello > 1000) {   // on frappe a la porte chaque seconde
        MsgHello h = { MSG_HELLO, NET_VERSION };
        lstrcpynA(h.name, g_cfg.playerName, sizeof(h.name));
        h.rejoin = g_everAccepted && *(int *)0x9B5F08 == 9;   // deja dans la partie : simple coupure
        h.session = g_session;
        SendTo(g_hostAddr, &h, sizeof(h));
        g_lastHello = now;
    }
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (i == g_localId || !g_players[i].connected) continue;
        if (now - g_players[i].lastSeen > TIMEOUT_MS) {
            Log("reseau : joueur %d ne repond plus", i);
            if (g_onNotice) g_onNotice(i == 0 && !g_cfg.host ? "l'hote ne repond plus, reconnexion..." : "ne repond plus",
                                       i == 0 && !g_cfg.host ? "the host is not responding, reconnecting..." : "is not responding", i);
            Disconnect(i);
        }
    }
}

// Depuis le fil du chien de garde (toutes les 3 s) : un signe de vie meme quand le fil du jeu ne presente plus
// d'image (chargement d'une sauvegarde, gel) ; sinon l'autre cote nous declarait parti et detruisait tout.
void NetKeepAlive()
{
    if (g_sock == INVALID_SOCKET || g_localId < 0) return;
    MsgPing p = { MSG_PING, GetTickCount() };
    if (g_cfg.host) { for (int i = 1; i < MAX_PLAYERS; i++) if (g_players[i].connected) SendTo(g_peerAddr[i], &p, sizeof(p)); }
    else SendTo(g_hostAddr, &p, sizeof(p));
}

void NetSendToGuests(const void *data, int len)
{
    if (g_sock == INVALID_SOCKET || !g_cfg.host) return;
    for (int i = 1; i < MAX_PLAYERS; i++)
        if (g_players[i].connected) SendTo(g_peerAddr[i], data, len);
}

void NetSendToAll(const void *data, int len)
{
    if (g_sock == INVALID_SOCKET || g_localId < 0) return;
    if (g_cfg.host) NetSendToGuests(data, len);
    else SendTo(g_hostAddr, data, len);
}

void NetSendBye()
{
    if (g_sock == INVALID_SOCKET || g_localId < 0) return;
    MsgBye b = { MSG_BYE, (uint8_t)g_localId };
    for (int k = 0; k < 2; k++) {   // deux fois : c'est de l'UDP et on ne reviendra pas
        if (g_cfg.host) NetSendToGuests(&b, sizeof(b)); else SendTo(g_hostAddr, &b, sizeof(b));
    }
}

void NetSendState(const MsgState &s)
{
    if (g_sock == INVALID_SOCKET || g_localId < 0) return;
    if (g_cfg.host) {
        for (int i = 1; i < MAX_PLAYERS; i++)
            if (g_players[i].connected) SendTo(g_peerAddr[i], &s, sizeof(s));
    } else {
        SendTo(g_hostAddr, &s, sizeof(s));
    }
}

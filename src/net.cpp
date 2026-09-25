#include <winsock2.h>
#include <ws2tcpip.h>
#include "util.h"
#include "vccoop.h"
#include "net.h"
#include "conditions.h"
#include <string.h>

NetPlayer g_players[MAX_PLAYERS];
int g_localId = -1;
void (*g_onWorld)(const MsgWorld &w);
void (*g_onVehicle)(const MsgVehicle &v);
void (*g_onVehRemove)(uint32_t id);
void (*g_onPed)(const MsgPed &p);
void (*g_onPedRemove)(uint32_t handle);

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
    return false;
}

static SOCKET g_sock = INVALID_SOCKET;
static sockaddr_in g_hostAddr;               // invite : adresse de l'hote
static sockaddr_in g_peerAddr[MAX_PLAYERS];  // hote : adresse de chaque invite
static uint32_t g_lastHello;
void (*g_onReliable)(int from, const uint8_t *data, int len);
void (*g_onJoin)(int peer);

// --- Flux fiables : un par pair (hote : un par invite ; invite : un seul, vers l'hote, indice 0) ---
enum { RL_QUEUE = 2048 };   // la mission 0 fait reproduire ~400 commandes d'un coup
struct RlOut { uint32_t seq; uint16_t len; uint8_t data[MAX_RELIABLE_PAYLOAD]; };
struct RlStream {
    RlOut queue[RL_QUEUE];   // anneau des messages non acquittes
    uint32_t nextSeq;        // prochain numero a attribuer (commence a 1)
    uint32_t acked;          // plus grand numero acquitte (cumulatif)
    uint32_t expected;       // reception : prochain numero attendu
    uint32_t lastResend;
};
static RlStream g_rl[MAX_PLAYERS];

static void SendTo(const sockaddr_in &to, const void *data, int len);
static void RlReset(RlStream &r) { memset(&r, 0, sizeof(r)); r.nextSeq = 1; r.expected = 1; }
enum { TIMEOUT_MS = 8000 };

bool NetIsHost() { return g_cfg.host; }

bool NearAnyGuest(const float *p, uint8_t area, float r)
{
    for (int i = 1; i < MAX_PLAYERS; i++) {
        const NetPlayer &g = g_players[i];
        if (!g.connected || !g.state.inGame || g.state.area != area) continue;
        float dx = g.state.pos[0] - p[0], dy = g.state.pos[1] - p[1], dz = g.state.pos[2] - p[2];
        if (dx * dx + dy * dy + dz * dz < r * r) return true;
    }
    return false;
}

static void SendTo(const sockaddr_in &to, const void *data, int len)
{
    sendto(g_sock, (const char *)data, len, 0, (const sockaddr *)&to, sizeof(to));
}

static const sockaddr_in *PeerAddr(int peer);

static void RlPush(int peer, const void *data, int len)
{
    RlStream &r = g_rl[peer];
    if (r.nextSeq - r.acked > RL_QUEUE) { Log("reseau : file fiable pleine pour %d, message perdu", peer); return; }
    if (g_cfg.logScripts && ((const uint8_t *)data)[0] == 4) Log("reseau : fiable n%u vers %d, %d octets", r.nextSeq, peer, len);
    RlOut &o = r.queue[r.nextSeq % RL_QUEUE];
    o.seq = r.nextSeq++;
    o.len = (uint16_t)len;
    memcpy(o.data, data, len);
    uint8_t pkt[8 + MAX_RELIABLE_PAYLOAD];
    pkt[0] = MSG_RELIABLE;
    memcpy(pkt + 1, &o.seq, 4);
    memcpy(pkt + 5, o.data, len);
    if (const sockaddr_in *a = PeerAddr(peer)) SendTo(*a, pkt, 5 + len);
}

static void RlResend(int peer, uint32_t now)
{
    RlStream &r = g_rl[peer];
    if (r.acked + 1 >= r.nextSeq || now - r.lastResend < 150) return;
    r.lastResend = now;
    const sockaddr_in *a = PeerAddr(peer);
    if (!a) return;
    int sent = 0;
    for (uint32_t s = r.acked + 1; s < r.nextSeq && sent < 32; s++, sent++) {
        RlOut &o = r.queue[s % RL_QUEUE];
        uint8_t pkt[8 + MAX_RELIABLE_PAYLOAD];
        pkt[0] = MSG_RELIABLE;
        memcpy(pkt + 1, &o.seq, 4);
        memcpy(pkt + 5, o.data, o.len);
        SendTo(*a, pkt, 5 + o.len);
    }
}

// Reception : n'accepte que le numero attendu (les autres seront renvoyes), acquitte toujours.
static void RlReceive(int peer, const uint8_t *buf, int len)
{
    if (len < 6) return;
    RlStream &r = g_rl[peer];
    uint32_t seq;
    memcpy(&seq, buf + 1, 4);
    if (g_cfg.logScripts && buf[5] == 4) Log("reseau : fiable n%u (attendu %u) type %d, %d octets", seq, r.expected, buf[5], len - 5);
    if (seq == r.expected) {
        r.expected++;
        if (g_onReliable) g_onReliable(peer, buf + 5, len - 5);
    }
    uint8_t ack[5] = { MSG_ACK };
    uint32_t a = r.expected - 1;
    memcpy(ack + 1, &a, 4);
    if (const sockaddr_in *addr = PeerAddr(peer)) SendTo(*addr, ack, 5);
}

static void RlAck(int peer, const uint8_t *buf, int len)
{
    if (len < 5) return;
    uint32_t a;
    memcpy(&a, buf + 1, 4);
    if (a > g_rl[peer].acked && a < g_rl[peer].nextSeq) g_rl[peer].acked = a;
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
    g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    u_long nb = 1;
    ioctlsocket(g_sock, FIONBIO, &nb);

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

static void HostReceive(const uint8_t *buf, int len, const sockaddr_in &from)
{
    int id = -1;
    for (int i = 1; i < MAX_PLAYERS; i++)
        if (g_players[i].connected && SameAddr(g_peerAddr[i], from)) id = i;

    if (buf[0] == MSG_HELLO && len >= (int)sizeof(MsgHello)) {
        const MsgHello *h = (const MsgHello *)buf;
        if (id < 0 && h->version == NET_VERSION)
            for (int i = 1; i < MAX_PLAYERS && id < 0; i++)
                if (!g_players[i].connected) {
                    id = i;
                    memset(&g_players[i], 0, sizeof(g_players[i]));
                    g_players[i].connected = true;
                    g_peerAddr[i] = from;
                    RlReset(g_rl[i]);
                    lstrcpynA(g_players[i].state.name, h->name, sizeof(g_players[i].state.name));
                    Log("reseau : %s entre (joueur %d)", g_players[i].state.name, i);
                    if (g_onJoin) g_onJoin(i);
                }
        if (id < 0) {
            uint8_t full = MSG_FULL;
            SendTo(from, &full, 1);
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
        g_players[id].state = s;
        for (int i = 1; i < MAX_PLAYERS; i++)   // relais aux autres invites
            if (i != id && g_players[i].connected) SendTo(g_peerAddr[i], &s, sizeof(s));
    } else if (buf[0] == MSG_RELIABLE) {
        RlReceive(id, buf, len);
    } else if (buf[0] == MSG_ACK) {
        RlAck(id, buf, len);
    } else if (HandleEntityMsg(buf, len)) {
        for (int i = 1; i < MAX_PLAYERS; i++)   // relais aux autres invites
            if (i != id && g_players[i].connected) SendTo(g_peerAddr[i], buf, len);
    } else if (buf[0] == MSG_BYE) {
        g_players[id].connected = false;
        Log("reseau : joueur %d parti", id);
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
    case MSG_ACK:
        RlAck(0, buf, len);
        break;
    case MSG_WELCOME:
        if (len >= (int)sizeof(MsgWelcome) && g_localId != ((const MsgWelcome *)buf)->id) {
            RlReset(g_rl[0]);
            g_localId = ((const MsgWelcome *)buf)->id;
            g_players[0].connected = true;
            Log("reseau : accepte par l'hote, joueur %d", g_localId);
        }
        break;
    case MSG_FULL:
        Log("reseau : l'hote refuse (partie pleine ou version differente)");
        break;
    case MSG_STATE:
        if (len >= (int)sizeof(MsgState)) {
            const MsgState *s = (const MsgState *)buf;
            if (s->id < MAX_PLAYERS && s->id != g_localId) {
                NetPlayer &p = g_players[s->id];
                if (!p.connected) Log("reseau : %s (joueur %d) est la", s->name, s->id);
                p.connected = true;
                p.state = *s;
                p.lastSeen = GetTickCount();
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
        if (len >= (int)sizeof(MsgPed) && g_onPed) g_onPed(*(const MsgPed *)buf);
        break;
    case MSG_MARKER:
        OnMarker(buf, len);
        break;
    case MSG_PED_REMOVE:
        if (len >= (int)sizeof(MsgPedRemove) && g_onPedRemove) g_onPedRemove(((const MsgPedRemove *)buf)->handle);
        break;
    case MSG_BYE:
        if (len >= (int)sizeof(MsgBye) && ((const MsgBye *)buf)->id < MAX_PLAYERS)
            g_players[((const MsgBye *)buf)->id].connected = false;
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
        if (g_cfg.host) HostReceive(buf, len, from);
        else GuestReceive(buf, len, from);
    }

    uint32_t now = GetTickCount();
    if (g_cfg.host) { for (int i = 1; i < MAX_PLAYERS; i++) if (g_players[i].connected) RlResend(i, now); }
    else if (g_localId >= 0) RlResend(0, now);
    if (!g_cfg.host && g_localId < 0 && now - g_lastHello > 1000) {   // on frappe a la porte chaque seconde
        MsgHello h = { MSG_HELLO, NET_VERSION };
        lstrcpynA(h.name, g_cfg.playerName, sizeof(h.name));
        SendTo(g_hostAddr, &h, sizeof(h));
        g_lastHello = now;
    }
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (i == g_localId || !g_players[i].connected) continue;
        if (now - g_players[i].lastSeen > TIMEOUT_MS) {
            g_players[i].connected = false;
            Log("reseau : joueur %d ne repond plus", i);
            if (!g_cfg.host && i == 0) g_localId = -1;   // l'hote a disparu : on refrappe
        }
    }
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

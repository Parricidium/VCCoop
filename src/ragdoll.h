// Corps mous des personnages (ragdoll.cpp).
#pragma once
#include <stdint.h>

void RagdollAfterProcess();                         // interp.cpp, apres CGame::Process
void RagdollOnMsg(const uint8_t *buf, int len);     // net.cpp : MSG_RAGDOLL
void RagdollReset();

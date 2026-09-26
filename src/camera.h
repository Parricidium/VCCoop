// Camera libre en vehicule et visee a la souris (camera.cpp).
#pragma once

void InstallCamera();
void CameraFrame();
bool FreeAimActive();      // clic droit en vehicule avec une arme de tir en voiture
extern float g_testMouseX; // autotest
extern bool g_testAim;

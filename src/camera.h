// Camera libre en vehicule et visee a la souris (camera.cpp).
#pragma once

void InstallCamera();
void CameraFrame();
bool FirstPersonActive();  // vue depuis la tete de Tommy (touche ToucheVue)
bool FreeAimActive();      // clic droit en vehicule avec une arme de tir en voiture
extern float g_testMouseX; // autotest
extern bool g_testAim;
void HookDirectInput(void *di);   // IDirectInput8A cree par le jeu : on lit la souris a la source
void MouseFocusFrame();   // reprise de la souris au retour du premier plan (Alt+Tab)

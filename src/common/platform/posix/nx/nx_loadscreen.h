#pragma once
/*
** nx_loadscreen.h
** Loading screen for the Nintendo Switch build.
**
** The engine does all of its heavy loading (reading archives, textures,
** compiling ZScript) long before the video system is up, which used to leave
** the Switch on a black screen for minutes with big mods. This draws a
** progress screen straight into the framebuffer from a helper thread until
** the real video output takes over.
**
** SPDX-License-Identifier: GPL-3.0-or-later
*/

// argv is inspected for -iwad / -file to show what is being loaded
void NXLS_Start(int argc, char **argv);
// releases the framebuffer; must be called before SDL creates its window
void NXLS_Stop();
bool NXLS_Active();
// true after this process registered buffers on the default NWindow.
// Stays true after NXLS_Stop so Vulkan can restore the scanout format.
bool NXLS_ClaimedDisplay();
// feed engine console output (used to track the startup stages)
void NXLS_OnPrint(const char *text);
// texture manager / startup window progress
void NXLS_Progress(int cur, int max);
// ZScript code generation progress
void NXLS_ScriptProgress(int cur, int max);

// optional CPU boost while loading (official "FastLoad" boost mode used by
// retail games on loading screens), enabled with -nxboost from the launcher.
void NXLS_SetBoost(bool on);

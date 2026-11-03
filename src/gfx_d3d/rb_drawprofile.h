#pragma once
#include "r_gfx.h"
#include "r_font.h"
#include <universal/profile.h>


struct alignas(8) DrawProfileGlobals // sizeof=0x81A8
{                                       // ...
    Font_s *font;                       // ...
    float fontWidth;                    // ...
    float fontHeight;                   // ...
    GfxColor textColor;                 // ...
    GfxColor labelColor;                // ...
    ProfileProbe log[5];                // ...
    int sortedProbeIndices[432];        // ...
    int lastSortTime;                   // ...
    ProfileReadableGlobal global[432];  // ...
    int devguiSelection;
    // padding byte
    // padding byte
    // padding byte
    // padding byte
};


void TRACK_rb_drawprofile();
void RB_AddProfileThread(int threadContext);
char RB_IsUsingAnyProfile();
void RB_DrawProfile();
void RB_DrawProfileHistory(const ProfileSettings *profSettings);
void RB_DrawProfileHistoryGraph(const ProfileReadable *read, int parity, int probeIndex, float x, float y);
void RB_DrawProfileRect(float x, float y, float width, float height, GfxColor color);
void RB_DrawProfileBar(float x, float y, float pixelsPerClock, uint32_t clockCount, GfxColor color);
void RB_DrawProfileHistoryLabel(int profEnum, float x, float y);
int RB_GetProfileHistoryProbeIndex(int historyIndex, const ProfileSettings *profSettings);
int RB_DrawProfileOverlay();
void RB_DrawAllProfileBackgrounds(const ProfileSettings *profSettings, int rowCount);
void RB_DrawProfileBackground(float y);
double RB_DrawProfileLabels();
void RB_DrawProfileString(int column, float y, const char *s, GfxColor textColor);
double RB_DrawProfileRow(int probeIndex, int indentation, float y);
int CompareSelfTimes(uint32_t *e0, uint32_t *e1);
int CompareTotalTimes(uint32_t *e0, uint32_t *e1);
int CompareAvgTotalTimes(int *e0, int *e1);
int CompareMaxTimes(uint32_t *e0, uint32_t *e1);
int CompareAvgSelfTimes(int *e0, int *e1);
int CompareMaxSelfTimes(uint32_t *e0, uint32_t *e1);
void RB_DrawProfileScript();
void RB_ProfileInit();

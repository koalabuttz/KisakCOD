#pragma once
#include "rb_backend.h"

void TRACK_rb_stats();
void R_TrackPrims(GfxCmdBufState *state, GfxPrimStatsTarget target);
void RB_TrackImmediatePrims(GfxPrimStatsTarget target);
void RB_EndTrackImmediatePrims();
void RB_TrackDrawPrimCall(int triCount);
int RB_Stats_TotalIndexCount();
int RB_Stats_TotalVertexCount();
int RB_Stats_TotalPrimCount();
int RB_Stats_ViewIndexCount(const GfxViewStats *viewStats);
void RB_Stats_f();
void RB_Stats_Display(const GfxFrameStats *frameStats);
void RB_Stats_Summarize(const char *label, const GfxViewStats *viewStats);
void RB_Stats_AccumulatePrimStats(const GfxPrimStats *primStats, GfxPrimStats *total);
void RB_Stats_SummarizePrimStats(const char *label, const GfxPrimStats *primStats);
void RB_Stats_UpdateMaxs(const GfxFrameStats *frameStatsCur, GfxFrameStats *frameStatsMax);
void RB_DrawPrimHistogramOverlay();


extern GfxPrimStats *backupPrimStats;
extern GfxViewStats *g_viewStats;
extern GfxPrimStats *g_primStats;
extern GfxFrameStats g_frameStatsCur;
#pragma once

struct UiContext;
// Shared production UI timing; callers retain their own context/history layout.
// Initialize the nonnegative sample index/history to zero for each owner.
// Call once per UI frame, not for each input event or paint operation.
void UI_UpdateContextTime(UiContext *dc,int *timeIndex,int previousTimes[4],int realtime);

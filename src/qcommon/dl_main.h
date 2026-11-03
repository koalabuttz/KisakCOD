#pragma once

enum dlStatus_t : __int32
{                                       // ...
    DL_CONTINUE = 0x0,
    DL_DONE = 0x1,
    DL_FAILED = 0x2,
};

void DL_CancelDownload();
void DL_InitDownload();
int DL_BeginDownload(char *localName, char *remoteName);
int DL_DownloadLoop();
bool DL_InProgress();
bool DL_DLIsMotd();

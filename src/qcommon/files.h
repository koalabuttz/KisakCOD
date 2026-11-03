#pragma once

enum FS_SERVER_COMPARE_RESULT : __int32
{                                       // ...
    FILES_MATCH = 0x0,
    NEED_DOWNLOAD = 0x1,
    NOT_DOWNLOADABLE = 0x2,
};

extern int fs_numServerReferencedFFs;
extern const char *fs_serverReferencedFFNames[32];

char *FS_GetMapBaseName(char *mapname);
int FS_iwIwd(char *iwd, char *base);
FS_SERVER_COMPARE_RESULT FS_CompareWithServerFiles(char *neededFiles, int len, int dlstring);
void FS_PureServerSetLoadedIwds(char *iwdSums, char *iwdNames);
void FS_ServerSetReferencedIwds(char *iwdSums, char *iwdNames);
void FS_ServerSetReferencedFFs(char *FFSums, char *FFNames);
void FS_ShutdownServerIwdNames();
void FS_ShutdownServerReferencedIwds();
void FS_ShutdownServerReferencedFFs();
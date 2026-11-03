#pragma once

#include "q_shared.h"

enum FsThread : __int32
{                                       // ...
    FS_THREAD_MAIN = 0x0,
    FS_THREAD_STREAM = 0x1,
    FS_THREAD_DATABASE = 0x2,
    FS_THREAD_BACKEND = 0x3,
    FS_THREAD_SERVER = 0x4,
    FS_THREAD_COUNT = 0x5,
    FS_THREAD_INVALID = 0x6,
};

enum fsMode_t : __int32
{                                       // ...
    FS_READ = 0x0,
    FS_WRITE = 0x1,
    FS_APPEND = 0x2,
    FS_APPEND_SYNC = 0x3,
};

struct directory_t // sizeof=0x200
{
    char path[256];
    char gamedir[256];
};
struct fileInIwd_s // sizeof=0xC
{
    uint32_t pos;
    char* name;
    fileInIwd_s* next;
};
struct iwd_t // sizeof=0x324
{
    char iwdFilename[256];
    char iwdBasename[256];
    char iwdGamename[256];
    uint8_t* handle;
    int checksum;
    int pure_checksum;
    volatile uint32_t hasOpenFile;
    int numfiles;
    uint8_t referenced;
    // padding byte
    // padding byte
    // padding byte
    uint32_t hashSize;
    fileInIwd_s** hashTable;
    fileInIwd_s* buildBuffer;
};
struct searchpath_s // sizeof=0x1C
{
    searchpath_s *next;
    iwd_t *iwd;
    directory_t *dir;
    int bLocalized;
    int ignore;
    int ignorePureCheck;
    int language;
};

union qfile_gus // sizeof=0x4
{                                       // ...
    FILE *o;
    unsigned char *z;
};
struct qfile_us // sizeof=0x8
{                                       // ...
    qfile_gus file;
    int iwdIsClone;
};
struct fileHandleData_t // sizeof=0x11C
{
    qfile_us handleFiles;
    int handleSync;
    int fileSize;
    int zipFilePos;
    iwd_t *zipFile;
    int streamed;
    char name[256];
};

bool FS_Initialized();

int FS_ConditionalRestart(int localClientNum, int checksumFeed);

char *FS_ReferencedIwdPureChecksums();
char *FS_ReferencedIwdNames();
char *FS_ReferencedIwdChecksums();
char *FS_LoadedIwdNames();
char *FS_LoadedIwdChecksums();
void FS_ClearIwdReferences();

char *FS_LoadedIwdPureChecksums();
void FS_CheckFileSystemStarted();
int FS_OpenFileOverwrite(char *qpath);
int FS_LoadStack();
int FS_HashFileName(const char *fname, int hashSize);
int FS_filelength(int f);
bool FS_IsFileInZip(int f);
void FS_ReplaceSeparators(char *path);
void FS_BuildOSPath(const char *base, const char *game, const char *qpath, char *ospath);
void FS_BuildOSPathForThread(const char *base, const char *game, const char *qpath, char *ospath, FsThread thread);
int FS_CreatePath(char *OSPath);
void FS_FCloseFile(int h);
void FS_FCloseLogFile(int h);
int FS_FOpenFileWrite(const char *filename);
int FS_FOpenTextFileWrite(const char *filename);
int FS_FOpenFileAppend(const char *filename);
uint32_t FS_FOpenFileReadStream(const char *filename, int *file);
uint32_t FS_FOpenFileReadForThread(const char *filename, int *file, FsThread thread);
int FS_FOpenFileReadDatabase(const char *filename, int *file);
uint32_t FS_FOpenFileRead(const char *filename, int *file);
bool FS_Delete(const char *filename);
int FS_FilenameCompare(const char *s1, const char *s2);
uint32_t FS_Read(uint8_t *buffer, uint32_t len, int h);
uint32_t FS_Write(const char *buffer, uint32_t len, int h);
uint32_t FS_WriteLog(const char *buffer, uint32_t len, int h);
void FS_Printf(int h, const char *fmt, ...);
int FS_Seek(int f, int offset, int origin);
int FS_ReadFile(const char *qpath, void **buffer);
uint32_t *FS_AllocMem(int bytes);
void FS_ResetFiles();
void FS_FreeFile(char *buffer);
void FS_FreeMem(char *buffer);
int FS_FileExists(char *file);
int FS_WriteFile(char *filename, char *buffer, uint32_t size);
void FS_ConvertPath(char *s);
void FS_InitFilesystem();
uint32_t FS_FOpenFileByMode(char *qpath, int *f, fsMode_t mode);
void FS_Flush(int f);
void FS_FreeFileList(const char **list);

void FS_CopyFile(char *fromOSPath, char *toOSPath);

void FS_Remove(const char *osPath);

char *FS_ShiftStr(const char *string, char shift);
int FS_SV_FOpenFileRead(const char *filename, int *fp);
int FS_SV_FOpenFileWrite(const char *filename);
void FS_SV_Rename(char *from, char *to);
int FS_SV_FileExists(char *file);

uint32_t FS_FTell(int f);
enum FsListBehavior_e : __int32
{                                       // ...
    FS_LIST_PURE_ONLY = 0x0,
    FS_LIST_ALL = 0x1,
};
int FS_GetFileList(
    const char *path,
    const char *extension,
    FsListBehavior_e behavior,
    char *listbuf,
    int bufsize);

const char **FS_ListFiles(const char *path, const char *extension, FsListBehavior_e behavior, int *numfiles);
const char **FS_ListFilesInLocation(
    const char *path,
    const char *extension,
    FsListBehavior_e behavior,
    int *numfiles,
    int lookInFlags);

void FS_FileClose(struct iobuf *stream);
int FS_FOpenFileWriteToDirForThread(const char *filename, const char *dir, FsThread thread);
int FS_FOpenFileWriteToDir(const char *filename, const char *dir);
int FS_WriteFileToDir(const char *filename, const char *path, char *buffer, uint32_t size);

void FS_Restart(int localClientNum, int checksumFeed);
bool FS_NeedRestart(int checksumFeed);

int FS_TouchFile(const char *name);

void FS_RegisterDvars();
void FS_Shutdown();
bool FS_DeleteInDir(char *filename, char *dir);
void FS_Rename(char *from, char *fromDir, char *to, char *toDir);

extern const dvar_t *fs_remotePCDirectory;
extern const dvar_t *fs_remotePCName;
extern const dvar_t *fs_homepath;
extern const dvar_s *fs_debug;
extern const dvar_s *fs_restrict;
extern const dvar_s *fs_ignoreLocalized;
extern const dvar_s *fs_basepath;
extern const dvar_s *fs_copyfiles;
extern const dvar_s *fs_cdpath;
extern const dvar_s *fs_gameDirVar;
extern const dvar_s *fs_basegame;

extern searchpath_s *fs_searchpaths;

extern int fs_numServerIwds;
extern int fs_serverIwds[1024];
extern int com_fileAccessed;
extern void *g_writeLogEvent;
extern int marker_com_files;
extern void *g_writeLogCompleteEvent;
extern int fs_loadStack;
extern const char *fs_serverIwdNames[1024];

extern int fs_checksumFeed;

extern char fs_gamedir[256];
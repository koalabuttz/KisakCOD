#pragma once
#include <cstdio>
#include <cstdint>

uint32_t FS_FileRead(void *ptr, uint32_t len, FILE *stream);
uint32_t FS_FileWrite(const void *ptr, uint32_t len, FILE *stream);
FILE *FS_FileOpenReadBinary(const char *filename);
FILE *FS_FileOpenReadText(const char *filename);
FILE *FS_FileOpenWriteBinary(const char *filename);
FILE *FS_FileOpenAppendText(const char *filename);
FILE *FS_FileOpenWriteText(const char *filename);
FILE *FS_FileOpenWriteReadBinary(const char *filename);
void FS_FileClose(FILE *stream);
int FS_FileSeek(FILE *file, int offset, int whence);
int FileWrapper_Seek(FILE *h, int offset, int origin);
int FS_FileGetFileSize(FILE *file);
int FileWrapper_GetFileSize(FILE *h);

uint32_t FS_FileTell(FILE *file);
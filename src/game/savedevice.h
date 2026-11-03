#pragma once

#ifndef KISAK_SP 
#error This file is for SinglePlayer only 
#endif

#ifdef KISAK_XBOX
static const char *CONSOLE_DEFAULT_SAVE_NAME = "savegame.svg";
#endif

void Memcard_InitializeSystem(void);
void SaveDevice_Init(void);
void SV_DisplaySaveErrorUI(void);
bool BuildCleanSavePath(char *, unsigned int, char const *, enum SaveType);
bool SaveDevice_IsAccessingDevice(void);
void WriteSaveToDeviceCleanup(void);
bool SaveDevice_IsSaveSuccessful(void);
int ReadFromDevice(void *, int, void *);
int OpenDevice(char const *, void **);
void CloseDevice(void *);
bool SaveExists(char const *);
int WriteSaveToDevice(unsigned char *, struct SaveHeader const *, bool);
void SV_ForceSelectSaveDevice_f(void);
void SV_SelectSaveDevice_f(void);
bool SaveDevice_CheckForError(struct MemcardError const *);
int WriteSaveToDeviceInternal(struct SaveHeader const *);
void BeginScreenUpdateIfSupported(void);
void EndScreenUpdateIfSupported(void);
void *SaveExists_OpenContextFile(char const *);
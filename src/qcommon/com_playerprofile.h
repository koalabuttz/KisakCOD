#pragma once


char Com_SetInitialPlayerProfile(int localClientNum);
int Com_BuildPlayerProfilePath(char *path, int pathSize, const char *format, ...);
void Com_InitPlayerProfiles(int localClientNum);
bool Com_HasPlayerProfile();
char Com_DeletePlayerProfile(const char *profileName);
char Com_NewPlayerProfile(const char *profileName);
bool Com_IsValidPlayerProfileDir(const char *profileName);
void Com_ChangePlayerProfile(int localClientNum, char *profileName);
void Com_CheckSetRecommended(int localClientNum);
void Com_SetRecommended(int localClientNum, int restart);

extern const dvar_t *ui_playerProfileAlreadyChosen;
extern const dvar_t *com_playerProfile;

extern const dvar_t *sys_configureGHz;
extern const dvar_t *sys_sysMB;
extern const dvar_t *sys_gpu;
extern const dvar_t *sys_configSum;
extern const dvar_t *sys_SSE;
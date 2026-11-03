#pragma once
#include <cstdint>

enum StatType : __int32
{                                       // ...
    STAT_TYPE_PRIMARY = 0x1,          // ...
    STAT_TYPE_SECONDARY = 0x2,          // ...
    STAT_TYPE_EQUIPMENT = 0x4,          // ...
    STAT_TYPE_WEAPON = 0x8,          // ...
    STAT_TYPE_ABILITY = 0x10,         // ...
    STAT_TYPE_GRENADE = 0x20,         // ...
};

struct StatsData // sizeof=0x2104
{                                       // ...
    char path[260];
    unsigned __int8 stats[8192];        // ...
};
struct StatsFile_s// sizeof=0x2114
{                                       // ...
    uint32_t hash[4];
    StatsData statsData;                // ...
};
struct StatsFile // sizeof=0x211C
{                                       // ...
    unsigned __int8 magic[4];
    uint32_t nonce;
    StatsFile_s body; // ...
};

struct playerStatNetworkData // sizeof=0x2002
{                                       // ...
    unsigned __int8 playerStats[8192];  // ...
    bool statsFetched;                  // ...
    bool statWriteNeeded;               // ...
};

struct CaCItem // sizeof=0xC
{                                       // ...
    int itemIndex;                      // ...
    int minLevel;
    StatType type;
};

void LiveStorage_ValidateCaCStat(int controllerIndex, int index, int value);
void LiveStorage_ValidateSetStatCmd(int index, int value);
void LiveStorage_StatsInit(int controllerIndex);
void LiveStorage_TrySetStat(int controllerIndex, int index, uint32_t value);
void LiveStorage_TrySetStatRange(int controllerIndex, int first, int last, uint32_t value);
void LiveStorage_WeaponPerkChallengeReset(int controllerIndex);
void LiveStorage_UnlockClassAssault(int controllerIndex);
void LiveStorage_UnlockClassDemolitions(int controllerIndex);
void LiveStorage_UnlockClassHeavyGunner(int controllerIndex);
void LiveStorage_UnlockClassSniper(int controllerIndex);
void LiveStorage_UnlockClassSpecOps(int controllerIndex);
void LiveStorage_SetFromLocString(int controllerIndex, const char *dvarName, char *preLocalizedText);
void LiveStorage_ReadStats();
void LiveStorage_ReadStatsFromDir(char *directory);
bool LiveStorage_DecryptAndCheck(StatsFile *statsFile, const char *statsDir);
void LiveStorage_GetCryptKey(uint32_t nonce, unsigned __int8 *outKey);
int LiveStorage_ChecksumGamerStats(unsigned __int8 *buffer, int len);
void LiveStorage_NoStatsFound();
void LiveStorage_WriteChecksumToBuffer(unsigned __int8 *buffer, int len);
bool LiveStorage_ReadStatsFile(const char *qpath, unsigned __int8 *buffer, uint32_t lenToRead);
void LiveStorage_HandleCorruptStats(char *filename);
playerStatNetworkData *LiveStorage_GetStatBuffer();
bool LiveStorage_DoWeHaveStats();
void LiveStorage_StatsWriteNeeded();
void LiveStorage_UploadStats();
void LiveStorage_Encrypt(StatsFile *statsFile);
int LiveStorage_GetStat(int __formal, int index);
void LiveStorage_SetStat(int __formal, int index, uint32_t value);
void LiveStorage_TrySetStatForCmd(int index, uint32_t value);
void LiveStorage_NewUser();
void LiveStorage_Init();
void LiveStorage_StatSetCmd();
void LiveStorage_StatGetCmd();
void LiveStorage_StatGetInDvarCmd();
void LiveStorage_UploadStatsCmd();
void LiveStorage_ReadStatsCmd();

#pragma once

#include "win_local.h"

void Sys_DetectVideoCard(int descLimit, char* description);
void Sys_GetPhysicalCpuCount(SysInfo* sysInfo);
void Sys_DetectCpuVendorAndName(char* vendor, char* name);
void Sys_SetAutoConfigureGHz(SysInfo* sysInfo);
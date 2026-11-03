#pragma once

#ifndef KISAK_SP 
#error This file is for SinglePlayer only 
#endif

struct ScriptFunctions;
struct pathnode_t;

int GScr_LoadScriptAndLabel(const char *filename, const char *label, ScriptFunctions *functions);
void GScr_LoadSingleAnimScript(const char *name, ScriptFunctions *functions);
void GScr_LoadAnimScripts(ScriptFunctions *functions);
void GScr_LoadDogAnimScripts(ScriptFunctions *functions);
void GScr_LoadLevelScript(const char *mapname, ScriptFunctions *functions);
void GScr_LoadScriptsForPathNode(pathnode_t *loadNode, void *data);
void GScr_LoadScriptsForPathNodes(ScriptFunctions *functions);
void GScr_LoadScriptsForEntities(ScriptFunctions *functions);
void GScr_LoadEntities();
void GScr_LoadScripts(const char *mapname, ScriptFunctions *functions);

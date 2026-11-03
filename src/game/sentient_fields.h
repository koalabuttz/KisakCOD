#pragma once
#include "sentient.h"
#include "game_public.h"

#ifndef KISAK_SP 
#error This file is for SinglePlayer only 
#endif

struct sentient_fields_s
{
	const char *name;
	int ofs;
	fieldtype_t type;
	void(*setter)(sentient_s *, const sentient_fields_s *);
	void(*getter)(sentient_s *, const sentient_fields_s *);
};

void SentientScr_ReadOnly(sentient_s *pSelf, const sentient_fields_s *pField);
void SentientScr_SetTeam(sentient_s *pSelf, const sentient_fields_s *pField);
void SentientScr_GetTeam(sentient_s *pSelf, const sentient_fields_s *pField);
void GScr_AddFieldsForSentient();
void Scr_SetSentientField(sentient_s *sentient, unsigned int offset);
void Scr_GetSentientField(sentient_s *sentient, unsigned int offset);

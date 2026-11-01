#pragma once
struct ScreenPlacement;
struct CachedAssets_t;
struct Font_s;
struct dvar_s;
Font_s *UI_SelectFont(const ScreenPlacement *,int,float,const CachedAssets_t &,const dvar_s *small,const dvar_s *big,const dvar_s *extraBig);

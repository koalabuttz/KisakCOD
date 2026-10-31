#include <linux/client/diagnostic_host.h>
#include <qcommon/qcommon.h>
#include <universal/com_memory.h>
#include <script/scr_stringlist.h>
#include <ui/ui_shared.h>
#include <ui/ui_registration.h>
#include <cstdio>
#include <cstring>
extern scrStringDebugGlob_t* scrStringDebugGlob;
static int checks, failures;
static void Check(bool ok,const char* what){++checks;failures+=!ok;std::printf("%s: %s\n",ok?"PASS":"FAIL",what);}
int main()
{
 for(int cycle=0;cycle<3;++cycle) {
  {
   kisak::client::Runtime runtime;
   const auto baseline=scrStringDebugGlob->totalRefCount;
   const char *a=CopyString("shared owned text"),*b=CopyString("shared owned text");
   Check(a==b && scrStringDebugGlob->totalRefCount==baseline+2,"identical text shares storage with two owned references");
   FreeString(a);Check(!std::strcmp(b,"shared owned text"),"one release preserves other owner's string");
   ReplaceString(&b,b);Check(scrStringDebugGlob->totalRefCount==baseline+1,"self-alias replacement balances references");
   FreeString(b);const char *nullable=nullptr;ReplaceString(&nullable,"new value");FreeString(nullable);
   Check(scrStringDebugGlob->totalRefCount==baseline,"null-old replacement returns to baseline");
   const auto first=Dvar_RegisterString("same_name_value","same_name_value",DVAR_EXTERNAL,"");
   const auto second=Dvar_RegisterString("independent_owner","same_name_value",DVAR_EXTERNAL,"");
   Check(first->name==first->current.string && first->current.string==second->current.string,"name/value and separate dvars share interned storage");
   Check(first->current.string==first->latched.string && first->current.string==first->reset.string,"current latched reset are weak aliases");
   Dvar_SetString(first,"changed");
   Check(!std::strcmp(second->current.string,"same_name_value") && !std::strcmp(first->reset.string,"same_name_value"),"changing one dvar preserves reset and independent owner");
   Dvar_Reset(first,DVAR_SOURCE_INTERNAL);
   Check(first->current.string==first->reset.string && first->latched.string==first->reset.string,"reset reunites weak aliases");
   const auto latched=Dvar_RegisterString("latched_owner","start",DVAR_EXTERNAL|DVAR_LATCH,"");
   Dvar_SetStringFromSource(const_cast<dvar_s*>(latched),const_cast<char*>("later"),DVAR_SOURCE_EXTERNAL);
   Check(!std::strcmp(latched->current.string,"start") && !std::strcmp(latched->latched.string,"later"),"latched assignment owns distinct pending text");
   Dvar_RegisterString("latched_owner","start",DVAR_EXTERNAL|DVAR_LATCH,"");
   Check(latched->current.string==latched->latched.string && !std::strcmp(latched->current.string,"later"),"reregistration applies pending text");
   UiContext context{};menuDef_t menu{};menu.window.name="owned_menu";menuDef_t* menus[]={&menu};
   const auto beforeUi=scrStringDebugGlob->totalRefCount;
   Check(UI_ReplaceOwnedMenus(&context,menus,1)==UiOwnedMenuRegistration::ok,"real owned menu registration");
   auto *local=reinterpret_cast<UILocalVar*>(UILocalVar_FindOrCreate(&context.localVars,const_cast<char*>("ui_owned")));
   UILocalVar_SetString(local,const_cast<char*>("same_name_value"));
   Check(local->u.string==first->current.string,"UI local shares dvar string storage");
   UILocalVar_SetString(local,const_cast<char*>(local->u.string));
   Check(scrStringDebugGlob->totalRefCount==beforeUi+2,"UI self alias retains name and value references");
   Check(UI_ReplaceOwnedMenus(&context,context.Menus,context.menuCount)==UiOwnedMenuRegistration::ok && scrStringDebugGlob->totalRefCount==beforeUi,"reload releases locals before registry replacement");
   Check(UILocalVar_Find(&context.localVars,"ui_owned")==nullptr,"reload clears local ownership");
   UI_ShutdownOwnedMenuContext(&context);Check(context.menuCount==0 && context.Menus[0]==nullptr,"shutdown clears borrowed menu references");
  }
  Check(scrStringDebugGlob==nullptr,"Runtime completes zero-reference SL shutdown");
 }
 std::printf("string consumers: %d checks, %d failures\n",checks,failures);return failures?1:0;
}

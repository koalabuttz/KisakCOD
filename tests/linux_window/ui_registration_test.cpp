#include <ui/ui_registration.h>
#include <ui/ui_shared.h>
#include <universal/com_memory.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <array>
namespace {
int checks,failures,allocations;
bool failCopy;
struct Allocation {char *pointer{};bool live{};};
std::array<Allocation,128> tracked;
size_t used;
bool Live(const char *p){for(size_t n=0;n<used;++n)if(tracked[n].pointer==p)return tracked[n].live;return false;}
UiContext *observed;
void Check(bool ok,const char *name){++checks;failures+=!ok;std::printf("%s: %s\n",ok?"PASS":"FAIL",name);}
void Keywords(){Check(observed->menuCount==0&&observed->openMenuCount==0,"shared setup resets counts before game keyword callback");}
void ThrowKeywords(){throw std::runtime_error("fixture keyword failure");}
}
void MyAssertHandler(const char*,int,int,const char*,...){throw std::runtime_error("engine assertion");}
const char *CopyString(const char *s){
 if(failCopy){failCopy=false;throw std::bad_alloc();}
 // Quarantine released storage: flag old-code alias reads without executing UB.
 for(size_t n=0;n<used;++n)if(tracked[n].pointer==s&&!tracked[n].live)throw std::logic_error("copy after release");
 if(used==tracked.size())throw std::logic_error("fixture capacity");
 auto *p=static_cast<char*>(std::malloc(std::strlen(s)+1));if(!p)throw std::bad_alloc();
 tracked[used++]={p,true};++allocations;return std::strcpy(p,s);
}
void FreeString(const char *s){if(!s)return;for(size_t n=0;n<used;++n)if(tracked[n].pointer==s){if(!tracked[n].live)throw std::logic_error("duplicate release");tracked[n].live=false;--allocations;return;}throw std::logic_error("unowned release");}
int main(){
 UiContext dc{};observed=&dc;dc.menuCount=3;dc.openMenuCount=2;UI_SetupMenuContext(&dc,Keywords);
 Check(!dc.menuCount&&!dc.openMenuCount,"setup publishes empty registration without claiming full UI startup");
 menuDef_t a{},b{},sameName{};a.window.name="a";b.window.name="b";sameName.window.name="a";
 menuDef_t *initial[]={&a,&b};
 Check(UI_ReplaceOwnedMenus(&dc,initial,2)==UiOwnedMenuRegistration::ok&&dc.Menus[0]==&a&&dc.Menus[1]==&b,"owned references retain source order and identity");
 char key[]="owned",value[]="value";auto *var=UILocalVar_FindOrCreate(&dc.localVars,key);UILocalVar_SetString(var->table,value);
 Check(allocations==2,"actual localvar owns name and string allocations");
 auto reject=[&](menuDef_t **list,int count,UiOwnedMenuRegistration expected){auto before=dc;auto status=UI_ReplaceOwnedMenus(&dc,list,count);Check(status==expected&&!std::memcmp(&before,&dc,sizeof(dc))&&allocations==2,"invalid replacement preserves registry, stack and owned localvars");};
 menuDef_t *duplicate[]={&a,&a};reject(duplicate,2,UiOwnedMenuRegistration::duplicate);
 menuDef_t *names[]={&a,&sameName};reject(names,2,UiOwnedMenuRegistration::duplicate);
 sameName.window.name="A";reject(names,2,UiOwnedMenuRegistration::duplicate);
 std::array<char,256> unterminated;unterminated.fill('x');sameName.window.name=unterminated.data();reject(names,2,UiOwnedMenuRegistration::invalidMenu);
 menuDef_t *nullItem[]={&a,nullptr};reject(nullItem,2,UiOwnedMenuRegistration::invalidMenu);
 reject(nullptr,1,UiOwnedMenuRegistration::invalidMenu);reject(initial,641,UiOwnedMenuRegistration::capacity);reject(initial,-1,UiOwnedMenuRegistration::capacity);
 dc.menuStack[0]=&a;dc.openMenuCount=1;reject(initial,2,UiOwnedMenuRegistration::activeMenus);dc.openMenuCount=0;
 Check(UI_ReplaceOwnedMenus(&dc,dc.Menus,2)==UiOwnedMenuRegistration::ok&&dc.Menus[1]==&b&&!dc.menuStack[0]&&allocations==0,"aliasing reload snapshots references and frees actual localvars and stale stack");
 Check(UI_AppendMenuReference(&dc,&a)&&dc.menuCount==3,"shared legacy append intentionally permits duplicate references");
 Check(!UI_AppendMenuReference(nullptr,&a)&&!UI_AppendMenuReference(&dc,nullptr)&&dc.menuCount==3,"invalid append cannot mutate registration");
 dc.menuCount=640;Check(!UI_AppendMenuReference(&dc,&b)&&dc.menuCount==640,"shared append refuses capacity overflow");
 dc.menuCount=-1;Check(!UI_AppendMenuReference(&dc,&b)&&UI_ReplaceOwnedMenus(&dc,initial,2)==UiOwnedMenuRegistration::invalidContext,"negative registry rejected before indexing");dc.menuCount=0;
 std::array<menuDef_t,640> menus{};std::array<std::array<char,16>,640> namesStorage{};std::array<menuDef_t*,640> refs{};
 for(int n=0;n<640;++n){std::snprintf(namesStorage[n].data(),16,"menu%d",n);menus[n].window.name=namesStorage[n].data();refs[n]=&menus[n];}
 Check(UI_ReplaceOwnedMenus(&dc,refs.data(),640)==UiOwnedMenuRegistration::ok&&dc.menuCount==640&&dc.Menus[639]==refs[639],"full supported registry capacity is usable");
 UI_ShutdownOwnedMenuContext(&dc);UI_ShutdownOwnedMenuContext(&dc);Check(!dc.menuCount&&!dc.Menus[639]&&!dc.openMenuCount&&allocations==0,"repeat shutdown clears borrowed pointers without freeing menus");
 for(int n=0;n<3;++n){UI_ReplaceOwnedMenus(&dc,initial,2);UILocalVar_SetString(UILocalVar_FindOrCreate(&dc.localVars,key)->table,value);UI_ShutdownOwnedMenuContext(&dc);}
 Check(allocations==0&&!dc.Menus[0],"same-context recreation releases actual localvars each cycle");
 try{UI_SetupMenuContext(&dc,ThrowKeywords);Check(false,"keyword error propagated");}catch(const std::runtime_error&){Check(!dc.menuCount&&!dc.openMenuCount,"keyword error propagates without claiming registration");}
 // Real string mutation failure must preserve values and remain safe to shut down.
 auto *stringVar=UILocalVar_FindOrCreate(&dc.localVars,key)->table;
 UILocalVar_SetString(stringVar,value);const char *old=stringVar->u.string;
 failCopy=true;bool threw=false;try{UILocalVar_SetString(stringVar,key);}catch(const std::bad_alloc&){threw=true;}
 Check(threw&&stringVar->type==UILOCALVAR_STRING&&stringVar->u.string==old&&Live(old),"failed replacement preserves the existing live string");
 bool cleanup=true;try{UI_ShutdownOwnedMenuContext(&dc);}catch(const std::logic_error&){cleanup=false;dc.localVars={};}
 Check(cleanup&&allocations==0,"shutdown after failed replacement performs no duplicate release");
 UILocalVar scalar{};scalar.type=UILOCALVAR_INT;scalar.u.integer=73;failCopy=true;threw=false;
 try{UILocalVar_SetString(&scalar,value);}catch(const std::bad_alloc&){threw=true;}
 Check(threw&&scalar.type==UILOCALVAR_INT&&scalar.u.integer==73,"failed string conversion preserves prior scalar type and value");
 UILocalVar alias{};UILocalVar_SetString(&alias,value);old=alias.u.string;bool aliasOk=true;
 try{UILocalVar_SetString(&alias,const_cast<char*>(alias.u.string));}catch(const std::logic_error&){aliasOk=false;}
 Check(aliasOk&&Live(alias.u.string)&&!std::strcmp(alias.u.string,value)&&!Live(old),"self-alias string assignment copies before releasing prior storage");
 if(Live(alias.u.string))FreeString(alias.u.string);
 Check(allocations==0,"string failure and alias fixtures leave no live owned allocation");
 for(size_t n=0;n<used;++n)std::free(tracked[n].pointer);
 std::printf("-- %d checks, %d failures --\n",checks,failures);return failures?1:0;
}

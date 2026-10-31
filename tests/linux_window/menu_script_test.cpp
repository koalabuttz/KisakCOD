// Real menu parser, handlers, local variables and close lifecycle. Audio here is
// an explicit alias-recording callback; this test does not claim audio playback.
#include <linux/client/window/menu_script_host.h>
#include <linux/client/diagnostic_host.h>
#include <ui/ui_script_core.h>
#include <ui/ui_input_core.h>
#include <universal/q_parse.h>
#include <cstring>
#include <cstdio>
#include <string>
#include <thread>

using kisak::window::MenuScriptHost;
namespace {
int checks,failures,plays;std::string lastAlias;
void Check(bool ok,const char *name) { ++checks;failures+=!ok;std::printf("%s: %s\n",ok?"PASS":"FAIL",name); }
template<class F> bool Reject(F action) {try { action();return false; }catch(const std::exception&) {return true;}}
int Play(void*,const char *alias) { ++plays;lastAlias=alias;return plays; }
int FailedPlay(void*,const char*) { return 0; }
int Value(UiContext &dc,const char *name) { auto *var=UILocalVar_Find(&dc.localVars,name);return var?var->table[0].u.integer:-999; }
void Run(UiContext &dc,itemDef_s &item,const char *script) { Item_RunScript(&dc,&item,const_cast<char*>(script)); }
}
int main() {
 try {
  kisak::client::Runtime runtime;
  UiContext dc{};menuDef_t menu{};itemDef_s item{};
  menu.window.name="options_control";item.parent=&menu;
  {
   MenuScriptHost host(nullptr,Play);
   Check(Reject([&]{MenuScriptHost other(nullptr,Play);}),"one scoped script provider enforced");
   const auto valid=host.RegisterScript("setLocalVarBool ui_hideBack 1 ; setLocalVarInt ui_highlight 31 ; setLocalVarString ui_choicegroup \"\" ; play mouse_click ;");
   Run(dc,item,valid);
   Check(Value(dc,"ui_hideBack")==1 && Value(dc,"ui_highlight")==31 && plays==1 && lastAlias=="mouse_click","real parser dispatch mutates approved local variables and invokes audio callback");
   auto *choice=UILocalVar_Find(&dc.localVars,"ui_choicegroup");
   Check(choice && choice->table[0].type==UILOCALVAR_STRING && choice->table[0].u.string && !*choice->table[0].u.string,"actual string handler stores owned empty value");
   const int before=plays;
   for(const char *bad:{"exec quit ;","setLocalVarBool ui_hideBack 0 ; exec quit ;","close main ;","play arbitrary ;","setLocalVarBool ui_hideBack 2 ;","setLocalVarInt ui_highlight 31junk ;","setLocalVarInt ui_highlight 33 ;","setLocalVarString ui_choicegroup x ;","setLocalVarInt other 31 ;","close ;","play \"mouse_click","play @mouse_click ;","play mouse_click // hidden","play mouse_click\n;"}) {
    Check(Reject([&]{host.RegisterScript(bad);}) && Value(dc,"ui_hideBack")==1 && plays==before,"unsupported complete script rejected without prefix execution");
   }
   Check(Reject([&]{host.RegisterScript(std::string("play mouse_click ;\0exec quit",27));}),"embedded NUL rejected before parser truncation");
   Check(Reject([&]{host.RegisterScript(std::string(5120,' '));}),"oversized script rejected before production copy truncation");
   const char external[]="setLocalVarBool ui_hideBack 0 ;";
   Check(Reject([&]{Run(dc,item,external);}) && Value(dc,"ui_hideBack")==1,"unregistered pointer rejected even when bytes are individually allowed");
   const auto mutableScript=host.RegisterScript("setLocalVarBool ui_hideBack 0 ; play mouse_click ;");
   const auto pos=std::string(mutableScript).find("mouse_click");const_cast<char*>(mutableScript)[pos]='x';
   Check(Reject([&]{Run(dc,item,mutableScript);}) && Value(dc,"ui_hideBack")==1 && plays==before,"mutated unsupported tail rejected before valid prefix changes state");
   const auto clear=host.RegisterScript("setLocalVarBool ui_hideBack 0 ;");
   for(int i=0;i<40;++i)host.RegisterScript(";");
   Run(dc,item,clear);Check(Value(dc,"ui_hideBack")==0,"registered script pointer remains stable across new registrations");
   const auto missing=host.RegisterScript("close options_sound ;");Run(dc,item,missing);
   Check(dc.openMenuCount==0,"approved absent menu name retains real close no-op");
   menu.window.dynamicFlags[0]=6;dc.Menus[0]=&menu;dc.menuCount=1;dc.menuStack[0]=&menu;dc.openMenuCount=1;
   menu.onClose=host.RegisterScript("setLocalVarInt ui_highlight 32 ;");
   const auto close=host.RegisterScript("close self ;");Run(dc,item,close);
   Check(dc.openMenuCount==0 && !(menu.window.dynamicFlags[0]&6) && Value(dc,"ui_highlight")==32,"real close lifecycle executes registered onClose then removes menu state");
   menu.window.dynamicFlags[0]=6;dc.menuStack[0]=&menu;dc.openMenuCount=1;menu.onClose=close;
   Check(Reject([&]{Run(dc,item,close);}),"recursive close script bounded before parse stack overflow");
   menu.onClose=nullptr;Run(dc,item,close);Check(dc.openMenuCount==0,"rejected recursion restores dispatch and parser scopes");
   bool wrongThread=false;std::thread other([&]{wrongThread=Reject([&]{host.RegisterScript(";");});});other.join();
   Check(wrongThread,"script admission rejects foreign thread");
   Run(dc,item,nullptr);Run(dc,item,"");Item_RunScript(&dc,nullptr,const_cast<char*>(valid));
   Check(plays==before,"null and empty invocations retain production no-op behavior");
   UILocalVar_Shutdown(&dc.localVars);
  }
  Check(Reject([&]{MenuScriptHost::Current();}),"provider unavailable after owner teardown");
  {
   MenuScriptHost host(nullptr,nullptr);const auto script=host.RegisterScript("setLocalVarBool ui_hideBack 1 ; play mouse_over ;");
   Check(Reject([&]{Run(dc,item,script);}) && !UILocalVar_Find(&dc.localVars,"ui_hideBack"),"missing audio provider rejects entire script before local-variable prefix");
  }
  {
   MenuScriptHost host(nullptr,FailedPlay);const auto script=host.RegisterScript("play mouse_over ;");
   Check(Reject([&]{Run(dc,item,script);}),"audio callback failure is explicit");
  }
 }catch(const std::exception&e) {std::fprintf(stderr,"menu script test failed: %s\n",e.what());++failures;}
 std::printf("-- %d checks, %d failures --\n",checks,failures);return failures?1:0;
}

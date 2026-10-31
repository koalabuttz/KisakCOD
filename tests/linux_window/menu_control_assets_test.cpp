#include "linux/client/window/menu_control_assets.h"
#include <vector>
#include <cstring>
#include <cstdio>
#include <stdexcept>
using namespace kisak::window;
namespace {
int checks=0,failures=0;
void Check(bool ok,const char *name){++checks;if(!ok){++failures;std::printf("FAIL %s\n",name);}}
void W(std::vector<uint8_t>&b,uint32_t v){for(int n=0;n<4;++n)b.push_back(v>>(n*8));}
void F(std::vector<uint8_t>&b,float f){uint32_t v;std::memcpy(&v,&f,4);W(b,v);}
void S(std::vector<uint8_t>&b,const char*s){W(b,std::strlen(s));b.insert(b.end(),s,s+std::strlen(s));}
void R(std::vector<uint8_t>&b){for(int n=0;n<4;++n)F(b,0);W(b,0);W(b,0);}
void Window(std::vector<uint8_t>&b,int style,int flags,int dynamic){R(b);R(b);W(b,style);W(b,0);F(b,1);W(b,0);W(b,0);W(b,flags);W(b,dynamic);for(int n=0;n<8;++n)F(b,0);}
std::vector<uint8_t> Fixture(){std::vector<uint8_t>b{'K','U','C','T','R','L','0','1'};W(b,4);S(b,"options_look");Window(b,0,0,0);W(b,0);W(b,0);W(b,1);for(int n=0;n<12;++n)F(b,0);for(int n=0;n<3;++n)S(b,"");
 const char*keys[]={"EXE_YES","EXE_NO","MENU_FREE_LOOK","MENU_SMOOTH_MOUSE"};for(auto key:keys){S(b,key);S(b,"synthetic");}
 const int indices[]={105,106,111,112};for(int k=0;k<4;++k){bool c=k%2;W(b,indices[k]);Window(b,c,c?0:1048576,65540);R(b);W(b,c?11:1);W(b,c?11:1);W(b,0);W(b,c?0:1);W(b,c?8:10);F(b,0);F(b,0);F(b,.4f);W(b,c?3:6);W(b,0);W(b,0);S(b,c?(k==1?"cl_freelook":"m_filter"):"");for(int n=0;n<3;++n)S(b,"");W(b,c?0:2);if(!c){W(b,0);W(b,16);W(b,1);W(b,2);S(b,k==0?"@MENU_FREE_LOOK":"@MENU_SMOOTH_MOUSE");}}
 return b;}
bool Rejected(const std::vector<uint8_t>&b){try{auto a=DecodeMenuControl(b);return false;}catch(const std::invalid_argument&){return true;}}
void Replace(std::vector<uint8_t>&b,const char*from,const char*to){for(size_t i=0;i+std::strlen(from)<=b.size();++i)if(!std::memcmp(b.data()+i,from,std::strlen(from))){std::memcpy(b.data()+i,to,std::strlen(from));return;}throw std::runtime_error("fixture string not found");}
}
int main(int argc,char**argv){auto b=Fixture();auto a=DecodeMenuControl(b);Check(a->menu.items[1]==&a->items[1]&&a->items[1].parent==&a->menu,"stable engine pointers");Check(a->items[0].textExp.entries[1]->data.operand.internals.string==a->itemStrings[0][4].c_str(),"owned expression string");Check(!std::strcmp(a->Localize("@EXE_YES"),"synthetic")&&!a->Localize("UNKNOWN"),"bounded localization lookup");bool truncated=true;for(size_t n=0;n<b.size();++n){std::vector<uint8_t>t(b.begin(),b.begin()+n);if(!Rejected(t)){truncated=false;break;}}Check(truncated,"every truncated byte span rejected");auto t=b;t.push_back(0);Check(Rejected(t),"trailing byte rejected");t=b;t[8]=5;Check(Rejected(t),"unexpected count rejected");t=b;Replace(t,"cl_freelook","xx_freelook");Check(Rejected(t),"unadmitted dvar rejected");t=b;Replace(t,"@MENU_FREE_LOOK","@MENU_FAKE_LOOK");Check(Rejected(t),"unadmitted expression rejected");t=b;t[28]=0;t[29]=0;t[30]=128;t[31]=127;Check(Rejected(t),"nonfinite rectangle rejected");t=b;t.resize(32769);Check(Rejected(t),"bounded total size");Check(MenuControlScriptSupported("play mouse_click ; ; setLocalVarInt ui_highlight 31 ;"),"supported script grammar");Check(!MenuControlScriptSupported("exec dangerous ;")&&!MenuControlScriptSupported("play mouse_click; close unknown;"),"unsupported command rejected");Check(!MenuControlScriptSupported("play \"mouse_click\"junk;")&&!MenuControlScriptSupported("play \"mouse_click"),"malformed script rejected");Check(!MenuControlScriptSupported("setLocalVarInt ui_highlight 33;")&&!MenuControlScriptSupported("setLocalVarString ui_choicegroup arbitrary;"),"local variable domain enforced");if(argc==2){auto genuine=LoadMenuControlAsset(argv[1]);Check(genuine->menu.blurRadius==7&&genuine->sourceIndices[3]==112&&genuine->items[1].action&&genuine->menu.onOpen,"private genuine profile loaded");}
 std::printf("-- %d checks, %d failures --\n",checks,failures);return failures?1:0;}

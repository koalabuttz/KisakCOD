#include <linux/client/window/menu_composition_assets.h>
#include <cstdio>
#include <cstring>
#include <stdexcept>
using namespace kisak::window;
namespace {
int checks=0,failures=0;
void Check(bool ok,const char *label){++checks;failures+=!ok;std::printf("%s: %s\n",ok?"PASS":"FAIL",label);}
void Word(std::vector<uint8_t> &v,unsigned n){for(unsigned i=0;i<4;++i)v.push_back(n>>(8*i));}
void Float(std::vector<uint8_t> &v,float f){unsigned n;std::memcpy(&n,&f,4);Word(v,n);}
void Name(std::vector<uint8_t> &v,const char *s){Word(v,std::strlen(s));v.insert(v.end(),s,s+std::strlen(s));}
bool Reject(const std::vector<uint8_t>&v){try{DecodeMenuComposition(v);return false;}catch(const std::invalid_argument&){return true;}}
}
int main(){try{
 std::vector<uint8_t> bytes={'K','U','C','O','M','P','0','2'};Word(bytes,8);Word(bytes,1);Float(bytes,1);Float(bytes,.1f);Float(bytes,0);
 std::vector<size_t> records;
 for(unsigned i=0;i<8;++i){records.push_back(bytes.size());Word(bytes,i);for(int n=0;n<4;++n)Word(bytes,0);Word(bytes,i==0);for(float f:{0.f,0.f,i==5?-16.f:16.f,i==2?-64.f:64.f})Float(bytes,f);Word(bytes,0);Word(bytes,0);Word(bytes,3);Word(bytes,0);Float(bytes,1);Word(bytes,1048576);Word(bytes,65540);for(float f:{1.f,1.f,1.f,1.f,0.f,0.f,0.f,0.f})Float(bytes,f);Name(bytes,i==2?"button_highlight_end":"white");}Name(bytes,"ui_cursor");
 auto assets=DecodeMenuComposition(bytes);Check(assets.items.size()==8&&assets.items[7]->sourceIndex==7,"actual parser preserves canonical source order");
 auto *record=assets.items[0].get();auto *statement=record->item.materialExp.entries;Check(record->item.materialExp.numEntries==2&&statement[0]->type==0&&int(statement[0]->data.op)==16&&statement[1]->type==1&&statement[1]->data.operand.dataType==VAL_STRING&&!std::strcmp(statement[1]->data.operand.internals.string,"white"),"literal expression preserves real operator and owned operand");
 Check(assets.items[1]->item.materialExp.numEntries==0&&assets.items[1]->item.text==nullptr&&assets.items[1]->item.type==0,"static decoration remains null-text type0 with no invented expression");
 Check(assets.items[2]->item.window.rect.h==-64&&assets.items[5]->item.window.rect.w==-16,"signed mirrored geometry preserved for actual draw helper");
 auto moved=std::move(assets);Check(moved.items[0].get()==record&&moved.items[0]->item.materialExp.entries==statement&&!std::strcmp(statement[1]->data.operand.internals.string,"white"),"moving asset aggregate preserves item and expression addresses");
 for(size_t offset:{size_t(60),size_t(64)}){auto flags=bytes;flags[records[1]+offset]^=1;Check(Reject(flags),"unsupported initial decoration visibility/static flags reject");}
 auto old=bytes;old[7]='1';Check(Reject(old),"flattened old schema rejected explicitly");
 bool truncated=true;for(size_t n=0;n<bytes.size();++n)truncated&=Reject({bytes.begin(),bytes.begin()+n});Check(truncated,"every truncated prefix rejects without out-of-bounds traversal");
 auto bad=bytes;bad.push_back(0);Check(Reject(bad),"trailing metadata rejected");
 bool fields=true;for(size_t word=0;word<6;++word){bad=bytes;bad[records[1]+word*4]^=1;fields&=Reject(bad);}Check(fields,"source type dataType alignment flags and unexpected expression reject");
 bad=bytes;bad[records[0]+20]=0;Check(Reject(bad),"missing genuine backing expression rejected");
 bytes.clear();Check(!std::strcmp(statement[1]->data.operand.internals.string,"white"),"expression owns literal after serialized input destruction");
}catch(const std::exception &e){++failures;std::fprintf(stderr,"composition metadata fixture: %s\n",e.what());}
 std::printf("-- %d checks, %d failures --\n",checks,failures);return failures?1:0;
}

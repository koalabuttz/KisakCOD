#pragma once
#include <cstdint>
#include <list>
#include <string>
#include <thread>

struct UiContext;
struct itemDef_s;
namespace kisak::window {
// Owns admitted script bytes for one main-thread menu lifecycle. Item/menu
// objects referencing these strings must be destroyed before this provider.
class MenuScriptHost {
public:
    using Play = int(*)(void *,const char *);
    MenuScriptHost(void *context,Play play);
    ~MenuScriptHost();
    MenuScriptHost(const MenuScriptHost&)=delete;
    MenuScriptHost& operator=(const MenuScriptHost&)=delete;
    MenuScriptHost(MenuScriptHost&&)=delete;
    MenuScriptHost& operator=(MenuScriptHost&&)=delete;
    const char *RegisterScript(std::string script);
    void Run(UiContext *,itemDef_s *,const char *script);
    int PlayAlias(uint32_t client,const char *alias);
    static MenuScriptHost &Current();
private:
    void CheckThread() const;
    bool Validate(const std::string &script) const;
    void *context_;
    Play play_;
    std::thread::id thread_;
    std::list<std::string> scripts_;
    unsigned depth_=0;
};
}

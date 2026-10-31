#pragma once
#include "cpu_renderer.h"
#include <ui/ui_shared.h>
#include <string>
#include <vector>
namespace kisak::window {
struct MenuCompositionWindow {windowDef_t window{};unsigned sourceIndex=0;CpuRenderer::UiMaterialHandle material=0;};
struct MenuCompositionAssets {
 std::vector<MenuCompositionWindow> windows;
 CpuRenderer::UiMaterialHandle cursor=0;
 int fadeCycle=0;float fadeClamp=0,fadeAmount=0,fadeInAmount=0;
};
// Successful resources belong to the renderer generation; failures release all
// resources created by this call. No game assets are embedded in this loader.
MenuCompositionAssets LoadMenuComposition(CpuRenderer &,const std::string &bundle,const std::string &images);
}

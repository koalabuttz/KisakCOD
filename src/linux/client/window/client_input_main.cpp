#include "window_host.h"
#include "client_input_host.h"
#include "shutdown_signal.h"
#include "frame_deadline.h"
#include "cpu_renderer.h"
#include "font_assets.h"
#include "menu_assets.h"
#include "menu_input_owner.h"
#include "iwi_assets.h"
#include <client/client.h>
#include <linux/sys_events.h>
#include <qcommon/qcommon.h>
#include <win32/win_local.h>
#include <ui/keycodes.h>
#include <universal/com_memory.h>
#include <memory>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>

namespace {
struct Metrics {
    unsigned keys=0,characters=0,mouse=0,resizes=0,focus=0,resets=0;
    bool trace=false;

} metrics;
class ShellInput : public kisak::window::ClientInputHost {
public:
    void Mouse(uint32_t time,int x,int y,int dx,int dy) override {
        ++metrics.mouse;
        EngineInput::Mouse(time,x,y,dx,dy);
        if(metrics.trace)std::printf("mouse x=%d y=%d dx=%d dy=%d\n",x,y,dx,dy);
    }
    void Resize(int width,int height) override {
        ClientInputHost::Resize(width,height);++metrics.resizes;
        if(metrics.trace)std::printf("resize %d %d\n",width,height);
    }
    void OnFocus(bool focused) override {
        ClientInputHost::OnFocus(focused);
        ++metrics.focus;
        if(metrics.trace) { std::printf("focus %s\n",focused?"gained":"lost");
            if(!focused)std::printf("input=reset held=%d\n",Held()); }
    }
    void OnQuit() override { std::puts("window=closing"); }
};
void PresentEngineClear(kisak::window::WindowHost &host,kisak::window::CpuRenderer &renderer,kisak::window::CpuRenderer::TextureHandle &texture,kisak::window::CpuRenderer::UiMaterialHandle &material,kisak::window::CpuRenderer::FontHandle font,std::string_view text,const char *uiImage,kisak::window::MenuTextAsset *menu,const std::array<int,4> *viewport)
{
    if(renderer.Status()!=kisak::window::CpuRenderer::State::stopped && renderer.Status()!=kisak::window::CpuRenderer::State::ready)
        throw std::logic_error("Frontend frame already active");
    const auto extent=host.SurfaceExtent();
    const auto current=renderer.Pixels();
    if(renderer.Status()==kisak::window::CpuRenderer::State::stopped)renderer.Initialize(extent.width,extent.height);
    else if(current.width!=extent.width || current.height!=extent.height)renderer.Resize(extent.width,extent.height);
    if(!menu && (current.width!=extent.width || current.height!=extent.height))
        std::printf("presentation=engine-ui-image width=%d height=%d producer=UI_DrawHandlePic\n",extent.width,extent.height);
    const float color[4]={0.25f,0.5f,0.75f,1.0f};
    if(menu) {
        ScreenPlacement placement{};
        ScrPlace_SetupFloatViewportForDisplay(&placement,0,0,float(extent.width),float(extent.height),1.0f,extent.width,extent.height);
        auto frame=renderer.BeginScopedFrame(host);renderer.Clear(color);
        renderer.SetViewport(0,0,extent.width,extent.height);
        renderer.DrawMenuText(menu->fonts,placement,menu->item,menu->text,menu->item.window.foreColor);
        frame.Finish();
        if(current.width!=extent.width || current.height!=extent.height || renderer.Presented()==1)
            std::printf("presentation=engine-menu-text width=%d height=%d source=%s producer=Item_Text_AutoWrapped_Paint text_rect=%g,%g,%g,%g\n",extent.width,extent.height,menu->source.c_str(),menu->item.textRect[0].x,menu->item.textRect[0].y,menu->item.textRect[0].w,menu->item.textRect[0].h);
        return;
    }

    if(!texture) {
        kisak::window::PixelBuffer image;
        if(uiImage) {
            image=kisak::window::LoadIwiFile(uiImage);
            std::printf("ui_image=loaded width=%d height=%d source=IWI producer=UI_DrawHandlePic\n",image.Extent().width,image.Extent().height);
        } else {
            image.Resize(64,64);auto pixels=image.Bytes();
            for(int y=0;y<64;++y)for(int x=0;x<64;++x) {
                auto *p=pixels.data()+(y*64+x)*4;
                p[0]=(x<32)==(y<32)?255:0;p[1]=x>=32?255:0;p[2]=y>=32 && x<32?255:0;p[3]=255;
            }
        }
        texture=renderer.Upload(image.View()); // The caller's pixels die immediately after upload.
        material=renderer.RegisterUiMaterial(texture);
    }
    auto frame=renderer.BeginScopedFrame(host);renderer.Clear(color);
    const std::array<int,4> view=viewport?*viewport:std::array<int,4>{0,0,extent.width,extent.height};
    renderer.SetViewport(view[0],view[1],view[2],view[3]);
    ScreenPlacement placement{};
    const float tint[4]={1,1,1,.5f};
    renderer.DrawUiImage(material,placement,float(view[2]-128)/2,float(view[3]-96)/2,128,96,5,5,tint);
    if(font) {
        const float textColor[4]={1,1,1,1};
        renderer.DrawUiText(font,placement,text,1024,24,48,5,5,.5f,textColor);
    }
    frame.Finish();
    if(viewport && (current.width!=extent.width || current.height!=extent.height || renderer.Presented()==1))
        std::printf("viewport=engine x=%d y=%d width=%d height=%d target_width=%d target_height=%d producer=CL_SetViewport\n",view[0],view[1],view[2],view[3],extent.width,extent.height);
}
bool Number(const char *text,unsigned maximum,unsigned &value) {
    if(!*text)return false;
    unsigned parsed=0;
    for(const char *p=text;*p;++p) {
        if(*p<'0' || *p>'9' || parsed>(maximum-unsigned(*p-'0'))/10)return false;
        parsed=parsed*10+unsigned(*p-'0');
    }
    value=parsed;return value>0 && value<=maximum;
}
}
int main(int argc,char **argv)
{
    unsigned frames=0,runMilliseconds=0;
    bool relative=false,requestFrame=false;
    bool hasViewport=false;std::array<int,4> viewport{};
    const char *server=nullptr,*fontFile=nullptr,*fontAtlas=nullptr,*uiImage=nullptr,*menuBundle=nullptr,*menuControls=nullptr,*assetDirectory=nullptr;
    std::string text="Native Linux UI";bool explicitText=false;
    kisak::window::Config config;
    for(int i=1;i<argc;++i) {
        const std::string argument(argv[i]);
        if(argument=="--help") {
            std::puts("Usage: KisakCOD-client-input [--frames 1..10000] [--run-ms 1..60000] [--trace] [--relative] [--title NAME] [--frame] [--server IPv4:port] [--menu-bundle DIR --ui-asset-dir DIR] [--menu-controls DIR --ui-asset-dir DIR] [--ui-image IWI] [--ui-viewport X Y W H] [--font-file FONT --font-atlas IWI [--text TEXT]]\n"
                "Disconnected engine input host with bounded CPU UI rendering. In menu-controls mode Escape closes the menu; the local menu_open console command reopens it. Window close exits. Other modes also exit on Escape. Full game frames and gameplay are unavailable.");return 0;
        } else if(argument=="--frames" && i+1<argc) {
            if(!Number(argv[++i],10000,frames))return 2;
        } else if(argument=="--run-ms" && i+1<argc) {
            if(!Number(argv[++i],60000,runMilliseconds))return 2;
        } else if(argument=="--trace")metrics.trace=true;
        else if(argument=="--relative")relative=true;
        else if(argument=="--frame")requestFrame=true;
        else if(argument=="--menu-bundle" && i+1<argc && !menuBundle)menuBundle=argv[++i];
        else if(argument=="--menu-controls" && i+1<argc && !menuControls)menuControls=argv[++i];
        else if(argument=="--ui-asset-dir" && i+1<argc && !assetDirectory)assetDirectory=argv[++i];
        else if(argument=="--ui-image" && i+1<argc && !uiImage)uiImage=argv[++i];
        else if(argument=="--ui-viewport" && i+4<argc && !hasViewport) {
            for(int field=0;field<4;++field) {
                const char *value=argv[++i];unsigned parsed=0;
                if(field<2 && std::string_view(value)=="0")viewport[field]=0;
                else {if(!Number(value,8192,parsed))return 2;viewport[field]=int(parsed);}
            }
            hasViewport=true;
        }
        else if(argument=="--font-file" && i+1<argc && !fontFile)fontFile=argv[++i];
        else if(argument=="--font-atlas" && i+1<argc && !fontAtlas)fontAtlas=argv[++i];
        else if(argument=="--text" && i+1<argc && !explicitText){text=argv[++i];explicitText=true;}
        else if(argument=="--server" && i+1<argc && !server)server=argv[++i];
        else if(argument=="--title" && i+1<argc && *argv[i+1])config.title=argv[++i];
        else return 2;
    }
    if(bool(fontFile)!=bool(fontAtlas) || (explicitText && !fontFile) || text.size()>1024)return 2;
    if(bool(menuBundle||menuControls)!=bool(assetDirectory) || (menuBundle&&menuControls) || ((menuBundle||menuControls) && (fontFile||uiImage||explicitText)))return 2;
    if(menuControls && (relative||server))return 2;
    if(hasViewport && (menuBundle||menuControls))return 2;
    if(menuBundle||menuControls){config.width=640;config.height=480;}
    Sys_InitializeCriticalSections();
    try {
        ShellInput input;
        if(requestFrame) { std::string reason;input.RequestFrame(reason);std::fprintf(stderr,"frame=unavailable %s\n",reason.c_str());return 5; }
        // Diagnostic preference only; the reusable platform host does not choose
        // the eventual game backend. SDL window surfaces may otherwise use GPU acceleration.
        SDL_SetHint(SDL_HINT_FRAMEBUFFER_ACCELERATION,"0");
        kisak::window::WindowHost host(input,config);
        kisak::window::ClientInputHost::WindowCursorBinding cursorBinding(input,host);
        kisak::window::ShutdownSignal signals;
        if(server)input.Discover(server);
        kisak::window::CpuRenderer renderer;
        // Destroy the UI owner (and detach its input callbacks) before the
        // renderer, platform window, or engine input service owner.
        std::unique_ptr<kisak::window::MenuInputOwner> controls;
        std::unique_ptr<kisak::window::MenuTextAsset> menu;
        if(menuControls) {
            const auto extent=host.SurfaceExtent();
            controls=std::make_unique<kisak::window::MenuInputOwner>(input,renderer,menuControls,assetDirectory,extent.width,extent.height);
        }
        if(menuBundle) {
            const auto extent=host.SurfaceExtent();renderer.Initialize(extent.width,extent.height);
            menu=std::make_unique<kisak::window::MenuTextAsset>(kisak::window::LoadMenuBundle(renderer,menuBundle,assetDirectory));
            std::printf("menu=loaded source=%s roles=7 style=%d text=%s visibility=not-evaluated scripts=not-executed\n",menu->source.c_str(),menu->item.textStyle,menu->text.c_str());
        }
        kisak::window::CpuRenderer::FontHandle font=0;
        if(fontFile) {
            const auto extent=host.SurfaceExtent();renderer.Initialize(extent.width,extent.height);
            const auto metrics=kisak::window::LoadFontFile(fontFile);
            const auto atlas=kisak::window::LoadIwiFile(fontAtlas);
            const auto atlasTexture=renderer.Upload(atlas.View());
            const auto atlasMaterial=renderer.RegisterUiMaterial(atlasTexture);
            font=renderer.RegisterFont(atlasMaterial,metrics.name,metrics.pixelHeight,metrics.glyphs);
            std::printf("font=loaded name=%s material=%s glyphs=%zu pixel_height=%d atlas_width=%d atlas_height=%d producer=UI_DrawText\n",
                metrics.name.c_str(),metrics.materialName.c_str(),metrics.glyphs.size(),metrics.pixelHeight,atlas.Extent().width,atlas.Extent().height);
        }
        kisak::window::CpuRenderer::TextureHandle texture=0;
        kisak::window::CpuRenderer::UiMaterialHandle material=0;
        if(controls)controls->Present(host);
        else PresentEngineClear(host,renderer,texture,material,font,text,uiImage,menu.get(),hasViewport?&viewport:nullptr);
        std::printf("window=created driver=%s id=%u title=%s game=false input=engine connection=disconnected frame=unavailable presentation=%s\n",
            SDL_GetCurrentVideoDriver(),host.Id(),config.title,controls?"engine-menu-controls":menu?"engine-menu-text":"engine-ui-image");
        std::printf("window=ready id=%u driver=%s\n",host.Id(),SDL_GetCurrentVideoDriver());
        if(relative)host.SetRelativeMouse(true);
        bool lastRelative=host.RelativeMouseActive();
        std::string lastRelativeError=host.RelativeMouseError();
        if(relative)std::printf("relative requested=1 active=%d error=%s\n",lastRelative,lastRelativeError.c_str());
        std::fflush(stdout);
        const uint32_t started=SDL_GetTicks();
        unsigned drawn=0;
        kisak::window::FrameDeadline cadence;
        while(host.Running()) {
            host.Pump(cadence.WaitMilliseconds(kisak::window::FrameDeadline::Clock::now()));
            if(int signal=signals.PendingSignal()) { std::printf("signal=%d\n",signal);host.RequestQuit(); }
            if(relative && (host.RelativeMouseActive()!=lastRelative || host.RelativeMouseError()!=lastRelativeError)) {
                lastRelative=host.RelativeMouseActive();lastRelativeError=host.RelativeMouseError();
                std::printf("relative requested=%d active=%d error=%s\n",host.RelativeMouseRequested(),lastRelative,lastRelativeError.c_str());
            }
            sysEvent_t event{};
            while(Sys_PollQueuedEvent(&event)) {
                auto release=[](void *p) { if(p)Z_Free(p,10); };
                std::unique_ptr<void,decltype(release)> payload(event.evPtr,release);
                input.Dispatch(event);
                if(event.evType==SE_KEY) {
                    ++metrics.keys;

                    if(metrics.trace)std::printf("key=%d down=%d\n",event.evValue,event.evValue2);
                    if(!menuControls && event.evValue==K_ESCAPE && event.evValue2)host.RequestQuit();
                } else if(event.evType==SE_CHAR) {
                    ++metrics.characters;
                    if(metrics.trace)std::printf("character=%d\n",event.evValue);
                }
            }
            const bool tick=host.Running() && cadence.TakeTick(kisak::window::FrameDeadline::Clock::now());
            if(tick) {
                if(metrics.trace)std::printf("command_tick=%llu\n",static_cast<unsigned long long>(input.Ticks()+1));
                input.Tick();
            }
            if(tick) {
                const auto sample=input.ConsumeMouse();
                if(metrics.trace && (sample.x || sample.y))std::printf("input_tick mouse_dx=%d mouse_dy=%d frame=unavailable\n",sample.x,sample.y);
                if(metrics.trace)std::printf("editor text=%s cursor=%d held=%d\n",input.Text(),input.Cursor(),input.Held());
                if(controls)controls->Present(host);
                else PresentEngineClear(host,renderer,texture,material,font,text,uiImage,menu.get(),hasViewport?&viewport:nullptr);
                ++drawn;
            }
            if(!host.Running())break;
            if((frames && drawn>=frames) || (runMilliseconds && SDL_GetTicks()-started>=runMilliseconds))host.RequestQuit();
            if(metrics.trace)std::fflush(stdout);
        }
        std::printf("window=closed keys=%u characters=%u mouse=%u resizes=%u focus=%u resets=%u held=%d unsupported_text=%u relative_active=%d presented=%u input=engine frame=unavailable game=false\n",
            metrics.keys,metrics.characters,metrics.mouse,metrics.resizes,metrics.focus,input.Resets(),input.Held(),input.UnsupportedCharacters(),host.RelativeMouseActive(),drawn+1);
    } catch(const std::exception &error) {
        std::fprintf(stderr,"Window host failed: %s\n",error.what());Sys_ShutdownEvents();return 1;
    }
    Sys_ShutdownEvents();return 0;
}

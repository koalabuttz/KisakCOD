// KisakCOD GXM probe: the PS Vita client's graphics layer, on its own.
//
// Before the client's renderer runs on sceGxm, this small program checks the
// pieces it stands on, on a real Vita, in a few seconds, and writes what it
// found to ux0:data/KisakCOD/gxm-probe.log:
//
//   1. memory free at start, then after each step below;
//   2. sceGxm, the GPU memory pools, the display buffers and the shader
//      patcher (src/gfx_gxm/gxm_display.h);
//   3. the user's shader compiler, ur0:data/libshacccg.suprx, and the time it
//      takes to compile four small Cg programs;
//   4. drawing: a full-screen colour that cycles (the way the Legacy back end
//      will clear), a spinning blended triangle with byte colours (the
//      D3DCOLOR layout), and a textured quad, for about five seconds, with
//      the time per frame;
//   5. sceGxmTransferFill on the back buffer, the other way to clear.
//
// Without the shader compiler it skips 3 and 4 and says so. START + SELECT
// stops it early. It uses no game data.

#include <gfx_gxm/gxm_display.h>
#include <gfx_gxm/gxm_memory.h>
#include <gfx_gxm/gxm_shader_compiler.h>

#include <psp2/ctrl.h>
#include <psp2/gxm.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/power.h>

#include <malloc.h>

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern "C" int _newlib_heap_size_user = 64 * 1024 * 1024;
extern "C" unsigned int sceUserMainThreadStackSize = 1024 * 1024;

namespace
{
using namespace gfx_gxm;

constexpr const char *kLogPath = "ux0:data/KisakCOD/gxm-probe.log";
SceUID g_log = -1;
uint64_t g_start = 0;

double Seconds()
{
    return (sceKernelGetProcessTimeWide() - g_start) / 1e6;
}

void LogLine(const char *format, ...)
{
    char text[600];
    const int prefix = std::snprintf(text, sizeof(text), "[%8.3f] ", Seconds());
    va_list args;
    va_start(args, format);
    std::vsnprintf(text + prefix, sizeof(text) - prefix - 1, format, args);
    va_end(args);
    std::strncat(text, "\n", sizeof(text) - std::strlen(text) - 1);
    // Written at once, so a crash keeps every line before it.
    if (g_log >= 0)
        sceIoWrite(g_log, text, std::strlen(text));
}

void LogFromDisplay(void *, const char *line)
{
    LogLine("%s", line);
}

void LogMemory(const char *when)
{
    SceKernelFreeMemorySizeInfo system;
    system.size = sizeof(system);
    if (sceKernelGetFreeMemorySize(&system) < 0)
        system.size_user = system.size_cdram = system.size_phycont = 0;
    const struct mallinfo heap = mallinfo();
    LogLine("memory %s: system free %.1f MB main, %.1f MB CDRAM, %.1f MB phycont; heap %.1f of %d MB used", when,
            system.size_user / 1048576.0, system.size_cdram / 1048576.0, system.size_phycont / 1048576.0,
            heap.uordblks / 1048576.0, _newlib_heap_size_user >> 20);
}

bool QuitRequested()
{
    SceCtrlData pad{};
    sceCtrlPeekBufferPositive(0, &pad, 1);
    return (pad.buttons & SCE_CTRL_START) && (pad.buttons & SCE_CTRL_SELECT);
}

// ---- Shaders -------------------------------------------------------------------------

// The clear: a full-screen triangle pair at the far plane in one colour.
const char kClearVertex[] = R"(
void main(float2 position, out float4 outPosition : POSITION)
{
    outPosition = float4(position, 1.0f, 1.0f);
}
)";
const char kClearFragment[] = R"(
uniform float4 u_color;
float4 main() : COLOR
{
    return u_color;
}
)";
// Vertex colours as D3DCOLOR bytes (B, G, R, A in memory), swizzled back here
// the way the Legacy back end will have to.
const char kColorVertex[] = R"(
uniform float2 u_rotation;
void main(float2 position, float4 color,
          out float4 outPosition : POSITION, out float4 outColor : TEXCOORD0)
{
    float2 p = float2(position.x * u_rotation.x - position.y * u_rotation.y,
                      position.x * u_rotation.y + position.y * u_rotation.x);
    outPosition = float4(p * float2(0.5f, 0.9f), 0.5f, 1.0f);
    outColor = color.bgra;
}
)";
const char kColorFragment[] = R"(
float4 main(float4 color : TEXCOORD0) : COLOR
{
    return color;
}
)";
const char kTextureVertex[] = R"(
void main(float2 position, float2 texcoord,
          out float4 outPosition : POSITION, out float2 outTexcoord : TEXCOORD0)
{
    outPosition = float4(position, 0.25f, 1.0f);
    outTexcoord = texcoord;
}
)";
const char kTextureFragment[] = R"(
uniform sampler2D u_texture : TEXUNIT0;
float4 main(float2 texcoord : TEXCOORD0) : COLOR
{
    return tex2D(u_texture, texcoord);
}
)";

struct Program
{
    std::vector<uint8_t> code;
    SceGxmShaderPatcherId id = nullptr;
    const SceGxmProgram *gxp() const { return reinterpret_cast<const SceGxmProgram *>(code.data()); }
};

bool Compile(const char *name, const char *source, ShaderStage stage, Program &out)
{
    const uint64_t begin = sceKernelGetProcessTimeWide();
    std::string log;
    const bool ok = CompileCg(source, std::strlen(source), stage, out.code, log);
    const double ms = (sceKernelGetProcessTimeWide() - begin) / 1000.0;
    LogLine("compile %s: %s in %.1f ms, %u bytes", name, ok ? "ok" : "FAILED", ms,
            static_cast<unsigned>(out.code.size()));
    if (!log.empty())
        LogLine("compile %s diagnostics:\n%s", name, log.c_str());
    if (!ok)
        return false;
    const int result = sceGxmShaderPatcherRegisterProgram(ShaderPatcher(), out.gxp(), &out.id);
    if (result < 0)
    {
        LogLine("register %s: FAILED (0x%08x)", name, static_cast<unsigned>(result));
        return false;
    }
    return true;
}

const SceGxmProgramParameter *Parameter(const Program &program, const char *name)
{
    const SceGxmProgramParameter *parameter = sceGxmProgramFindParameterByName(program.gxp(), name);
    if (!parameter)
        LogLine("parameter %s: not found", name);
    return parameter;
}

// ---- The drawing test ----------------------------------------------------------------

struct ColorVertex
{
    float x, y;
    uint32_t color; // D3DCOLOR: 0xAARRGGBB
};
struct TextureVertex
{
    float x, y, u, v;
};

struct Scene
{
    Program clearV, clearF, colorV, colorF, textureV, textureF;
    SceGxmVertexProgram *clearVertex = nullptr, *colorVertex = nullptr, *textureVertex = nullptr;
    SceGxmFragmentProgram *clearFragment = nullptr, *colorFragment = nullptr, *textureFragment = nullptr;
    const SceGxmProgramParameter *clearColor = nullptr, *rotation = nullptr;
    void *clearVertices = nullptr, *colorVertices = nullptr, *textureVertices = nullptr;
    uint16_t *indices = nullptr;
    void *texels = nullptr;
    SceGxmTexture texture;
};

bool CreateVertexProgram(const Program &program, std::initializer_list<const char *> names,
                         std::initializer_list<SceGxmAttributeFormat> formats,
                         std::initializer_list<uint8_t> components, std::initializer_list<uint16_t> offsets,
                         uint16_t stride, SceGxmVertexProgram *&out)
{
    SceGxmVertexAttribute attributes[4];
    uint32_t count = 0;
    auto format = formats.begin();
    auto component = components.begin();
    auto offset = offsets.begin();
    for (const char *name : names)
    {
        const SceGxmProgramParameter *parameter = Parameter(program, name);
        if (!parameter)
            return false;
        attributes[count].streamIndex = 0;
        attributes[count].offset = *offset++;
        attributes[count].format = static_cast<uint8_t>(*format++);
        attributes[count].componentCount = *component++;
        attributes[count].regIndex = static_cast<uint16_t>(sceGxmProgramParameterGetResourceIndex(parameter));
        ++count;
    }
    SceGxmVertexStream stream{stride, SCE_GXM_INDEX_SOURCE_INDEX_16BIT};
    const int result =
        sceGxmShaderPatcherCreateVertexProgram(ShaderPatcher(), program.id, attributes, count, &stream, 1, &out);
    if (result < 0)
        LogLine("vertex program: FAILED (0x%08x)", static_cast<unsigned>(result));
    return result >= 0;
}

bool CreateFragmentProgram(const Program &program, const Program &vertex, const SceGxmBlendInfo *blend,
                           SceGxmFragmentProgram *&out)
{
    const int result = sceGxmShaderPatcherCreateFragmentProgram(ShaderPatcher(), program.id,
                                                                SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4,
                                                                SCE_GXM_MULTISAMPLE_NONE, blend, vertex.gxp(), &out);
    if (result < 0)
        LogLine("fragment program: FAILED (0x%08x)", static_cast<unsigned>(result));
    return result >= 0;
}

bool BuildScene(Scene &s)
{
    const uint64_t begin = sceKernelGetProcessTimeWide();
    if (!Compile("clear vertex", kClearVertex, ShaderStage::Vertex, s.clearV) ||
        !Compile("clear fragment", kClearFragment, ShaderStage::Fragment, s.clearF) ||
        !Compile("colour vertex", kColorVertex, ShaderStage::Vertex, s.colorV) ||
        !Compile("colour fragment", kColorFragment, ShaderStage::Fragment, s.colorF) ||
        !Compile("texture vertex", kTextureVertex, ShaderStage::Vertex, s.textureV) ||
        !Compile("texture fragment", kTextureFragment, ShaderStage::Fragment, s.textureF))
        return false;
    LogLine("six programs compiled in %.1f ms", (sceKernelGetProcessTimeWide() - begin) / 1000.0);
    LogMemory("after compiling");

    SceGxmBlendInfo alpha{};
    alpha.colorMask = SCE_GXM_COLOR_MASK_ALL;
    alpha.colorFunc = SCE_GXM_BLEND_FUNC_ADD;
    alpha.alphaFunc = SCE_GXM_BLEND_FUNC_ADD;
    alpha.colorSrc = SCE_GXM_BLEND_FACTOR_SRC_ALPHA;
    alpha.colorDst = SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    alpha.alphaSrc = SCE_GXM_BLEND_FACTOR_ONE;
    alpha.alphaDst = SCE_GXM_BLEND_FACTOR_ZERO;

    if (!CreateVertexProgram(s.clearV, {"position"}, {SCE_GXM_ATTRIBUTE_FORMAT_F32}, {2}, {0}, 8, s.clearVertex) ||
        !CreateFragmentProgram(s.clearF, s.clearV, nullptr, s.clearFragment) ||
        !CreateVertexProgram(s.colorV, {"position", "color"},
                             {SCE_GXM_ATTRIBUTE_FORMAT_F32, SCE_GXM_ATTRIBUTE_FORMAT_U8N}, {2, 4}, {0, 8},
                             sizeof(ColorVertex), s.colorVertex) ||
        !CreateFragmentProgram(s.colorF, s.colorV, &alpha, s.colorFragment) ||
        !CreateVertexProgram(s.textureV, {"position", "texcoord"},
                             {SCE_GXM_ATTRIBUTE_FORMAT_F32, SCE_GXM_ATTRIBUTE_FORMAT_F32}, {2, 2}, {0, 8},
                             sizeof(TextureVertex), s.textureVertex) ||
        !CreateFragmentProgram(s.textureF, s.textureV, nullptr, s.textureFragment))
        return false;
    s.clearColor = Parameter(s.clearF, "u_color");
    s.rotation = Parameter(s.colorV, "u_rotation");
    if (!s.clearColor || !s.rotation)
        return false;

    // Geometry in GPU-visible main memory; indices shared by all three.
    s.indices = static_cast<uint16_t *>(Allocate(MemoryKind::Main, 6 * sizeof(uint16_t), 16));
    s.clearVertices = Allocate(MemoryKind::Main, 4 * 8, 16);
    s.colorVertices = Allocate(MemoryKind::Main, 3 * sizeof(ColorVertex), 16);
    s.textureVertices = Allocate(MemoryKind::Main, 4 * sizeof(TextureVertex), 16);
    if (!s.indices || !s.clearVertices || !s.colorVertices || !s.textureVertices)
    {
        LogLine("geometry: no GPU main memory");
        return false;
    }
    const uint16_t indices[6] = {0, 1, 2, 2, 1, 3};
    std::memcpy(s.indices, indices, sizeof(indices));
    const float clear[8] = {-1, -1, 1, -1, -1, 1, 1, 1};
    std::memcpy(s.clearVertices, clear, sizeof(clear));
    const ColorVertex triangle[3] = {
        {0.0f, 0.8f, 0xffff0000u},   // red
        {-0.7f, -0.6f, 0xc000ff00u}, // green, a little transparent
        {0.7f, -0.6f, 0x800000ffu},  // blue, half transparent
    };
    std::memcpy(s.colorVertices, triangle, sizeof(triangle));
    const TextureVertex quad[4] = {
        {0.55f, -0.95f, 0, 1}, {0.95f, -0.95f, 1, 1}, {0.55f, -0.25f, 0, 0}, {0.95f, -0.25f, 1, 0}};
    std::memcpy(s.textureVertices, quad, sizeof(quad));

    // A 64 x 64 checkerboard, white and orange, linear layout.
    constexpr uint32_t size = 64;
    s.texels = Allocate(MemoryKind::Cdram, size * size * 4, SCE_GXM_TEXTURE_ALIGNMENT);
    if (!s.texels)
    {
        LogLine("texture: no CDRAM");
        return false;
    }
    uint32_t *texels = static_cast<uint32_t *>(s.texels);
    for (uint32_t y = 0; y < size; ++y)
        for (uint32_t x = 0; x < size; ++x)
            texels[y * size + x] = ((x / 8 + y / 8) & 1) ? 0xffffffffu : 0xff0080ffu; // ABGR in memory: orange
    const int result =
        sceGxmTextureInitLinear(&s.texture, s.texels, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR, size, size, 0);
    if (result < 0)
    {
        LogLine("texture: sceGxmTextureInitLinear FAILED (0x%08x)", static_cast<unsigned>(result));
        return false;
    }
    return true;
}

void DrawFrame(Scene &s, uint32_t frame)
{
    SceGxmContext *context = Context();
    BeginScene();
    sceGxmSetFrontDepthFunc(context, SCE_GXM_DEPTH_FUNC_ALWAYS);
    sceGxmSetFrontDepthWriteEnable(context, SCE_GXM_DEPTH_WRITE_DISABLED);
    sceGxmSetCullMode(context, SCE_GXM_CULL_NONE);

    // Clear: the colour walks around the hue circle once every two seconds.
    const float t = frame / 120.0f * 6.2831853f;
    const float color[4] = {0.5f + 0.4f * std::cos(t), 0.5f + 0.4f * std::cos(t + 2.094f),
                            0.5f + 0.4f * std::cos(t + 4.189f), 1.0f};
    sceGxmSetVertexProgram(context, s.clearVertex);
    sceGxmSetFragmentProgram(context, s.clearFragment);
    void *uniforms = nullptr;
    sceGxmReserveFragmentDefaultUniformBuffer(context, &uniforms);
    sceGxmSetUniformDataF(uniforms, s.clearColor, 0, 4, color);
    sceGxmSetVertexStream(context, 0, s.clearVertices);
    sceGxmDraw(context, SCE_GXM_PRIMITIVE_TRIANGLES, SCE_GXM_INDEX_FORMAT_U16, s.indices, 6);

    const float angle = frame / 60.0f;
    const float rotation[2] = {std::cos(angle), std::sin(angle)};
    sceGxmSetVertexProgram(context, s.colorVertex);
    sceGxmSetFragmentProgram(context, s.colorFragment);
    sceGxmReserveVertexDefaultUniformBuffer(context, &uniforms);
    sceGxmSetUniformDataF(uniforms, s.rotation, 0, 2, rotation);
    sceGxmSetVertexStream(context, 0, s.colorVertices);
    sceGxmDraw(context, SCE_GXM_PRIMITIVE_TRIANGLES, SCE_GXM_INDEX_FORMAT_U16, s.indices, 3);

    sceGxmSetVertexProgram(context, s.textureVertex);
    sceGxmSetFragmentProgram(context, s.textureFragment);
    sceGxmSetFragmentTexture(context, 0, &s.texture);
    sceGxmSetVertexStream(context, 0, s.textureVertices);
    sceGxmDraw(context, SCE_GXM_PRIMITIVE_TRIANGLES, SCE_GXM_INDEX_FORMAT_U16, s.indices, 6);
    EndScene();
}

void RunDrawing(Scene &s)
{
    constexpr uint32_t kFrames = 300;
    uint64_t previous = sceKernelGetProcessTimeWide();
    uint64_t total = 0, worst = 0;
    uint32_t frames = 0;
    for (; frames < kFrames && !QuitRequested(); ++frames)
    {
        DrawFrame(s, frames);
        Present();
        const uint64_t now = sceKernelGetProcessTimeWide();
        const uint64_t frameTime = now - previous;
        previous = now;
        if (frames >= 10) // the first frames fill the display queue
        {
            total += frameTime;
            if (frameTime > worst)
                worst = frameTime;
        }
        if (frames == 0)
            LogLine("first frame presented");
    }
    Finish();
    const uint32_t measured = frames > 10 ? frames - 10 : 0;
    LogLine("drawing: %u frames, %.2f ms average, %.2f ms worst (vsync on: 16.7 ms is 60 fps)", frames,
            measured ? total / 1000.0 / measured : 0.0, worst / 1000.0);
}

// sceGxmTransferFill over the back buffer: a band of green across the top
// third, shown for a second. Times a full-screen fill too.
void RunTransferFill()
{
    // Every queued flip done: the back buffer is off the screen.
    sceGxmDisplayQueueFinish();
    void *back = BackBufferData();
    const uint64_t begin = sceKernelGetProcessTimeWide();
    int result = sceGxmTransferFill(0xff202020u, SCE_GXM_TRANSFER_FORMAT_U8U8U8U8_ABGR, back, 0, 0, kDisplayWidth,
                                    kDisplayHeight, kDisplayStride * 4, nullptr, 0, nullptr);
    if (result >= 0)
        result = sceGxmTransferFinish();
    const double fullMs = (sceKernelGetProcessTimeWide() - begin) / 1000.0;
    if (result < 0)
    {
        LogLine("transfer fill: FAILED (0x%08x)", static_cast<unsigned>(result));
        return;
    }
    result = sceGxmTransferFill(0xff00c000u, SCE_GXM_TRANSFER_FORMAT_U8U8U8U8_ABGR, back, 0, 0, kDisplayWidth,
                                kDisplayHeight / 3, kDisplayStride * 4, nullptr, 0, nullptr);
    if (result >= 0)
        result = sceGxmTransferFinish();
    LogLine("transfer fill: %s, full screen in %.2f ms", result >= 0 ? "ok" : "FAILED", fullMs);
    // Shown without a scene, which might write its tiles over the fill.
    Present();
    for (int i = 0; i < 60 && !QuitRequested(); ++i)
        sceKernelDelayThread(16 * 1000);
    Finish();
}

int Probe()
{
    LogLine("KisakCOD GXM probe: CPU %d MHz, bus %d MHz, GPU %d MHz", scePowerGetArmClockFrequency(),
            scePowerGetBusClockFrequency(), scePowerGetGpuClockFrequency());
    LogMemory("at start");

    DisplayDesc desc;
    desc.memory.cdram = 16u << 20;
    desc.memory.main = 8u << 20;
    desc.memory.vertexUsse = 256u << 10;
    desc.memory.fragmentUsse = 256u << 10;
    desc.log = LogFromDisplay;
    std::string error;
    uint64_t begin = sceKernelGetProcessTimeWide();
    if (!InitDisplay(desc, error))
    {
        LogLine("display: FAILED: %s", error.c_str());
        LogMemory("after the failure");
        return 1;
    }
    LogLine("display up in %.1f ms", (sceKernelGetProcessTimeWide() - begin) / 1000.0);
    LogLine("%s", DescribeMemory().c_str());
    LogMemory("after the display");

    std::string message;
    begin = sceKernelGetProcessTimeWide();
    const bool compiler = LoadShaderCompiler(message);
    LogLine("%s%s (%.1f ms)", compiler ? "" : "shader compiler: ", message.c_str(),
            (sceKernelGetProcessTimeWide() - begin) / 1000.0);
    if (compiler)
    {
        LogMemory("after loading the compiler");
        Scene scene;
        if (BuildScene(scene))
        {
            RunDrawing(scene);
            LogMemory("after drawing");
        }
        else
            LogLine("drawing: skipped, the scene could not be built");
        ReleaseShaderCompiler();
        LogMemory("after releasing the compiler");
    }
    else
        LogLine("drawing: skipped, no shader compiler");

    RunTransferFill();
    LogLine("%s", DescribeMemory().c_str());
    const DisplayStats stats = GetDisplayStats();
    LogLine("frames %llu, scenes %llu", static_cast<unsigned long long>(stats.frames),
            static_cast<unsigned long long>(stats.scenes));
    ShutdownDisplay();
    LogMemory("at the end");
    LogLine("probe done");
    return 0;
}
} // namespace

int main()
{
    g_start = sceKernelGetProcessTimeWide();
    // The CPU and bus clocks the game sets (src/vita/vita_main.cpp), and the
    // GPU at its highest clock, which the client will want too.
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);
    sceIoMkdir("ux0:data/KisakCOD", 0777);
    g_log = sceIoOpen(kLogPath, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_DIGITAL);
    const int status = Probe();
    if (g_log >= 0)
        sceIoClose(g_log);
    sceKernelExitProcess(status);
    return status;
}

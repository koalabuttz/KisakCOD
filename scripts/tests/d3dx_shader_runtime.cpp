// Asset-free runtime coverage for the D3DX9 APIs used by material shader loading.
#include <initguid.h>
#include <d3dx9.h>
#include <d3dx9shader.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
template <typename T> struct ComObject
{
    T *ptr = nullptr;
    ComObject() = default;
    ComObject(const ComObject &) = delete;
    ComObject &operator=(const ComObject &) = delete;
    ~ComObject() { if (ptr) ptr->Release(); }
    T *operator->() const { return ptr; }
};

void Require(bool condition, const char *operation)
{
    if (!condition)
    {
        std::fprintf(stderr, "FAIL: %s\n", operation);
        std::exit(1);
    }
}

class ShaderInclude final : public ID3DXInclude
{
public:
    unsigned opened = 0;
    unsigned closed = 0;
    HRESULT STDMETHODCALLTYPE Open(D3DXINCLUDE_TYPE, const char *filename,
        const void *, const void **data, UINT *bytes) noexcept override
    {
        static const char source[] =
            "float4 applyTint(float2 uv) { return float4(uv, 0, 1) * tint * SHADE_FACTOR; }";
        if (std::strcmp(filename, "probe.hlsl")) return E_FAIL;
        *data = source;
        *bytes = sizeof(source) - 1;
        ++opened;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Close(const void *) noexcept override
    {
        ++closed;
        return S_OK;
    }
};

const char shaderSource[] =
    "float4 tint;\n"
    "#include \"probe.hlsl\"\n"
    "struct VertexInput { float4 position : POSITION0; float2 uv : TEXCOORD0; };\n"
    "struct VertexOutput { float4 position : POSITION0; float2 uv : TEXCOORD0; };\n"
    "VertexOutput vertexMain(VertexInput input) { VertexOutput output; "
    "output.position = input.position; output.uv = input.uv; return output; }\n"
    "float4 pixelMain(float2 uv : TEXCOORD0) : COLOR0 { return applyTint(uv); }\n";

void Compile(const char *entry, const char *profile, ComObject<ID3DXBuffer> &shader,
    ComObject<ID3DXConstantTable> &constants)
{
    const D3DXMACRO macros[] = {{"SHADE_FACTOR", "0.5"}, {nullptr, nullptr}};
    ShaderInclude include;
    ComObject<ID3DXBuffer> errors;
    HRESULT hr = D3DXCompileShader(shaderSource, sizeof(shaderSource) - 1, macros,
        &include, entry, profile, 0, &shader.ptr, &errors.ptr, &constants.ptr);
    if (FAILED(hr))
    {
        std::fprintf(stderr, "D3DXCompileShader(%s) returned 0x%08lx\n", profile,
            static_cast<unsigned long>(hr));
        if (errors.ptr)
            std::fwrite(errors->GetBufferPointer(), 1,
                std::min<DWORD>(errors->GetBufferSize(), 4096), stderr);
    }
    Require(SUCCEEDED(hr) && shader.ptr && constants.ptr, "compile Shader Model 3 shader");
    Require(include.opened > 0 && include.opened == include.closed,
        "shader include callback ownership");
    Require(shader->GetBufferSize() >= 2 * sizeof(DWORD), "compiled shader bytecode");
}

void CheckSemantics(ID3DXBuffer *shader, bool input, UINT usage, UINT index)
{
    const auto function = input ? D3DXGetShaderInputSemantics : D3DXGetShaderOutputSemantics;
    const DWORD *bytecode = static_cast<const DWORD *>(shader->GetBufferPointer());
    UINT count = 0;
    Require(SUCCEEDED(function(bytecode, nullptr, &count)) && count && count <= 16,
        "query shader semantic count");
    std::array<D3DXSEMANTIC, 16> semantics{};
    UINT written = count;
    Require(SUCCEEDED(function(bytecode, semantics.data(), &written)) && written == count,
        "read shader semantics");
    Require(std::any_of(semantics.begin(), semantics.begin() + written,
        [=](const D3DXSEMANTIC &semantic) {
            return semantic.Usage == usage && semantic.UsageIndex == index;
        }), input ? "shader input semantic" : "shader output semantic");
}
}

int main()
{
    static_assert(sizeof(void *) == 4, "The game and this check target Win32.");
    {
        ComObject<ID3DXBuffer> buffer;
        Require(SUCCEEDED(D3DXCreateBuffer(64, &buffer.ptr)) && buffer.ptr,
            "D3DXCreateBuffer");
        Require(buffer->GetBufferSize() == 64 && buffer->GetBufferPointer(),
            "D3DX buffer storage");
        std::memset(buffer->GetBufferPointer(), 0xa5, 64);
        buffer->AddRef();
        buffer->Release();
        Require(static_cast<unsigned char *>(buffer->GetBufferPointer())[63] == 0xa5,
            "D3DX buffer reference ownership");
    }
    std::puts("PASS: D3DX buffer allocation and ownership");

    ComObject<ID3DXBuffer> vertex, pixel;
    ComObject<ID3DXConstantTable> vertexConstants, pixelConstants;
    Compile("vertexMain", "vs_3_0", vertex, vertexConstants);
    Compile("pixelMain", "ps_3_0", pixel, pixelConstants);
    Require(*static_cast<const DWORD *>(vertex->GetBufferPointer()) == D3DVS_VERSION(3, 0),
        "vertex shader version");
    Require(*static_cast<const DWORD *>(pixel->GetBufferPointer()) == D3DPS_VERSION(3, 0),
        "pixel shader version");
    std::puts("PASS: HLSL vertex/pixel compilation with macros and includes");

    ComObject<ID3DXConstantTable> reflected;
    Require(SUCCEEDED(D3DXGetShaderConstantTable(
        static_cast<const DWORD *>(pixel->GetBufferPointer()), &reflected.ptr)) && reflected.ptr,
        "D3DXGetShaderConstantTable");
    ComObject<ID3DXConstantTable> queried;
    Require(SUCCEEDED(reflected->QueryInterface(IID_ID3DXConstantTable,
        reinterpret_cast<void **>(&queried.ptr))) && queried.ptr,
        "constant table SDK interface identity");
    D3DXCONSTANTTABLE_DESC table{};
    Require(SUCCEEDED(reflected->GetDesc(&table)) && table.Constants == 1,
        "constant table description");
    D3DXHANDLE tint = reflected->GetConstantByName(nullptr, "tint");
    D3DXCONSTANT_DESC constant{};
    UINT count = 1;
    Require(tint && SUCCEEDED(reflected->GetConstantDesc(tint, &constant, &count)) && count == 1,
        "named shader constant reflection");
    Require(constant.Type == D3DXPT_FLOAT && constant.Class == D3DXPC_VECTOR &&
        constant.Rows == 1 && constant.Columns == 4 && constant.RegisterCount == 1,
        "float4 shader constant layout");
    std::puts("PASS: shader constant reflection and COM interface identity");

    CheckSemantics(vertex.ptr, true, D3DDECLUSAGE_POSITION, 0);
    CheckSemantics(vertex.ptr, true, D3DDECLUSAGE_TEXCOORD, 0);
    CheckSemantics(vertex.ptr, false, D3DDECLUSAGE_POSITION, 0);
    CheckSemantics(vertex.ptr, false, D3DDECLUSAGE_TEXCOORD, 0);
    CheckSemantics(pixel.ptr, true, D3DDECLUSAGE_TEXCOORD, 0);
    CheckSemantics(pixel.ptr, false, D3DDECLUSAGE_COLOR, 0);
    std::puts("PASS: vertex/pixel input and output semantics");

    ComObject<ID3DXBuffer> invalid, errors;
    ComObject<ID3DXConstantTable> invalidConstants;
    const char invalidSource[] = "this is not a shader";
    HRESULT hr = D3DXCompileShader(invalidSource, sizeof(invalidSource) - 1,
        nullptr, nullptr, "main", "ps_3_0", 0, &invalid.ptr, &errors.ptr, &invalidConstants.ptr);
    Require(FAILED(hr) && !invalid.ptr && !invalidConstants.ptr && errors.ptr &&
        errors->GetBufferPointer() && errors->GetBufferSize(),
        "invalid HLSL returns compiler diagnostics without success outputs");
    std::puts("PASS: invalid HLSL failure and diagnostic ownership");
    return 0;
}

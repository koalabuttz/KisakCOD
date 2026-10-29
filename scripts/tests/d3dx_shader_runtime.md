The MinGW build links the real `d3dx9_43` import library instead of providing
`E_NOTIMPL` implementations. Windows needs the **x86 D3DX9_43 runtime** from the
official DirectX End-User Runtimes (June 2010); the legacy game-provided
`d3dx9_34.dll` does not satisfy this dependency. Wine supplies its own D3DX9_43
and shader compiler implementations. No runtime DLL is stored in this repository.

This asset-free Win32 check exercises the five APIs used by material shader
loading: buffer creation, HLSL compilation, constant-table reflection and shader
input/output semantics. It compiles Shader Model 3 vertex and pixel shaders with
an include callback and macro, checks the reflected constant layout and SDK COM
interface, and verifies invalid HLSL reports an error without success outputs.
It does not require a GPU or game files.

Cross-compile with LLVM-MinGW:

```sh
cmake -S scripts/tests/d3dx -B build-d3dx -G Ninja \
  -DCMAKE_SYSTEM_NAME=Windows \
  -DCMAKE_CXX_COMPILER=i686-w64-mingw32-clang++
cmake --build build-d3dx
wine build-d3dx/kisakcod-d3dx-runtime.exe
```

On native Windows with the DirectX SDK or the `Microsoft.DXSDK.D3DX` NuGet
package's `build/native` directory:

```powershell
cmake -S scripts/tests/d3dx -B build-d3dx -G "Visual Studio 17 2022" -A Win32 `
  -DDXSDK_DIR="C:/path/to/build/native"
cmake --build build-d3dx --config Release
./build-d3dx/Release/kisakcod-d3dx-runtime.exe
```

The native Windows check needs both `D3DX9_43.dll` and its shader compiler runtime
`D3DCompiler_43.dll` installed or staged beside the executable. The check requires
five `PASS` lines and process exit zero. It uses real runtime implementations;
linking the former stub source instead makes the first buffer-allocation check
fail.

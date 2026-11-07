# Shader translator spike tools

Tools for the renderer plan's shader spike (S0): choosing the translator that turns the
game's Direct3D 9 shader bytecode (SM2/SM3) into SPIR-V for the future Vulkan back end.
They live in `scripts/tools/shader_spike/` and are not part of any build.

| File | What it does |
|---|---|
| `shader_corpus.py` | Reads fastfiles from a local game install, finds every vertex and pixel shader, and reports derived counts only: shader models, renderer path (SM2/SM3), families, opcodes, D3D9 features, MP-only vs SP-only, and which features each translator is predicted to reject. `--dump-dir` writes the unique bytecode to a folder outside the repository for translator runs on the same machine. |
| `d3d9_asm.py` | A small SM2/SM3 assembler (D3D assembly in, bytecode with a CTAB out). |
| `cases.py` | 81 op-by-op test shaders, one per instruction or D3D9 feature, written in fxc's style. No game data. |
| `run_translators.py` | Runs translator command-line tools over the cases (or a `--corpus` folder) and checks the SPIR-V with `spirv-val` for Vulkan 1.3. A corpus report holds counts only. |
| `build_translators.sh` | Fetches and builds the pinned candidates (vkd3d-shader 2.0, MojoShader ad5dff8, dxbc-spirv f528e1e) into a work folder outside the repository. |
| `mojo_spv.c` | Command-line driver for MojoShader's SPIR-V profile, used by the build script. |

Typical run on a machine with the game installed:

```sh
ZONE="$HOME/.steam/debian-installation/steamapps/common/Call of Duty 4/zone/english"
python3 scripts/tools/shader_spike/shader_corpus.py \
    --group mp="$ZONE/*mp*.ff" --group sp="$ZONE/*.ff" \
    --out ~/s0/corpus.json --dump-dir ~/s0/bytecode
scripts/tools/shader_spike/build_translators.sh ~/s0/tools
python3 scripts/tools/shader_spike/run_translators.py --corpus ~/s0/bytecode \
    --vkd3d ~/s0/tools/bin/vkd3d-compiler --mojoshader ~/s0/tools/bin/mojo_spv \
    --dxbc-spirv ~/s0/tools/bin/dxbc_compiler --out ~/s0/translators.json
```

Game bytecode, translated SPIR-V and translator builds stay out of the repository.
`scripts/tests/test_shader_spike.py` checks the tools with hand-made shaders and a
synthetic fastfile; it needs no game files and no translators.

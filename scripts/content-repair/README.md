# Repair the missing multiplayer tire-fire effect

Stock multiplayer content can request `fire/tire_fire_med` without including its
effect definition in a loaded fastfile. The engine then reports a missing FX and
uses `misc/missing_fx`. This recipe builds the original effect into a small mod
using your licensed Call of Duty 4 Mod Tools and game assets.

The CSV is an asset manifest. The original effect, compiler, runtime DLL and
generated fastfile stay in your private tool/game installation.

## Required inputs

Use a complete Mod Tools installation in a private working directory, with its
`bin`, `raw`, `deffiles` and `zone_source` directories and supplied license. The
effect source must be present at `raw/fx/fire/tire_fire_med.efx`. Keep the matching
materials, techniques and precompiled shaders from the same kit.

The kit does not supply the game's image files. Extract these existing IWD
members into the kit's `raw/images` directory:

| IWD in the game's `main` directory | Image members |
| --- | --- |
| `iw_01.iwd` | `images/distortion_heat.iwi`, `images/exp_igc.iwi`, `images/fire_fuel.iwi`, `images/fire_roar_pm_atlas.iwi`, `images/flare_halogen_glw.iwi` |
| `iw_03.iwd` | `images/smk_white_atlas.iwi` |

The original x86 `linker_pc.exe` imports `d3dx9_34.dll`. Use the original game DLL
or the x86 DLL from the supplied `DirectX/JUN2007_d3dx9_34_x86.cab`. For Wine,
place this DLL beside `bin/linker_pc.exe` and select its native implementation as
shown below. KisakCOD's LLVM-MinGW executable uses D3DX9_43; the original asset
compiler uses D3DX9_34.

These are the SHA-256 hashes of the inputs used to validate this recipe:

| Input | SHA-256 |
| --- | --- |
| Original `bin/linker_pc.exe` | `7a3bd700eb4e21f5dafd94b25f54600e6963c15d4cec2ddef76d3661ae524c31` |
| Original `raw/fx/fire/tire_fire_med.efx` | `cc1674082efd3557fd51868e95a6984dd1e384cab171143ddf8b5ea80800f3fd` |
| Original x86 `d3dx9_34.dll` | `b6f77f06518d35345fb61172b6a13159125ed60c469d28b1a2e07970e9ddf81d` |

The validated kit snapshot was
[`cod4mw/CoD4-Mod-Tools@95c76d6`](https://github.com/cod4mw/CoD4-Mod-Tools/tree/95c76d6a470bea43fdd5ab591afe8475fac7699d),
with matching compiler/effect blobs in the Promod mirror. The pinned snapshot's
607,138,423-byte archive had SHA-256
`7d0b6d6c3561f91b50e5ca87394374cd18dd2a6fe122b2283239bf15f29e754b`.
Use your existing licensed kit; individual file hashes identify the tested
inputs across different archive formats. The native DLL was checked byte for
byte against the DLL inside the supplied June 2007 x86 DirectX cabinet.

## Build a standalone repair mod

Use an otherwise unused `zone_source/mod.csv` in the private working kit. Copy
[tire-fire.csv](tire-fire.csv) there, then create these output directories:

```text
zone_source/english/assetlist
zone_source/english/assetinfo
zone/english
```

Run the original linker **from the kit's `bin` directory**:

```bat
linker_pc.exe -language english -compress -cleanup mod
```

The output is `zone/english/mod.ff`. Confirm that the linker reports
`process...link...compress...save...done` and exits successfully. Errors can leave
the original tool waiting for a keypress; an existing output file alone does
not establish a successful rebuild.

On a working 32-bit Wine installation, preserve any existing DLL overrides and
select the original D3DX9_34 implementation:

```sh
export WINEDLLOVERRIDES="${WINEDLLOVERRIDES:+$WINEDLLOVERRIDES;}d3dx9_34=n,b"
wine linker_pc.exe -language english -compress -cleanup mod
```

Wine's built-in D3DX9_34 produced `Unknown shader input/output usage 10:4` with
these inputs. The original DLL completed the same build. `WINEDEBUG=+loaddll`
can confirm that the DLL beside the linker loads as `native`.

### Hosts running 32-bit Wine through QEMU

The original linker has a fixed PE base at `0x00400000` and stripped relocations.
Launching Wine's PIE loader directly under QEMU can occupy that address and fail
with status `c0000018`. Use the matching official Wine preloader to reserve the
Windows address space first. The validated preloader was Debian's
`wine32-preloader:i386` version `10.0~repack-6`, matching Wine 10.0.
Its package SHA-256 was
`8cd2c9bc73bc4aa21bfee774bed55506ef2fca82039083c135c7a9147041ae5e`,
verified through Debian's signed archive metadata.

With that preloader installed at its normal Wine path, a working 32-bit prefix
and its Wine/QEMU libraries already configured, run from the kit's `bin` directory:

```sh
export WINEDLLOVERRIDES="${WINEDLLOVERRIDES:+$WINEDLLOVERRIDES;}d3dx9_34=n,b"
WINELOADERNOEXEC=1 qemu-i386 -0 wine-preloader \
  /usr/lib/wine/wine-preloader /usr/lib/wine/wine \
  linker_pc.exe -language english -compress -cleanup mod
```

`WINELOADERNOEXEC=1` skips Wine's second loader exec after the explicit preloader
has run. Select the same `WINEPREFIX` and working `WINESERVER` used by your
existing 32-bit Wine environment. The compiler executable remains unchanged.

## Install and verify

For a standalone repair, create a new game directory
`Mods/codex_asset_repair` and place the newly built `mod.ff` inside it. Launch:

```bat
KisakCOD-mp.exe +set fs_game Mods/codex_asset_repair +set logfile 2 +devmap mp_shipment
```

The existing mod loader loads this fastfile before client graphics registration.
Verify that the selected mod loads, Shipment initializes, and the console no
longer reports `Could not load fx "fire/tire_fire_med".`

To combine this repair with an existing mod, add the entry from
`tire-fire.csv` to that mod's own source manifest and rebuild the full mod.
This retains its scripts, menus and other assets. `fs_game` selects one mod at a
time, so selecting the standalone repair switches away from another mod.

// Asset-free checks for the world front-end probe's rules and its camera list.
//
// The probe itself needs a packaged map, the real database loader and the
// renderer front end, so it cannot run here. What can run here is everything
// that decides whether a frame passes: each rule in world_frontend_invariants.h
// is shown to accept a frame built the way the front end builds one, and to
// reject that frame with exactly one thing broken. The spawn-point scanner is
// driven with hand-written entity strings, including malformed ones.
//
// The test links no engine source; it includes one header.
//
// Build and run (scripts/tests/run_world_surface_checks.py runs it as
// world_frontend_invariants; the probe build also registers it as
// KisakCOD-world-frontend-invariants-test):
//   clang++ -m32 -std=c++20 -I src tests/linux_asset_export/world_frontend_invariants_test.cpp -o /tmp/wf_test
//   /tmp/wf_test

#include <linux/tools/world_frontend_invariants.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{
namespace wf = kisak_world_frontend;

int failures = 0;

void Check(bool condition, const std::string &what)
{
    if (condition)
        return;
    std::printf("FAIL %s\n", what.c_str());
    ++failures;
}

// Keys as R_SortWorldSurfaces leaves them: objectId zero, the material sorted
// index at bit 29.
uint64_t Key(uint32_t material)
{
    return uint64_t(material) << 29;
}

// Two cells, eight surfaces:
//   lit 0..5 (materials A A B B B C), decal 6, emissive 7
//   cell 0 lists 0 1 2 6 7 through its trees and cull groups, cell 1 lists 3 4 5
wf::WorldShape MakeWorld()
{
    wf::WorldShape world;
    world.surfaceCount = 8;
    world.cellSurfaces = {{0, 1, 2, 6, 7}, {3, 4, 5}};
    world.stageRange[wf::kStageLit] = {0, 6};
    world.stageRange[wf::kStageDecal] = {6, 7};
    world.stageRange[wf::kStageEmissive] = {7, 8};
    world.surfaceKey = {Key(1), Key(1), Key(2), Key(2), Key(2), Key(3), Key(4), Key(5)};
    return world;
}

wf::Batch MakeBatch(uint32_t material, uint16_t objectId, std::vector<uint32_t> surfaces)
{
    wf::Batch batch;
    batch.key = Key(material) | objectId;
    batch.surfaces = std::move(surfaces);
    return batch;
}

// The camera is in cell 0 and sees through to cell 1; surface 4 is culled.
// Batches are grouped the way R_AddAllBspDrawSurfacesCamera groups runs of
// equal keys, and each carries its own primitive-stream offset as objectId.
wf::Frame MakeFrame()
{
    wf::Frame frame;
    frame.cameraCell = 0;
    frame.visibleCells = {0, 1};
    frame.visibleSurfaces = {0, 1, 2, 3, 5, 6, 7};
    frame.batches[wf::kStageLit] = {MakeBatch(1, 0, {0, 1}), MakeBatch(2, 3, {2, 3}), MakeBatch(3, 6, {5})};
    frame.batches[wf::kStageDecal] = {MakeBatch(4, 9, {6})};
    frame.batches[wf::kStageEmissive] = {MakeBatch(5, 12, {7})};
    return frame;
}

bool Rejects(const wf::Frame &frame, const char *expected)
{
    const std::vector<std::string> problems = wf::CheckFrame(MakeWorld(), frame);
    for (const std::string &line : problems)
        if (line.find(expected) != std::string::npos)
            return true;
    for (const std::string &line : problems)
        std::printf("  got: %s\n", line.c_str());
    return false;
}

void CheckRules()
{
    const wf::WorldShape world = MakeWorld();
    Check(wf::CheckWorldShape(world).empty(), "the hand-built world can be checked");
    {
        const std::vector<std::string> problems = wf::CheckFrame(world, MakeFrame());
        Check(problems.empty(), "a frame built like the front end's passes every rule");
        for (const std::string &line : problems)
            std::printf("  unexpected: %s\n", line.c_str());
    }

    // World shape.
    {
        wf::WorldShape bad = world;
        bad.surfaceKey.pop_back();
        Check(!wf::CheckWorldShape(bad).empty(), "a key table shorter than the surfaces is refused");
        bad = world;
        bad.cellSurfaces[1] = {5, 3};
        Check(!wf::CheckWorldShape(bad).empty(), "an unsorted cell surface list is refused");
        bad = world;
        bad.cellSurfaces[1] = {3, 8};
        Check(!wf::CheckWorldShape(bad).empty(), "a cell listing a surface past the end is refused");
        bad = world;
        bad.stageRange[wf::kStageDecal] = {7, 6};
        Check(!wf::CheckWorldShape(bad).empty(), "a reversed stage range is refused");
        bad = world;
        bad.stageRange[wf::kStageEmissive] = {7, 9};
        Check(!wf::CheckWorldShape(bad).empty(), "a stage range past the surfaces is refused");
    }

    // Camera cell.
    {
        wf::Frame frame = MakeFrame();
        frame.cameraCell = -1;
        Check(Rejects(frame, "camera cell -1 is not one of 2 cells"), "a camera in no cell is refused");
        frame = MakeFrame();
        frame.cameraCell = 2;
        Check(Rejects(frame, "camera cell 2 is not one of 2 cells"), "a camera cell past the end is refused");
        frame = MakeFrame();
        frame.cameraCell = 1;
        frame.visibleCells = {0};
        frame.visibleSurfaces = {0, 1, 2, 6, 7};
        frame.batches[wf::kStageLit] = {MakeBatch(1, 0, {0, 1}), MakeBatch(2, 3, {2})};
        Check(Rejects(frame, "camera cell 1 is not marked visible"), "an invisible camera cell is refused");
    }

    // Visible cells and surfaces.
    {
        wf::Frame frame = MakeFrame();
        frame.visibleCells = {1, 0};
        Check(Rejects(frame, "visible cell list is not sorted"), "an unsorted visible cell list is refused");
        frame = MakeFrame();
        frame.visibleCells = {0, 1, 2};
        Check(Rejects(frame, "visible cell outside"), "a visible cell past the end is refused");
        frame = MakeFrame();
        frame.visibleSurfaces = {0, 1, 2, 3, 5, 6, 7, 9};
        Check(Rejects(frame, "visible surface 9 is not one of 8"), "a visible surface past the end is refused");
        // Cell 1 is not visible, so its surfaces cannot be.
        frame = MakeFrame();
        frame.visibleCells = {0};
        Check(Rejects(frame, "visible surface 3 is listed by no visible cell"),
              "a surface of a cell the walk did not reach is refused");
    }

    // Batches.
    {
        wf::Frame frame = MakeFrame();
        frame.batches[wf::kStageLit].push_back(MakeBatch(3, 20, {}));
        Check(Rejects(frame, "lit batch 3 is empty"), "an empty batch is refused");
        frame = MakeFrame();
        frame.batches[wf::kStageDecal] = {MakeBatch(4, 9, {5, 6})};
        Check(Rejects(frame, "decal batch 0 holds surface 5 outside the stage range"),
              "a lit surface in the decal stage is refused");
        frame = MakeFrame();
        frame.batches[wf::kStageLit][2] = MakeBatch(3, 6, {4, 5});
        Check(Rejects(frame, "holds surface 4, which is not visible"), "a culled surface in a batch is refused");
        frame = MakeFrame();
        frame.batches[wf::kStageLit][1] = MakeBatch(1, 3, {2, 3});
        Check(Rejects(frame, "lit batch 1 key does not match surface 2"),
              "a batch whose key is another material's is refused");
        // objectId differs per batch and is not part of the comparison.
        frame = MakeFrame();
        frame.batches[wf::kStageLit][0] = MakeBatch(1, 0xFFFF, {0, 1});
        Check(wf::CheckFrame(world, frame).empty(), "objectId is ignored when keys are compared");
        // Only the low 16 bits are objectId: bit 16 is the reflection probe index.
        frame = MakeFrame();
        frame.batches[wf::kStageLit][0].key |= 1ull << 16;
        Check(Rejects(frame, "lit batch 0 key does not match surface 0"), "bits above objectId are compared");
        frame = MakeFrame();
        frame.batches[wf::kStageLit] = {MakeBatch(1, 0, {0, 1}), MakeBatch(2, 3, {2, 3})};
        Check(Rejects(frame, "lit stage leaves visible surface 5 out"), "a visible surface left out is refused");
        frame = MakeFrame();
        frame.batches[wf::kStageLit].push_back(MakeBatch(3, 20, {5}));
        Check(Rejects(frame, "surface 5 is emitted more than once"), "a surface emitted twice is refused");
    }

    // The problem list is bounded.
    {
        wf::Frame frame = MakeFrame();
        frame.visibleCells = {};
        Check(wf::CheckFrame(world, frame, 2).size() == 2, "the problem list stops at its limit");
    }

    // Observations and frame comparisons.
    {
        const wf::Frame frame = MakeFrame();
        Check(wf::CountKeyDescents(frame.batches[wf::kStageLit]) == 0, "ascending keys have no descent");
        std::vector<wf::Batch> down = {MakeBatch(2, 0, {2}), MakeBatch(1, 0, {0}), MakeBatch(1, 5, {1})};
        Check(wf::CountKeyDescents(down) == 1, "one descent is counted once, objectId aside");
        wf::Frame outside = frame;
        wf::WorldShape narrow = world;
        narrow.stageRange[wf::kStageEmissive] = {7, 7};
        Check(wf::CountVisibleOutsideStages(narrow, outside) == 1, "a visible surface in no stage range is counted");
        Check(wf::CountVisibleOutsideStages(world, frame) == 0, "every visible surface is in a stage range");

        Check(wf::IsSubset({0, 2}, {0, 1, 2}), "a subset is a subset");
        Check(!wf::IsSubset({0, 3}, {0, 1, 2}), "a missing element is not a subset");
        Check(wf::SameFrame(frame, MakeFrame()), "a frame equals itself");
        wf::Frame shifted = MakeFrame();
        for (wf::Batch &batch : shifted.batches[wf::kStageLit])
            batch.key += 1; // a different stream offset only
        Check(wf::SameFrame(frame, shifted), "frames that differ only in stream offsets are the same");
        wf::Frame other = MakeFrame();
        other.batches[wf::kStageLit][2].surfaces = {4};
        Check(!wf::SameFrame(frame, other), "frames with different batches differ");
        other = MakeFrame();
        other.visibleSurfaces.pop_back();
        Check(!wf::SameFrame(frame, other), "frames with different visible surfaces differ");
    }
}

wf::SpawnScan Scan(const char *text, size_t limit = 8)
{
    return wf::ScanSpawnViews(text, std::strlen(text), limit);
}

void CheckSpawnScan()
{
    const char *entities =
        "{\n\"classname\" \"worldspawn\"\n\"origin\" \"0 0 0\"\n}\n"
        "{\n\"origin\" \"100 -200 8\"\n\"angles\" \"0 90 0\"\n\"classname\" \"mp_tdm_spawn\"\n}\n"
        "{\n\"classname\" \"info_player_start\"\n\"origin\" \"1 2 3\"\n}\n"
        "{\n\"classname\" \"mp_sab_spawn_allies\"\n\"origin\" \"-5.5 6 0\"\n}\n"
        "{\n\"classname\" \"mp_global_intermission\"\n\"origin\" \"10 20 300\"\n\"angles\" \"15 -45 0\"\n}\n"
        "{\n\"classname\" \"script_model\"\n\"origin\" \"0 0 0\"\n}\n";
    wf::SpawnScan scan = Scan(entities);
    Check(scan.error.empty(), "a well-formed entity string scans: " + scan.error);
    Check(scan.entities == 6, "every entity block is counted");
    Check(scan.matching == 3 && scan.views.size() == 3, "player spawns and the intermission camera match");
    if (scan.views.size() == 3)
    {
        const wf::SpawnView &tdm = scan.views[0];
        Check(tdm.classname == "mp_tdm_spawn", "views keep file order");
        Check(tdm.eye[0] == 100 && tdm.eye[1] == -200 && tdm.eye[2] == 8 + wf::kSpawnEyeHeight,
              "a spawn's eye is its origin raised to the standing view height");
        Check(tdm.angles[0] == 0 && tdm.angles[1] == 90 && tdm.angles[2] == 0, "spawn angles are read");
        const wf::SpawnView &sab = scan.views[1];
        Check(sab.classname == "mp_sab_spawn_allies" && sab.eye[0] == -5.5f, "a mode-specific spawn class matches");
        Check(sab.angles[0] == 0 && sab.angles[1] == 0, "missing angles are zero");
        const wf::SpawnView &cam = scan.views[2];
        Check(cam.eye[2] == 300 && cam.angles[1] == -45, "the intermission camera's origin is the eye as given");
    }

    scan = Scan(entities, 1);
    Check(scan.error.empty() && scan.matching == 3 && scan.views.size() == 1,
          "the limit caps the views but every match is still counted");

    Check(!wf::IsPlayerSpawnClass("info_player_start"), "a non-mp class is not a spawn");
    Check(!wf::IsPlayerSpawnClass("mp_global_intermission"), "the intermission camera is not a player spawn");
    Check(wf::IsPlayerSpawnClass("mp_dm_spawn"), "mp_dm_spawn is a spawn");
    Check(!wf::IsPlayerSpawnClass("xmp_dm_spawn"), "the mp_ prefix is anchored");

    Check(!Scan("\"classname\" \"x\"").error.empty(), "a key outside a block is refused");
    Check(!Scan("{ \"classname\" }").error.empty(), "a key without a value is refused");
    Check(!Scan("{ \"classname\" \"mp_dm_spawn\" ").error.empty(), "an unterminated block is refused");
    Check(!Scan("{ \"classname\" \"mp_dm_spawn").error.empty(), "an unterminated string is refused");
    Check(!Scan("{ \"classname\" \"mp_dm_spawn\" }").error.empty(), "a spawn without an origin is refused");
    Check(!Scan("{ \"classname\" \"mp_dm_spawn\" \"origin\" \"1 2\" }").error.empty(),
          "an origin with two components is refused");
    Check(!Scan("{ \"classname\" \"mp_dm_spawn\" \"origin\" \"1 2 3 4\" }").error.empty(),
          "an origin with four components is refused");
    Check(!Scan("{ \"classname\" \"mp_dm_spawn\" \"origin\" \"1 2 nan\" }").error.empty(),
          "a non-finite origin is refused");
    Check(!Scan("{ \"classname\" \"mp_dm_spawn\" \"origin\" \"1 2 3\" \"angles\" \"x\" }").error.empty(),
          "malformed angles are refused");
    // A non-spawn entity's values are not interpreted, so they cannot fail the scan.
    Check(Scan("{ \"classname\" \"script_origin\" \"origin\" \"bad\" }").error.empty(),
          "a non-spawn entity's values are not parsed");
    Check(Scan("").error.empty() && Scan("").entities == 0, "an empty string has no entities");
    // The engine's entity strings end with a NUL inside numEntityChars.
    const char withNul[] = "{ \"classname\" \"mp_dm_spawn\" \"origin\" \"0 0 0\" }\n";
    wf::SpawnScan nul = wf::ScanSpawnViews(withNul, sizeof(withNul), 4);
    Check(nul.error.empty() && nul.views.size() == 1, "a trailing NUL ends the scan");
}
} // namespace

int main()
{
    CheckRules();
    CheckSpawnScan();
    if (failures)
    {
        std::printf("world_frontend_invariants_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("world_frontend_invariants_test: rules and spawn scan ok\n");
    return 0;
}

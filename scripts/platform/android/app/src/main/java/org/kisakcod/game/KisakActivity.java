package org.kisakcod.game;

import android.content.Intent;
import android.util.Log;

import java.io.File;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

import org.libsdl.app.SDLActivity;

/**
 * The KisakCOD app: SDL3's activity around libmain.so, the Android build of
 * KisakCOD-mp (docs/android.md). SDL3 loads the libraries below, makes the
 * window and calls SDL_main, the game's main.
 *
 * The game folder is the app's folder on shared storage,
 * Android/data/org.kisakcod.game/files, where the player copies the game's
 * files from their own install (main, zone and the rest) over USB or with
 * adb push. Configs and player profiles go to its "home" folder beside them.
 * Nothing needs a storage permission: the app owns that folder.
 *
 * The game always starts full screen. Extra engine arguments for test runs
 * come from the intent's "args" extra, split at spaces (they come last, so
 * they win):
 *   adb shell am start -n org.kisakcod.game/.KisakActivity --es args "+set developer 1"
 */
public class KisakActivity extends SDLActivity {
    private static final String TAG = "KisakCOD";

    @Override
    protected void onDestroy() {
        super.onDestroy();
        // The game's process (":game") ends with it: the engine cannot run
        // twice in one process, and the launcher starts the next game in a
        // new one.
        if (isFinishing()) {
            System.exit(0);
        }
    }

    @Override
    protected String[] getLibraries() {
        // libmain.so needs libSDL3.so, libopenal.so and libd3d9_unavailable.so,
        // which Android's linker finds in the app's own library folder.
        return new String[] { "SDL3", "main" };
    }

    @Override
    protected String[] getArguments() {
        List<String> args = new ArrayList<>();
        File game = getExternalFilesDir(null);
        if (game != null) {
            File home = new File(game, "home");
            if (!home.isDirectory() && !home.mkdirs()) {
                Log.w(TAG, "cannot make " + home);
            }
            Collections.addAll(args, "+set", "fs_basepath", game.getAbsolutePath(),
                    "+set", "fs_homepath", home.getAbsolutePath());
        } else {
            Log.e(TAG, "shared storage is not available; the game cannot find its files");
        }
        // Always full screen: a windowed game leaves Android's navigation bar
        // over part of the screen (a 1920x970 window on the Retroid Pocket 6's
        // 1920x1080 panel, run 3). SDL hides the system bars for a full-screen
        // window. r_fullscreen defaults to off, the desktop's choice.
        Collections.addAll(args, "+set", "r_fullscreen", "1");
        // No shadow maps for now: on the Retroid Pocket 6 (Adreno 740) the sun's
        // shadows flicker from frame to frame (2026-11-15). The maps' baked
        // shadows stay. An "args" extra with "+set sm_enable 1" turns them on.
        Collections.addAll(args, "+set", "sm_enable", "0");
        Intent intent = getIntent();
        String extra = intent != null ? intent.getStringExtra("args") : null;
        if (extra != null) {
            for (String arg : extra.trim().split("\\s+")) {
                if (!arg.isEmpty()) {
                    args.add(arg);
                }
            }
        }
        Log.i(TAG, "arguments: " + args);
        return args.toArray(new String[0]);
    }
}

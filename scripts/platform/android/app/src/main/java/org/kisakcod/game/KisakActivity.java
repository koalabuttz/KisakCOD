package org.kisakcod.game;

import android.content.Intent;
import android.system.ErrnoException;
import android.system.Os;
import android.util.Log;

import java.io.File;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

import org.libsdl.app.SDLActivity;

/**
 * The KisakCOD app: SDL3's activity around libmain.so, the Android build of
 * KisakCOD-mp (docs/android.md), or libmain_sp.so, KisakCOD-sp (the campaign),
 * when the intent's boolean extra "campaign" is true. SDL3 loads the libraries
 * below, makes the window and calls SDL_main, the game's main.
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
 *   adb shell am start -n org.kisakcod.game/.KisakActivity --ez campaign true --es args "+devmap killhouse"
 *   adb shell am start -n org.kisakcod.game/.CampaignActivity --es args "+devmap killhouse"
 */
public class KisakActivity extends SDLActivity {
    private static final String TAG = "KisakCOD";
    // The campaign's player profile when the home folder has none yet.
    private static final String PROFILE = "player";

    // CampaignActivity says yes whatever the intent holds.
    protected boolean campaign() {
        Intent intent = getIntent();
        return intent != null && intent.getBooleanExtra("campaign", false);
    }

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
        // libmain.so (and libmain_sp.so) need libSDL3.so, libopenal.so and
        // libd3d9_unavailable.so, which Android's linker finds in the app's own
        // library folder. One game per process (":game"), so only one loads.
        return new String[] { "SDL3", campaign() ? "main_sp" : "main" };
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
            if (campaign()) {
                seedProfile(home);
            }
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
        setEnvironment(intent != null ? intent.getStringExtra("env") : null);
        Log.i(TAG, (campaign() ? "campaign " : "") + "arguments: " + args);
        return args.toArray(new String[0]);
    }

    // An "env" extra: KISAK_* variables, NAME=VALUE pairs split at spaces, set
    // in this process before the game starts (the game runs in this
    // activity's ":game" process and reads them at start-up), such as
    // KISAK_VK_MODERN_SSR_STEPS for a test run (scripts/runs/android_run.py's
    // "env"). Other names are refused.
    private static void setEnvironment(String extra) {
        if (extra == null) {
            return;
        }
        for (String pair : extra.trim().split("\\s+")) {
            int equals = pair.indexOf('=');
            String name = equals > 0 ? pair.substring(0, equals) : "";
            if (!name.matches("KISAK_[A-Z0-9_]+")) {
                if (!pair.isEmpty()) {
                    Log.w(TAG, "env: " + pair + " refused (only KISAK_NAME=value)");
                }
                continue;
            }
            try {
                Os.setenv(name, pair.substring(equals + 1), true);
                Log.i(TAG, "env: " + pair);
            } catch (ErrnoException e) {
                Log.w(TAG, "env: cannot set " + name + ": " + e);
            }
        }
    }

    // The singleplayer game needs an active player profile before a level
    // loads ("Tried to use a player profile before it was set"), and a phone
    // has no keyboard to name one at first start. The first campaign start
    // makes one called "player"; the game keeps it from then on.
    private static void seedProfile(File home) {
        File profiles = new File(home, "players/profiles");
        File active = new File(profiles, "active.txt");
        if (active.isFile()) {
            return;
        }
        File profile = new File(profiles, PROFILE);
        if (!profile.isDirectory() && !profile.mkdirs()) {
            Log.w(TAG, "cannot make " + profile);
            return;
        }
        try (java.io.FileOutputStream out = new java.io.FileOutputStream(active)) {
            out.write((PROFILE + "\n").getBytes(java.nio.charset.StandardCharsets.UTF_8));
        } catch (java.io.IOException e) {
            Log.w(TAG, "cannot write " + active + ": " + e);
        }
    }
}

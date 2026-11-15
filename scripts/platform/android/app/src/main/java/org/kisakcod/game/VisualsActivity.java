package org.kisakcod.game;

import android.app.Activity;
import android.content.SharedPreferences;
import android.graphics.Color;
import android.graphics.Typeface;
import android.os.Bundle;
import android.util.TypedValue;
import android.view.KeyEvent;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.util.ArrayList;
import java.util.List;

/**
 * The launcher's Visuals page: every setting of the look the game starts
 * with, one button each that steps through its choices, and a Preset button
 * that sets them all at once (Legacy, Modern at 30 fps, Modern Fast at 60
 * fps). Changing any one setting makes the preset Custom.
 *
 * The choices are saved in the launcher's preferences and sent with each
 * start (LauncherActivity.start): the look, the sun shadows and the
 * environment switches every time, and the game's saved settings (frame
 * rate and the Modern passes) only when they changed since the last start,
 * so a change made later in the game's own settings menu stays. "Default"
 * leaves a pass to the renderer (docs/vulkan-modern.md).
 *
 * Controls as on the first screen: the d-pad moves, A presses, B goes back.
 */
public class VisualsActivity extends Activity {
    static final String PREFS = "launcher";

    // What a setting's choice turns into: engine arguments saved by the game
    // (sent when changed), arguments for every start, or a KISAK_* variable.
    private static final int SAVED = 0;
    private static final int EVERY_START = 1;
    private static final int ENV = 2;

    static final class Setting {
        final String key;
        final String label;
        final String help;
        final int kind;
        final boolean modernOnly;
        final String[] choices;
        final String[] values;

        Setting(String key, String label, String help, int kind, boolean modernOnly, String[] choices,
                String[] values) {
            this.key = key;
            this.label = label;
            this.help = help;
            this.kind = kind;
            this.modernOnly = modernOnly;
            this.choices = choices;
            this.values = values;
        }
    }

    private static final String[] TRI = {"Default", "On", "Off"};

    private static Setting dvar(String key, String label, String help, String dvar) {
        return new Setting(key, label, help, SAVED, true, TRI, new String[] {
            "+set " + dvar + " default", "+set " + dvar + " on", "+set " + dvar + " off"});
    }

    private static Setting env(String key, String label, String help, String name) {
        return new Setting(key, label, help, ENV, true, TRI, new String[] {"", name + "=1", name + "=0"});
    }

    static final Setting[] SETTINGS = {
        new Setting("look", "Look", "Legacy: the game as it was made. Modern: the new lighting and effects.",
            EVERY_START, false, new String[] {"Legacy", "Modern"},
            new String[] {"+set r_visuals legacy", "+set r_aaSamples 1 +set r_visuals modern"}),
        new Setting("fps", "Frame rate", "60 fps, or an even 30 fps (every second refresh), or no vsync.",
            SAVED, false, new String[] {"60 fps", "30 fps", "No vsync"},
            new String[] {"+set r_vsync 1 +set r_vsyncInterval 1", "+set r_vsync 1 +set r_vsyncInterval 2",
                "+set r_vsync 0"}),
        new Setting("shadows", "Sun shadows", "Real-time sun shadows. Off by default: on the Retroid they "
                + "still darken some sunlit walls in Legacy.",
            EVERY_START, false, new String[] {"Off", "On"}, new String[] {"+set sm_enable 0", "+set sm_enable 1"}),
        new Setting("ao", "Ambient occlusion", "Soft darkening in corners and creases. Half: at half "
                + "resolution, cheaper.", SAVED, true, new String[] {"Default", "On", "Half", "Off"},
            new String[] {"+set r_modernAo default +set r_modernAoHalf default",
                "+set r_modernAo on +set r_modernAoHalf off", "+set r_modernAo on +set r_modernAoHalf on",
                "+set r_modernAo off"}),
        dvar("ssr", "Reflections", "Screen-space reflections on shiny floors and walls.", "r_modernSsr"),
        env("planar", "Mirror reflections", "Planar reflections on water and flat glossy floors.",
            "KISAK_VK_MODERN_PLANAR"),
        env("probes", "Live reflection probes", "Re-captures the reflection cubemaps with the new lighting.",
            "KISAK_VK_MODERN_PROBES"),
        dvar("contact", "Contact shadows", "Short, sharp shadows where objects meet surfaces.",
            "r_modernContactShadows"),
        dvar("soft", "Soft sun shadows", "Sun shadows that soften with distance from the caster "
                + "(needs sun shadows on).", "r_modernSoftShadows"),
        dvar("softSpot", "Soft lamp shadows", "The same for spot lights' shadows.", "r_modernSoftSpotShadows"),
        dvar("fog", "Volumetric fog", "Light scattered by the map's fog and haze.", "r_modernFog"),
        dvar("froxels", "Fog lit by lamps", "Spot lights shining through the fog.", "r_modernFogFroxels"),
        dvar("godRays", "God rays", "Sun shafts around bright sky and windows.", "r_modernGodRays"),
        dvar("bloom", "Bloom", "Glow around bright lights.", "r_modernBloom"),
        dvar("exposure", "Auto exposure", "The picture adapts to dark and bright places.", "r_modernExposure"),
        env("lamps", "Glowing lamps", "Lamps and lit signs give off light.", "KISAK_VK_MODERN_EMISSIVE"),
        env("flashes", "Muzzle-flash light", "Muzzle flashes and explosions light the scene.",
            "KISAK_VK_MODERN_LIGHT_PASS"),
        env("water", "Water", "The new water shader.", "KISAK_VK_MODERN_WATER"),
        new Setting("tonemap", "Tone map", "How bright light is fitted to the screen.", SAVED, true,
            new String[] {"Default", "Neutral", "AgX", "ACES", "Clamp"},
            new String[] {"+set r_modernTonemap default", "+set r_modernTonemap neutral",
                "+set r_modernTonemap agx", "+set r_modernTonemap aces", "+set r_modernTonemap clamp"}),
    };

    // The presets, as key=choice pairs; any setting a preset does not name is
    // left as it is. Modern Fast drops the dearest passes on a handheld GPU
    // (ambient occlusion, screen-space reflections, contact shadows).
    static final String[] PRESETS = {"Legacy", "Modern", "Modern Fast"};
    private static final String[][] PRESET_CHOICES = {
        {"look=0", "fps=0"},
        {"look=1", "fps=1", "ao=0", "ssr=0", "planar=0", "probes=0", "contact=0", "soft=0", "softSpot=0", "fog=0",
            "froxels=0", "godRays=1", "bloom=0", "exposure=0", "lamps=0", "flashes=0", "water=0", "tonemap=0"},
        {"look=1", "fps=0", "ao=3", "ssr=2", "planar=0", "probes=0", "contact=2", "soft=0", "softSpot=0", "fog=0",
            "froxels=0", "godRays=1", "bloom=0", "exposure=0", "lamps=0", "flashes=0", "water=0", "tonemap=0"},
    };

    static int choice(SharedPreferences prefs, Setting setting) {
        migrate(prefs);
        int value = prefs.getInt("set:" + setting.key, 0);
        return value >= 0 && value < setting.choices.length ? value : 0;
    }

    // The first time: the preset the old Visuals button held ("visuals", or
    // "modern" before Modern Fast).
    private static void migrate(SharedPreferences prefs) {
        if (prefs.contains("set:look")) {
            return;
        }
        int old = prefs.getInt("visuals", prefs.getBoolean("modern", false) ? 1 : 0);
        applyPreset(prefs, Math.max(0, Math.min(PRESETS.length - 1, old)));
    }

    static void applyPreset(SharedPreferences prefs, int preset) {
        SharedPreferences.Editor edit = prefs.edit();
        for (String pair : PRESET_CHOICES[preset]) {
            int equals = pair.indexOf('=');
            edit.putInt("set:" + pair.substring(0, equals), Integer.parseInt(pair.substring(equals + 1)));
        }
        edit.apply();
    }

    // The preset the settings match, or "Custom".
    static String presetName(SharedPreferences prefs) {
        for (int preset = PRESETS.length - 1; preset >= 0; --preset) {
            boolean matches = true;
            for (String pair : PRESET_CHOICES[preset]) {
                int equals = pair.indexOf('=');
                if (prefs.getInt("set:" + pair.substring(0, equals), -1)
                        != Integer.parseInt(pair.substring(equals + 1))) {
                    matches = false;
                    break;
                }
            }
            if (matches) {
                return PRESETS[preset];
            }
        }
        return "Custom";
    }

    // The engine arguments for a start; with markSent, the game's saved
    // settings are remembered as sent.
    static String startArgs(SharedPreferences prefs, boolean markSent) {
        StringBuilder args = new StringBuilder();
        SharedPreferences.Editor edit = prefs.edit();
        for (Setting setting : SETTINGS) {
            if (setting.kind == ENV) {
                continue;
            }
            int value = choice(prefs, setting);
            if (setting.kind == SAVED && prefs.getInt("sent:" + setting.key, -1) == value) {
                continue;
            }
            if (args.length() > 0) {
                args.append(' ');
            }
            args.append(setting.values[value]);
            edit.putInt("sent:" + setting.key, value);
        }
        if (markSent) {
            edit.apply();
        }
        return args.toString();
    }

    // The KISAK_* variables for a start (the "env" extra), for Modern only.
    static String startEnv(SharedPreferences prefs) {
        StringBuilder env = new StringBuilder();
        for (Setting setting : SETTINGS) {
            String value = setting.kind == ENV ? setting.values[choice(prefs, setting)] : "";
            if (!value.isEmpty()) {
                if (env.length() > 0) {
                    env.append(' ');
                }
                env.append(value);
            }
        }
        return env.toString();
    }

    private SharedPreferences prefs;
    private Button presetButton;
    private final List<Button> buttons = new ArrayList<>();
    private final List<View> rows = new ArrayList<>();

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        prefs = getSharedPreferences(PREFS, MODE_PRIVATE);
        migrate(prefs);

        LinearLayout page = new LinearLayout(this);
        page.setOrientation(LinearLayout.VERTICAL);
        page.setPadding(LauncherActivity.dp(this, 32), LauncherActivity.dp(this, 20),
                LauncherActivity.dp(this, 32), LauncherActivity.dp(this, 20));
        page.setBackgroundColor(Color.rgb(18, 20, 22));

        LinearLayout top = new LinearLayout(this);
        top.setOrientation(LinearLayout.HORIZONTAL);
        TextView title = new TextView(this);
        title.setText("Visuals");
        title.setTextColor(Color.WHITE);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        title.setTextSize(TypedValue.COMPLEX_UNIT_SP, 30);
        top.addView(title, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1));
        Button back = LauncherActivity.styledButton(this, "Back", v -> finish());
        top.addView(back, row(0));
        page.addView(top);

        presetButton = LauncherActivity.styledButton(this, "", v -> {
            int next = 0;
            String name = presetName(prefs);
            for (int i = 0; i < PRESETS.length; ++i) {
                if (PRESETS[i].equals(name)) {
                    next = (i + 1) % PRESETS.length;
                }
            }
            applyPreset(prefs, next);
            update();
        });
        page.addView(settingRow(presetButton, "Sets everything below at once. Modern holds an even 30 fps; "
                + "Modern Fast runs at 60 fps without the dearest passes."));

        for (Setting setting : SETTINGS) {
            Button button = LauncherActivity.styledButton(this, "", v -> {
                prefs.edit().putInt("set:" + setting.key, (choice(prefs, setting) + 1) % setting.choices.length)
                        .apply();
                update();
            });
            buttons.add(button);
            View row = settingRow(button, setting.help);
            rows.add(row);
            page.addView(row);
        }

        ScrollView scroll = new ScrollView(this);
        scroll.setFillViewport(true);
        scroll.addView(page);
        setContentView(scroll);
        update();
        presetButton.requestFocus();
    }

    @Override
    protected void onResume() {
        super.onResume();
        LauncherActivity.hideSystemBars(this);
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        int code = event.getKeyCode();
        boolean down = event.getAction() == KeyEvent.ACTION_DOWN;
        if (code == KeyEvent.KEYCODE_BUTTON_A) {
            View focus = getCurrentFocus();
            if (focus != null && !down) {
                focus.performClick();
            }
            return true;
        }
        if (code == KeyEvent.KEYCODE_BUTTON_B) {
            if (!down) {
                finish();
            }
            return true;
        }
        return super.dispatchKeyEvent(event);
    }

    private LinearLayout.LayoutParams row(float weight) {
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(
                weight > 0 ? 0 : LauncherActivity.dp(this, 160), LauncherActivity.dp(this, 64), weight);
        params.setMargins(LauncherActivity.dp(this, 6), LauncherActivity.dp(this, 6),
                LauncherActivity.dp(this, 6), LauncherActivity.dp(this, 6));
        return params;
    }

    private View settingRow(Button button, String help) {
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(android.view.Gravity.CENTER_VERTICAL);
        row.addView(button, row(1));
        TextView text = new TextView(this);
        text.setText(help);
        text.setTextColor(Color.rgb(170, 175, 180));
        text.setTextSize(TypedValue.COMPLEX_UNIT_SP, 15);
        text.setPadding(LauncherActivity.dp(this, 10), 0, 0, 0);
        row.addView(text, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1.4f));
        return row;
    }

    private void update() {
        presetButton.setText("Preset: " + presetName(prefs));
        boolean modern = choice(prefs, SETTINGS[0]) == 1;
        for (int i = 0; i < SETTINGS.length; ++i) {
            Setting setting = SETTINGS[i];
            buttons.get(i).setText(setting.label + ": " + setting.choices[choice(prefs, setting)]);
            // Modern's passes do nothing in Legacy: shown dimmed, still changeable.
            rows.get(i).setAlpha(setting.modernOnly && !modern ? 0.45f : 1.0f);
        }
    }
}

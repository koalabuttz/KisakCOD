package org.kisakcod.game;

import android.app.Activity;
import android.content.Intent;
import android.content.SharedPreferences;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.ColorDrawable;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.StateListDrawable;
import android.os.Bundle;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.widget.Button;
import android.widget.GridLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.TreeMap;

/**
 * The app's first screen: big buttons that a controller or a thumb can work,
 * for starting a match against KisakCOD's bots (docs/bots.md) on any map in
 * the game folder, the game's own multiplayer menu, or the campaign (the
 * singleplayer game's menu). Each choice starts KisakActivity with engine
 * arguments in its "args" extra, and "campaign" for the singleplayer game.
 *
 * The game runs in a process of its own (":game" in the manifest), which
 * ends with the game, so every match starts the engine afresh and this
 * screen comes back when the game quits.
 *
 * Controls: the d-pad or the left stick moves between buttons (Android turns
 * them into arrow keys), A presses, L1 and R1 change the number of bots.
 */
public class LauncherActivity extends Activity {
    private static final String PREFS = "launcher";
    private static final int[] BOT_COUNTS = {1, 3, 5, 7, 9, 11, 13, 17};
    private static final String[][] MODES = {{"war", "Team Deathmatch"}, {"dm", "Free-for-all"}};

    // The stock maps' names as the game shows them; other maps show their file name.
    private static final Map<String, String> MAP_NAMES = new HashMap<>();
    static {
        String[][] names = {
            {"mp_backlot", "Backlot"}, {"mp_bloc", "Bloc"}, {"mp_bog", "Bog"},
            {"mp_broadcast", "Broadcast"}, {"mp_carentan", "Chinatown"}, {"mp_cargoship", "Wet Work"},
            {"mp_citystreets", "District"}, {"mp_convoy", "Ambush"}, {"mp_countdown", "Countdown"},
            {"mp_crash", "Crash"}, {"mp_crash_snow", "Winter Crash"}, {"mp_creek", "Creek"},
            {"mp_crossfire", "Crossfire"}, {"mp_farm", "Downpour"}, {"mp_killhouse", "Killhouse"},
            {"mp_overgrown", "Overgrown"}, {"mp_pipeline", "Pipeline"}, {"mp_shipment", "Shipment"},
            {"mp_showdown", "Showdown"}, {"mp_strike", "Strike"}, {"mp_vacant", "Vacant"},
        };
        for (String[] name : names) {
            MAP_NAMES.put(name[0], name[1]);
        }
    }

    private int modeIndex;
    private int botIndex;
    private Button modeButton;
    private Button botButton;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        SharedPreferences prefs = getSharedPreferences(PREFS, MODE_PRIVATE);
        modeIndex = Math.max(0, Math.min(MODES.length - 1, prefs.getInt("mode", 0)));
        botIndex = Math.max(0, Math.min(BOT_COUNTS.length - 1, prefs.getInt("bots", 4)));

        LinearLayout page = new LinearLayout(this);
        page.setOrientation(LinearLayout.VERTICAL);
        page.setPadding(dp(32), dp(20), dp(32), dp(20));
        page.setBackgroundColor(Color.rgb(18, 20, 22));

        TextView title = new TextView(this);
        title.setText("KisakCOD");
        title.setTextColor(Color.WHITE);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        title.setTextSize(TypedValue.COMPLEX_UNIT_SP, 30);
        page.addView(title);

        LinearLayout settings = new LinearLayout(this);
        settings.setOrientation(LinearLayout.HORIZONTAL);
        modeButton = button("", v -> {
            modeIndex = (modeIndex + 1) % MODES.length;
            update();
        });
        botButton = button("", v -> {
            botIndex = (botIndex + 1) % BOT_COUNTS.length;
            update();
        });
        Button menu = button("Multiplayer", v -> start(null, false));
        Button campaign = button("Campaign", v -> start(null, true));
        settings.addView(modeButton, weighted(2));
        settings.addView(botButton, weighted(1));
        settings.addView(menu, weighted(1));
        settings.addView(campaign, weighted(1));
        page.addView(settings);

        TextView hint = new TextView(this);
        hint.setText("Pick a map to play against bots. A: press   L1/R1: fewer/more bots");
        hint.setTextColor(Color.rgb(170, 175, 180));
        hint.setTextSize(TypedValue.COMPLEX_UNIT_SP, 16);
        hint.setPadding(dp(4), dp(10), 0, dp(4));
        page.addView(hint);

        GridLayout grid = new GridLayout(this);
        grid.setColumnCount(4);
        Map<String, String> maps = findMaps();
        if (maps.isEmpty()) {
            TextView none = new TextView(this);
            File files = getExternalFilesDir(null);
            none.setText("No maps found. Copy your Call of Duty 4 folder (main, zone and the rest) into "
                    + (files != null ? files.getAbsolutePath() : "the app's folder") + ".");
            none.setTextColor(Color.WHITE);
            none.setTextSize(TypedValue.COMPLEX_UNIT_SP, 20);
            page.addView(none);
        }
        for (Map.Entry<String, String> map : maps.entrySet()) {
            final String file = map.getValue();
            Button play = button(map.getKey(), v -> start(matchArgs(file), false));
            GridLayout.LayoutParams cell = new GridLayout.LayoutParams(
                    GridLayout.spec(GridLayout.UNDEFINED), GridLayout.spec(GridLayout.UNDEFINED, 1f));
            cell.width = 0;
            cell.height = dp(84);
            cell.setMargins(dp(6), dp(6), dp(6), dp(6));
            grid.addView(play, cell);
        }

        ScrollView scroll = new ScrollView(this);
        scroll.setFillViewport(true);
        page.addView(grid);
        scroll.addView(page);
        setContentView(scroll);
        update();
        View first = grid.getChildCount() > 0 ? grid.getChildAt(0) : menu;
        first.requestFocus();
    }

    @Override
    protected void onResume() {
        super.onResume();
        hideSystemBars();
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        int code = event.getKeyCode();
        boolean down = event.getAction() == KeyEvent.ACTION_DOWN;
        if (code == KeyEvent.KEYCODE_BUTTON_A) {
            // A presses the button with the focus, as Enter would.
            View focus = getCurrentFocus();
            if (focus != null && !down) {
                focus.performClick();
            }
            return true;
        }
        if ((code == KeyEvent.KEYCODE_BUTTON_L1 || code == KeyEvent.KEYCODE_BUTTON_R1) && down) {
            int step = code == KeyEvent.KEYCODE_BUTTON_R1 ? 1 : -1;
            botIndex = Math.max(0, Math.min(BOT_COUNTS.length - 1, botIndex + step));
            update();
            return true;
        }
        return super.dispatchKeyEvent(event);
    }

    private String matchArgs(String map) {
        String mode = MODES[modeIndex][0];
        int bots = BOT_COUNTS[botIndex];
        // The stock scripts add test clients only in their developer blocks
        // (developer_script), with devmap as the PC's bot runs start them.
        return String.format(Locale.ROOT,
                "+set developer_script 1 +set bot_brain 1 +set scr_testclients %d +set g_gametype %s +devmap %s",
                bots, mode, map);
    }

    private void start(String args, boolean campaign) {
        getSharedPreferences(PREFS, MODE_PRIVATE).edit().putInt("mode", modeIndex).putInt("bots", botIndex).apply();
        Intent intent = new Intent(this, KisakActivity.class);
        if (args != null) {
            intent.putExtra("args", args);
        }
        intent.putExtra("campaign", campaign);
        startActivity(intent);
    }

    private void update() {
        modeButton.setText("Mode: " + MODES[modeIndex][1]);
        botButton.setText("Bots: " + BOT_COUNTS[botIndex]);
    }

    // The maps in the game folder: zone/<language>/mp_*.ff (not the _load
    // files) and usermaps/<map>/<map>.ff, by the name to show.
    private Map<String, String> findMaps() {
        Map<String, String> maps = new TreeMap<>();
        File root = getExternalFilesDir(null);
        if (root == null) {
            return maps;
        }
        List<File> folders = new ArrayList<>();
        File[] zones = new File(root, "zone").listFiles(File::isDirectory);
        if (zones != null) {
            for (File zone : zones) {
                folders.add(zone);
            }
        }
        File[] usermaps = new File(root, "usermaps").listFiles(File::isDirectory);
        if (usermaps != null) {
            for (File usermap : usermaps) {
                folders.add(usermap);
            }
        }
        for (File folder : folders) {
            File[] files = folder.listFiles();
            if (files == null) {
                continue;
            }
            for (File file : files) {
                String name = file.getName().toLowerCase(Locale.ROOT);
                if (!name.startsWith("mp_") || !name.endsWith(".ff") || name.endsWith("_load.ff")) {
                    continue;
                }
                String map = name.substring(0, name.length() - 3);
                String shown = MAP_NAMES.containsKey(map) ? MAP_NAMES.get(map) : map;
                maps.put(shown, map);
            }
        }
        return maps;
    }

    private Button button(String text, View.OnClickListener onClick) {
        Button button = new Button(this);
        button.setText(text);
        button.setAllCaps(false);
        button.setTextColor(Color.WHITE);
        button.setTextSize(TypedValue.COMPLEX_UNIT_SP, 22);
        button.setGravity(Gravity.CENTER);
        button.setFocusable(true);
        button.setFocusableInTouchMode(false);
        StateListDrawable background = new StateListDrawable();
        background.addState(new int[] {android.R.attr.state_pressed}, rounded(Color.rgb(230, 160, 40), 0));
        background.addState(new int[] {android.R.attr.state_focused}, rounded(Color.rgb(70, 76, 82), Color.rgb(240, 180, 60)));
        background.addState(new int[] {}, rounded(Color.rgb(48, 52, 56), 0));
        button.setBackground(background);
        button.setPadding(dp(12), dp(8), dp(12), dp(8));
        button.setOnClickListener(onClick);
        return button;
    }

    private GradientDrawable rounded(int fill, int border) {
        GradientDrawable shape = new GradientDrawable();
        shape.setColor(fill);
        shape.setCornerRadius(dp(10));
        if (border != 0) {
            shape.setStroke(dp(4), border);
        }
        return shape;
    }

    private LinearLayout.LayoutParams weighted(float weight) {
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(0, dp(84), weight);
        params.setMargins(dp(6), dp(10), dp(6), dp(6));
        return params;
    }

    private int dp(int value) {
        return Math.round(TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, value,
                getResources().getDisplayMetrics()));
    }

    private void hideSystemBars() {
        getWindow().setBackgroundDrawable(new ColorDrawable(Color.rgb(18, 20, 22)));
        WindowInsetsController controller = getWindow().getInsetsController();
        if (controller != null) {
            controller.hide(WindowInsets.Type.systemBars());
            controller.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
        }
    }
}

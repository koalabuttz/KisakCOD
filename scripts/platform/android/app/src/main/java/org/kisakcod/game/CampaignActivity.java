package org.kisakcod.game;

/**
 * The campaign: KisakActivity running libmain_sp.so, the singleplayer game
 * (KisakCOD-sp), without needing the "campaign" extra. Runs that can name an
 * activity but no boolean extra (scripts/runs/android_run.py) start this one:
 *   adb shell am start -n org.kisakcod.game/.CampaignActivity --es args "+devmap killhouse"
 */
public class CampaignActivity extends KisakActivity {
    @Override
    protected boolean campaign() {
        return true;
    }
}

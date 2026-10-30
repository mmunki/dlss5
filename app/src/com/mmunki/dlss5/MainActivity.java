package com.mmunki.dlss5;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.SharedPreferences;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.Drawable;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.content.ComponentName;
import android.content.Intent;
import android.provider.Settings;
import android.service.quicksettings.TileService;
import android.text.TextUtils;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.CompoundButton;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.Switch;
import android.widget.TextView;
import android.widget.Toast;

import com.mmunki.dlss5.core.Config;
import com.mmunki.dlss5.core.Status;

import java.util.List;

/**
 * The control centre app.  It writes the config the daemon reads and shows
 * which game is being boosted right now.  Everything privileged goes through
 * {@link RootShell}.
 */
public class MainActivity extends Activity {

    private static final int[] FPS_VALUES = {0, 30, 45, 60, 90, 120};
    private static final int[] MSAA_VALUES = {0, 4, 8};
    private static final int[] REFRESH_VALUES = {0, 60, 90, 120, 144};

    private Config config = Config.defaults();
    private final Handler handler = new Handler(Looper.getMainLooper());

    private TextView statusView;
    private TextView environmentView;
    private Switch enabledSwitch;
    private Switch aiSwitch;
    private Switch gpuSwitch;
    private Switch cpuSwitch;
    private Switch refreshSwitch;
    private Switch hudSwitch;
    private Button overlayButton;
    private Button installButton;
    private SeekBar scaleSeek;
    private SeekBar fpsSeek;
    private SeekBar saturationSeek;
    private SeekBar msaaSeek;
    private SeekBar refreshSeek;
    private SeekBar uiSeek;
    private TextView scaleLabel;
    private TextView fpsLabel;
    private TextView saturationLabel;
    private TextView msaaLabel;
    private TextView refreshLabel;
    private TextView uiLabel;
    private LinearLayout gamesContainer;
    private boolean populating;
    private boolean installing;
    private boolean autoSetupTried;

    private final Runnable statusTick = new Runnable() {
        @Override
        public void run() {
            refreshStatus();
            handler.postDelayed(this, 3000);
        }
    };

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        buildUi();
        loadState();
        handler.postDelayed(statusTick, 1200);
    }

    @Override
    protected void onStop() {
        super.onStop();
        // The user is probably launching a game now: keep the HUD alive for
        // that game until the app is opened again.
        if (config.hud) {
            GameHud.enable(this);
        }
    }

    @Override
    protected void onResume() {
        super.onResume();
        GameHud.disable();
    }

    @Override
    protected void onDestroy() {
        handler.removeCallbacksAndMessages(null);
        GameHud.disable();
        super.onDestroy();
    }

    private int dp(int value) {
        return Math.round(getResources().getDisplayMetrics().density * value);
    }

    private void buildUi() {
        ScrollView scroll = new ScrollView(this);
        scroll.setBackgroundColor(Color.rgb(16, 18, 22));
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(dp(20), dp(24), dp(20), dp(32));
        scroll.addView(root, new ScrollView.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        TextView title = new TextView(this);
        title.setText("DLSS5");
        title.setTextSize(30);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        title.setTextColor(Color.WHITE);
        root.addView(title);

        TextView subtitle = new TextView(this);
        subtitle.setText("Per-game ultra graphics engine: anti-aliasing, GPU/CPU "
                + "boost, refresh lock, game UI mode and colour calibration "
                + "for Android arm64 with root.");
        subtitle.setTextColor(Color.rgb(170, 178, 190));
        subtitle.setTextSize(13);
        subtitle.setPadding(0, dp(4), 0, dp(16));
        root.addView(subtitle);

        statusView = new TextView(this);
        statusView.setText("reading state...");
        statusView.setTextColor(Color.rgb(120, 220, 160));
        statusView.setTextSize(14);
        statusView.setPadding(0, dp(4), 0, dp(16));
        root.addView(statusView);

        environmentView = new TextView(this);
        environmentView.setTextColor(Color.rgb(170, 178, 190));
        environmentView.setTextSize(12);
        environmentView.setPadding(0, 0, 0, dp(16));
        root.addView(environmentView);

        enabledSwitch = addSwitch(root, "Enable DLSS5",
                "The control centre tile flips the same switch.");
        aiSwitch = addSwitch(root, "Adaptive AI controller",
                "Chooses the render scale from temperature, GPU load and SoC class.");
        gpuSwitch = addSwitch(root, "GPU performance boost",
                "Pins the GPU governor to performance while a selected game runs.");
        cpuSwitch = addSwitch(root, "CPU performance boost",
                "Pins every CPU cluster to performance while a selected game runs, "
                        + "then restores the original governors.");
        refreshSwitch = addSwitch(root, "Peak refresh-rate lock",
                "Locks the panel to the target hertz while a selected game runs "
                        + "and restores your previous setting after it.");

        scaleSeek = addSlider(root, "Render scale floor", 0, 70);
        scaleLabel = addValueLabel(root);
        fpsSeek = addSlider(root, "Frame-rate cap", 0, FPS_VALUES.length - 1);
        fpsLabel = addValueLabel(root);
        msaaSeek = addSlider(root, "Anti-aliasing force (MSAA)", 0,
                MSAA_VALUES.length - 1);
        msaaLabel = addValueLabel(root);
        refreshSeek = addSlider(root, "Refresh-rate target", 0,
                REFRESH_VALUES.length - 1);
        refreshLabel = addValueLabel(root);
        uiSeek = addSlider(root, "Game UI size shift (density delta)", 0, 12);
        uiLabel = addValueLabel(root);
        saturationSeek = addSlider(root, "Colour calibration (100 = native)", 0, 50);
        saturationLabel = addValueLabel(root);

        hudSwitch = addSwitch(root, "DLSS5 game HUD",
                "A small pass-through overlay shows the boost state while you "
                        + "play. It can never block a touch of the game.");
        overlayButton = new Button(this);
        overlayButton.setText("Grant the overlay permission");
        overlayButton.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                startActivity(new Intent(Settings.ACTION_MANAGE_OVERLAY_PERMISSION,
                        Uri.parse("package:" + getPackageName())));
            }
        });
        root.addView(overlayButton);

        TextView gamesHeading = new TextView(this);
        gamesHeading.setText("Games DLSS5 affects");
        gamesHeading.setTextColor(Color.WHITE);
        gamesHeading.setTextSize(17);
        gamesHeading.setTypeface(Typeface.DEFAULT_BOLD);
        gamesHeading.setPadding(0, dp(22), 0, dp(4));
        root.addView(gamesHeading);

        TextView gamesHint = new TextView(this);
        gamesHint.setText("Only user-installed apps are listed (that covers every "
                + "game you install yourself; system apps are never touched). "
                + "Marked with * when the app declares the game category, which "
                + "Android's GameManager needs for render scaling - the other "
                + "stages apply to the app regardless.");
        gamesHint.setTextColor(Color.rgb(150, 158, 170));
        gamesHint.setTextSize(12);
        gamesHint.setPadding(0, 0, 0, dp(10));
        root.addView(gamesHint);

        Button refresh = new Button(this);
        refresh.setText("Refresh app list");
        refresh.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                loadApps();
            }
        });
        root.addView(refresh);

        gamesContainer = new LinearLayout(this);
        gamesContainer.setOrientation(LinearLayout.VERTICAL);
        gamesContainer.setPadding(0, dp(8), 0, dp(8));
        root.addView(gamesContainer);

        Button apply = new Button(this);
        apply.setText("Save and apply");
        apply.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                saveConfig();
            }
        });
        root.addView(apply);

        Button restart = new Button(this);
        restart.setText("Restart background daemon");
        restart.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                restartDaemon();
            }
        });
        root.addView(restart);

        installButton = new Button(this);
        installButton.setText("Install / repair the DLSS5 module");
        installButton.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                installModule(false);
            }
        });
        root.addView(installButton);

        TextView installHint = new TextView(this);
        installHint.setText("The Magisk module ships inside this APK. Installing it "
                + "needs root only - no zip to flash, no Magisk app needed. The daemon "
                + "starts immediately; a reboot makes /system/bin/dlss5d available and "
                + "starts DLSS5 with Android.");
        installHint.setTextColor(Color.rgb(140, 148, 160));
        installHint.setTextSize(11);
        installHint.setPadding(dp(4), 0, 0, dp(8));
        root.addView(installHint);

        TextView footer = new TextView(this);
        footer.setText("\nThe Magisk module is inside this APK: 'Install / repair' "
                + "writes it to /data/adb/modules/dlss5 and starts the daemon. A reboot "
                + "finishes the install and autostarts it with Android.\n\n"
                + "DLSS5 is not NVIDIA DLSS. It is an on-device graphics "
                + "engine: it forces anti-aliasing, pins the GPU/CPU governors, "
                + "locks the display refresh rate, shifts the interface size of "
                + "the game, lowers the internal render resolution through "
                + "Android's GameManager when the phone is under load, and "
                + "upscales through the display pipeline. Real DLSS needs motion "
                + "vectors and tensor cores inside the game engine, which no root "
                + "module can add to an arbitrary app.\n\nThe MSAA force depends "
                + "on the game's GLES version and driver. The UI shift changes "
                + "the density Android reports while the game runs (the game "
                + "relaunches its activity once when that happens).");
        footer.setTextColor(Color.rgb(145, 152, 165));
        footer.setTextSize(11);
        root.addView(footer);

        setContentView(scroll);

        enabledSwitch.setOnCheckedChangeListener(new CompoundButton.OnCheckedChangeListener() {
            @Override
            public void onCheckedChanged(CompoundButton buttonView, boolean isChecked) {
                if (populating) {
                    return;
                }
                config.enabled = isChecked;
            }
        });
        aiSwitch.setOnCheckedChangeListener(new CompoundButton.OnCheckedChangeListener() {
            @Override
            public void onCheckedChanged(CompoundButton buttonView, boolean isChecked) {
                if (populating) {
                    return;
                }
                config.ai = isChecked;
            }
        });
        gpuSwitch.setOnCheckedChangeListener(new CompoundButton.OnCheckedChangeListener() {
            @Override
            public void onCheckedChanged(CompoundButton buttonView, boolean isChecked) {
                if (populating) {
                    return;
                }
                config.gpuBoost = isChecked;
            }
        });
        cpuSwitch.setOnCheckedChangeListener(new CompoundButton.OnCheckedChangeListener() {
            @Override
            public void onCheckedChanged(CompoundButton buttonView, boolean isChecked) {
                if (populating) {
                    return;
                }
                config.cpuBoost = isChecked;
            }
        });
        refreshSwitch.setOnCheckedChangeListener(new CompoundButton.OnCheckedChangeListener() {
            @Override
            public void onCheckedChanged(CompoundButton buttonView, boolean isChecked) {
                if (populating) {
                    return;
                }
                config.refreshLock = isChecked;
                updateRefreshLabel();
            }
        });
        hudSwitch.setOnCheckedChangeListener(new CompoundButton.OnCheckedChangeListener() {
            @Override
            public void onCheckedChanged(CompoundButton buttonView, boolean isChecked) {
                if (populating) {
                    return;
                }
                config.hud = isChecked;
                overlayButton.setVisibility(
                        Settings.canDrawOverlays(MainActivity.this)
                                ? View.GONE : View.VISIBLE);
            }
        });

        scaleSeek.setOnSeekBarChangeListener(new SimpleSeekListener() {
            @Override
            public void onProgressChanged(SeekBar seekBar, int progress, boolean fromUser) {
                updateScaleLabel();
            }
        });
        fpsSeek.setOnSeekBarChangeListener(new SimpleSeekListener() {
            @Override
            public void onProgressChanged(SeekBar seekBar, int progress, boolean fromUser) {
                updateFpsLabel();
            }
        });
        msaaSeek.setOnSeekBarChangeListener(new SimpleSeekListener() {
            @Override
            public void onProgressChanged(SeekBar seekBar, int progress, boolean fromUser) {
                updateMsaaLabel();
            }
        });
        refreshSeek.setOnSeekBarChangeListener(new SimpleSeekListener() {
            @Override
            public void onProgressChanged(SeekBar seekBar, int progress, boolean fromUser) {
                updateRefreshLabel();
            }
        });
        uiSeek.setOnSeekBarChangeListener(new SimpleSeekListener() {
            @Override
            public void onProgressChanged(SeekBar seekBar, int progress, boolean fromUser) {
                updateUiLabel();
            }
        });
        saturationSeek.setOnSeekBarChangeListener(new SimpleSeekListener() {
            @Override
            public void onProgressChanged(SeekBar seekBar, int progress, boolean fromUser) {
                updateSaturationLabel();
            }
        });
    }

    private Switch addSwitch(LinearLayout parent, String text, String hint) {
        Switch toggle = new Switch(this);
        toggle.setText(text);
        toggle.setTextColor(Color.WHITE);
        toggle.setTextSize(15);
        toggle.setPadding(0, dp(10), 0, dp(2));
        parent.addView(toggle);

        TextView note = new TextView(this);
        note.setText(hint);
        note.setTextColor(Color.rgb(140, 148, 160));
        note.setTextSize(11);
        note.setPadding(dp(4), 0, 0, dp(8));
        parent.addView(note);
        return toggle;
    }

    private SeekBar addSlider(LinearLayout parent, String title, int min, int max) {
        TextView label = new TextView(this);
        label.setText(title);
        label.setTextColor(Color.WHITE);
        label.setTextSize(14);
        label.setPadding(0, dp(14), 0, 0);
        parent.addView(label);

        SeekBar seekBar = new SeekBar(this);
        seekBar.setMax(max - min);
        parent.addView(seekBar, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        return seekBar;
    }

    private TextView addValueLabel(LinearLayout parent) {
        TextView label = new TextView(this);
        label.setTextColor(Color.rgb(120, 220, 160));
        label.setTextSize(12);
        label.setPadding(dp(4), 0, 0, dp(4));
        parent.addView(label);
        return label;
    }

    private abstract static class SimpleSeekListener
            implements SeekBar.OnSeekBarChangeListener {
        @Override
        public void onStartTrackingTouch(SeekBar seekBar) {
        }

        @Override
        public void onStopTrackingTouch(SeekBar seekBar) {
        }
    }

    private void loadState() {
        new Thread(new Runnable() {
            @Override
            public void run() {
                final boolean root = RootShell.available();
                final boolean module = root && ConfigStore.moduleInstalled();
                final String binary = root ? ConfigStore.daemonPath() : "";
                final boolean ready = root && module && !binary.isEmpty();
                final boolean running = root && ConfigStore.daemonRunning();
                final Config loaded = root ? ConfigStore.load() : Config.defaults();
                final String environment = (root ? "root: yes" : "root: no (su not found)")
                        + "   module: " + (module ? "installed" : "not installed")
                        + "   daemon: " + (running ? "running"
                                : binary.isEmpty() ? "no executable binary" : "stopped")
                        + (binary.isEmpty() ? "" : "\n" + binary);
                runOnUiThread(new Runnable() {
                    @Override
                    public void run() {
                        config = loaded;
                        applyConfigToViews();
                        environmentView.setText(environment);
                        installButton.setText(ready
                                ? "Reinstall / repair the DLSS5 module"
                                : "Install the DLSS5 module (needs root)");
                        if (!root) {
                            Toast.makeText(MainActivity.this,
                                    "DLSS5 needs root. Install the Magisk module first.",
                                    Toast.LENGTH_LONG).show();
                        }
                    }
                });
                loadAppsBlocking(root);
                // The module lives inside the APK, so the app can set itself
                // up on the first launch with root: install, start, offer the
                // reboot.  It runs once; after that the button is the way in.
                if (root && !ready) {
                    SharedPreferences prefs = getSharedPreferences("dlss5", MODE_PRIVATE);
                    if (!autoSetupTried && !prefs.getBoolean("auto_setup", false)) {
                        prefs.edit().putBoolean("auto_setup", true).apply();
                        runOnUiThread(new Runnable() {
                            @Override
                            public void run() {
                                installModule(true);
                            }
                        });
                    }
                }
            }
        }).start();
    }

    private void loadApps() {
        loadAppsBlocking(RootShell.available());
    }

    private void loadAppsBlocking(boolean root) {
        final List<AppCatalog.Entry> entries = AppCatalog.load(this);
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                populateGames(entries);
            }
        });
    }

    private void populateGames(List<AppCatalog.Entry> entries) {
        gamesContainer.removeAllViews();
        if (entries.isEmpty()) {
            TextView empty = new TextView(this);
            empty.setText("No user apps found.");
            empty.setTextColor(Color.rgb(150, 158, 170));
            gamesContainer.addView(empty);
            return;
        }
        for (AppCatalog.Entry entry : entries) {
            gamesContainer.addView(gameRow(entry));
        }
    }

    private View gameRow(final AppCatalog.Entry entry) {
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(0, dp(6), 0, dp(6));

        ImageView icon = new ImageView(this);
        Drawable drawable = entry.icon;
        if (drawable != null) {
            icon.setImageDrawable(drawable);
        }
        row.addView(icon, new LinearLayout.LayoutParams(dp(34), dp(34)));

        LinearLayout text = new LinearLayout(this);
        text.setOrientation(LinearLayout.VERTICAL);
        text.setPadding(dp(10), 0, 0, 0);
        TextView label = new TextView(this);
        label.setText((entry.game ? "* " : "") + entry.label);
        label.setTextColor(Color.WHITE);
        label.setTextSize(14);
        text.addView(label);
        TextView pkg = new TextView(this);
        pkg.setText(entry.pkg);
        pkg.setTextColor(Color.rgb(140, 148, 160));
        pkg.setTextSize(11);
        text.addView(pkg);
        row.addView(text, new LinearLayout.LayoutParams(0,
                ViewGroup.LayoutParams.WRAP_CONTENT, 1f));

        final CheckBox check = new CheckBox(this);
        check.setChecked(config.hasGame(entry.pkg));
        check.setOnCheckedChangeListener(new CompoundButton.OnCheckedChangeListener() {
            @Override
            public void onCheckedChanged(CompoundButton buttonView, boolean isChecked) {
                if (isChecked) {
                    config.addGame(entry.pkg);
                } else {
                    config.removeGame(entry.pkg);
                }
            }
        });
        row.addView(check);
        return row;
    }

    private void applyConfigToViews() {
        populating = true;
        config.clamp();
        enabledSwitch.setChecked(config.enabled);
        aiSwitch.setChecked(config.ai);
        gpuSwitch.setChecked(config.gpuBoost);
        cpuSwitch.setChecked(config.cpuBoost);
        refreshSwitch.setChecked(config.refreshLock);
        hudSwitch.setChecked(config.hud);

        scaleSeek.setProgress(Math.round((config.downscale - 0.30f) * 100.0f));
        int fpsIndex = 0;
        for (int i = 0; i < FPS_VALUES.length; i++) {
            if (FPS_VALUES[i] == config.targetFps) {
                fpsIndex = i;
                break;
            }
        }
        fpsSeek.setProgress(fpsIndex);
        msaaSeek.setProgress(Math.max(0, Config.snapMsaa(config.msaa) / 2));
        int refreshIndex = 0;
        for (int i = 0; i < REFRESH_VALUES.length; i++) {
            if (REFRESH_VALUES[i] == config.refreshRate) {
                refreshIndex = i;
                break;
            }
        }
        refreshSeek.setProgress(refreshIndex);
        uiSeek.setProgress(Math.round((config.uiDensityDelta + 120) / 20.0f));
        saturationSeek.setProgress(Math.max(0, config.saturation - 50));
        populating = false;

        updateScaleLabel();
        updateFpsLabel();
        updateMsaaLabel();
        updateRefreshLabel();
        updateUiLabel();
        updateSaturationLabel();
        overlayButton.setVisibility(
                Settings.canDrawOverlays(this) ? View.GONE : View.VISIBLE);
    }

    private float selectedScale() {
        return Config.snapScale(0.30f + scaleSeek.getProgress() * 0.01f);
    }

    private int selectedFps() {
        int index = Math.max(0, Math.min(FPS_VALUES.length - 1, fpsSeek.getProgress()));
        return FPS_VALUES[index];
    }

    private int selectedSaturation() {
        return 50 + saturationSeek.getProgress();
    }

    private int selectedMsaa() {
        int index = Math.max(0, Math.min(MSAA_VALUES.length - 1, msaaSeek.getProgress()));
        return MSAA_VALUES[index];
    }

    private int selectedRefresh() {
        int index = Math.max(0, Math.min(REFRESH_VALUES.length - 1, refreshSeek.getProgress()));
        return REFRESH_VALUES[index];
    }

    private int selectedUiDelta() {
        return (uiSeek.getProgress() - 6) * 20;
    }

    private void updateScaleLabel() {
        scaleLabel.setText(String.format(java.util.Locale.US,
                "floor %.0f%% of native (%s)", selectedScale() * 100.0f,
                selectedScale() >= 0.995f ? "AI cannot go higher" : "AI may raise it"));
    }

    private void updateFpsLabel() {
        int fps = selectedFps();
        fpsLabel.setText(fps == 0 ? "no cap, the panel decides" : fps + " fps");
    }

    private void updateMsaaLabel() {
        int msaa = selectedMsaa();
        msaaLabel.setText(msaa == 0 ? "off, the driver decides"
                : msaa + "x forced on GLES (edge smoothing)");
    }

    private void updateRefreshLabel() {
        int refresh = selectedRefresh();
        if (!refreshSwitch.isChecked()) {
            refreshLabel.setText("refresh-rate lock is off");
        } else if (refresh == 0) {
            refreshLabel.setText("the panel keeps its own default rate");
        } else {
            refreshLabel.setText(refresh + " Hz while a selected game runs"
                    + " (devices that cannot reach it simply play at their top rate)");
        }
    }

    private void updateUiLabel() {
        int delta = selectedUiDelta();
        if (delta == 0) {
            uiLabel.setText("no shift, the game keeps its own interface size");
        } else if (delta < 0) {
            uiLabel.setText(delta + " dpi - compact/sharper UI, more world on screen");
        } else {
            uiLabel.setText("+" + delta + " dpi - bigger UI, easier touch targets");
        }
    }

    private void updateSaturationLabel() {
        int value = selectedSaturation();
        saturationLabel.setText(value >= 100
                ? "native colour, no transform"
                : value + "% saturation (Android caps the transform at 100)");
    }

    private void saveConfig() {
        config.downscale = selectedScale();
        config.targetFps = selectedFps();
        config.saturation = selectedSaturation();
        config.msaa = selectedMsaa();
        config.refreshLock = refreshSwitch.isChecked();
        config.refreshRate = selectedRefresh();
        config.uiDensityDelta = selectedUiDelta();
        config.cpuBoost = cpuSwitch.isChecked();
        config.hud = hudSwitch.isChecked();
        config.enabled = enabledSwitch.isChecked();
        config.ai = aiSwitch.isChecked();
        config.gpuBoost = gpuSwitch.isChecked();
        config.clamp();
        new Thread(new Runnable() {
            @Override
            public void run() {
                final boolean saved = ConfigStore.save(MainActivity.this, config);
                runOnUiThread(new Runnable() {
                    @Override
                    public void run() {
                        Toast.makeText(MainActivity.this,
                                saved ? "Saved. The daemon reloads it immediately."
                                        : "Could not write the config (is root working?)",
                                Toast.LENGTH_SHORT).show();
                        refreshStatus();
                        TileService.requestListeningState(MainActivity.this,
                                new ComponentName(MainActivity.this, GameTileService.class));
                    }
                });
            }
        }).start();
    }

    private void restartDaemon() {
        new Thread(new Runnable() {
            @Override
            public void run() {
                final String output = ConfigStore.restartDaemon();
                runOnUiThread(new Runnable() {
                    @Override
                    public void run() {
                        Toast.makeText(MainActivity.this,
                                TextUtils.isEmpty(output) ? "daemon restart failed" : output,
                                Toast.LENGTH_SHORT).show();
                        refreshStatus();
                    }
                });
            }
        }).start();
    }

    private void installModule(final boolean automatic) {
        if (installing) {
            return;
        }
        installing = true;
        autoSetupTried = true;
        statusView.setText(automatic
                ? "setting up DLSS5, installing the bundled module..."
                : "installing the bundled module...");
        new Thread(new Runnable() {
            @Override
            public void run() {
                final ModuleInstaller.Result result =
                        ModuleInstaller.install(MainActivity.this);
                runOnUiThread(new Runnable() {
                    @Override
                    public void run() {
                        installing = false;
                        if (result.ok) {
                            Toast.makeText(MainActivity.this,
                                    "Module installed, daemon started",
                                    Toast.LENGTH_SHORT).show();
                            showRebootDialog(result.log);
                            loadState();
                        } else {
                            statusView.setText("module install failed");
                            new AlertDialog.Builder(MainActivity.this)
                                    .setTitle("Module install failed")
                                    .setMessage("DLSS5 could not install its module.\n\n"
                                            + "Make sure root works (su -c id prints uid=0) "
                                            + "and that /data/adb is writable, then tap "
                                            + "'Install / repair the DLSS5 module' again.\n\n"
                                            + result.log)
                                    .setPositiveButton("OK", null)
                                    .show();
                        }
                    }
                });
            }
        }).start();
    }

    private void showRebootDialog(String log) {
        new AlertDialog.Builder(this)
                .setTitle("DLSS5 is installed")
                .setMessage("The daemon is running now and the Quick Settings tile works.\n\n"
                        + "Reboot to finish: the module is mounted as /system/bin/dlss5d "
                        + "and starts with Android, so the boost is there before your first "
                        + "game.\n\n" + log)
                .setPositiveButton("Reboot now", new android.content.DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(android.content.DialogInterface dialog, int which) {
                        reboot();
                    }
                })
                .setNegativeButton("Later", null)
                .show();
    }

    private void reboot() {
        new Thread(new Runnable() {
            @Override
            public void run() {
                RootShell.Result result = RootShell.run(
                        "svc power reboot 2>/dev/null || reboot 2>/dev/null");
                if (!result.ok()) {
                    runOnUiThread(new Runnable() {
                        @Override
                        public void run() {
                            Toast.makeText(MainActivity.this,
                                    "Could not reboot, restart the phone manually.",
                                    Toast.LENGTH_LONG).show();
                        }
                    });
                }
            }
        }).start();
    }

    private void refreshStatus() {
        new Thread(new Runnable() {
            @Override
            public void run() {
                final Status status = ConfigStore.status();
                final boolean running = status.running || ConfigStore.daemonRunning();
                runOnUiThread(new Runnable() {
                    @Override
                    public void run() {
                        statusView.setText((running ? "" : "daemon stopped - ")
                                + status.describe());
                    }
                });
            }
        }).start();
    }
}

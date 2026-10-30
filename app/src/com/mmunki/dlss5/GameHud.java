package com.mmunki.dlss5;

import android.annotation.SuppressLint;
import android.content.Context;
import android.graphics.Color;
import android.graphics.PixelFormat;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.os.Handler;
import android.os.Looper;
import android.provider.Settings;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.widget.TextView;

import com.mmunki.dlss5.core.Status;

/**
 * The DLSS5 in-game HUD.  A tiny, non-touchable overlay window that shows
 * what the daemon is doing while a selected game is in the foreground:
 * the render scale, the frame cap, MSAA, CPU/GPU boost and the UI density.
 * The window is drawn with TYPE_APPLICATION_OVERLAY and is fully pass-through
 * (flag NOT_TOUCHABLE), so it can never swallow a touch of the game.
 *
 * It only appears while the daemon's status says active = a selected game is
 * being boosted - and disappears the moment the boost is released.
 */
final class GameHud {

    private static final Object LOCK = new Object();
    private static GameHud instance;

    private final Context app;
    private final Handler main = new Handler(Looper.getMainLooper());
    private final WindowManager windows;

    private View view;
    private TextView body;
    private volatile boolean stopped;
    private Thread worker;
    private boolean shown;

    private GameHud(Context context) {
        app = context.getApplicationContext();
        windows = (WindowManager) app.getSystemService(Context.WINDOW_SERVICE);
    }

    /** Shows the HUD service for as long as the process lives. */
    static void enable(Context context) {
        synchronized (LOCK) {
            if (instance != null) {
                return;
            }
            if (!Settings.canDrawOverlays(context)) {
                return;
            }
            instance = new GameHud(context);
            instance.start();
        }
    }

    /** Removes the overlay; called when the app opens again. */
    static void disable() {
        synchronized (LOCK) {
            if (instance == null) {
                return;
            }
            GameHud hud = instance;
            instance = null;
            hud.stop();
        }
    }

    private void start() {
        worker = new Thread(new Runnable() {
            @Override
            public void run() {
                pollLoop();
            }
        }, "dlss5-hud");
        worker.start();
    }

    private void stop() {
        stopped = true;
        main.post(new Runnable() {
            @Override
            public void run() {
                hide();
            }
        });
        if (worker != null) {
            worker.interrupt();
        }
    }

    private void pollLoop() {
        while (!stopped) {
            try {
                Status status = ConfigStore.status();
                boolean active = status.running && status.enabled && status.active;
                main.post(new Runnable() {
                    @Override
                    public void run() {
                        update(status, active);
                    }
                });
            } catch (RuntimeException e) {
                // su lost permission or the daemon binary vanished: hide instead
                // of crashing.
                main.post(new Runnable() {
                    @Override
                    public void run() {
                        hide();
                    }
                });
            }
            try {
                Thread.sleep(1500);
            } catch (InterruptedException e) {
                break;
            }
        }
    }

    @SuppressLint("InlinedApi")
    private void update(Status status, boolean active) {
        if (stopped) {
            return;
        }
        if (active) {
            show(textOf(status));
        } else {
            hide();
        }
    }

    private static String textOf(Status status) {
        StringBuilder line = new StringBuilder("DLSS5 ULTRA");
        line.append("\n").append(status.pkg);
        line.append(String.format(java.util.Locale.US, "\nrender %.0f%%  %d fps",
                status.downscale * 100.0f, status.fps));
        if (status.msaa > 0) {
            line.append("\nMSAA ").append(status.msaa).append("x");
        }
        line.append(status.gpu > 0 ? "\nGPU boost on" : "\nGPU stock");
        line.append(status.cpu > 0 ? "\nCPU boost (" + status.cpu + " clusters)"
                                   : "\nCPU stock");
        if (status.refresh > 0) {
            line.append("\n").append(status.refresh).append(" Hz locked");
        }
        if (status.density > 0) {
            line.append("\nUI ").append(status.density).append(" dpi");
        }
        return line.toString();
    }

    private void show(CharSequence text) {
        if (view == null) {
            view = build();
        }
        body.setText(text);
        try {
            if (!shown) {
                windows.addView(view, params());
                shown = true;
            }
        } catch (RuntimeException e) {
            shown = false;
        }
    }

    private void hide() {
        if (view == null || !shown) {
            return;
        }
        try {
            windows.removeView(view);
        } catch (RuntimeException e) {
            // the window is already gone
        }
        shown = false;
    }

    private View build() {
        GradientDrawable background = new GradientDrawable();
        background.setColor(0xCC0A0D12);
        background.setCornerRadius(dip(12));
        background.setStroke(dip(1), Color.rgb(60, 200, 120));

        body = new TextView(app);
        body.setTextColor(Color.rgb(150, 240, 190));
        body.setTextSize(11.5f);
        body.setTypeface(Typeface.MONOSPACE, Typeface.BOLD);
        body.setPadding(dip(10), dip(8), dip(10), dip(8));
        body.setShadowLayer(dip(2), 0, dip(1), 0xFF001108);
        body.setBackground(background);
        return body;
    }

    private WindowManager.LayoutParams params() {
        WindowManager.LayoutParams lp = new WindowManager.LayoutParams(
                WindowManager.LayoutParams.WRAP_CONTENT,
                WindowManager.LayoutParams.WRAP_CONTENT,
                WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY,
                WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
                        | WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE
                        | WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL
                        | WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS,
                PixelFormat.TRANSLUCENT);
        lp.gravity = Gravity.TOP | Gravity.END;
        lp.x = dip(8);
        lp.y = dip(8);
        return lp;
    }

    private int dip(int value) {
        return Math.round(app.getResources().getDisplayMetrics().density * value);
    }
}

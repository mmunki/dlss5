package com.mmunki.dlss5;

import android.graphics.drawable.Icon;
import android.os.Handler;
import android.os.Looper;
import android.service.quicksettings.Tile;
import android.service.quicksettings.TileService;
import android.widget.Toast;

import com.mmunki.dlss5.core.Config;

/**
 * The Quick Settings tile: one tap turns DLSS5 on or off.  The daemon picks
 * the change up immediately and boosts (or releases) the game that is in the
 * foreground, so the current game's UI gets the boost without a restart.
 */
public class GameTileService extends TileService {

    private final Handler handler = new Handler(Looper.getMainLooper());

    @Override
    public void onStartListening() {
        super.onStartListening();
        refreshFromConfig();
    }

    @Override
    public void onClick() {
        new Thread(new Runnable() {
            @Override
            public void run() {
                final Config config = ConfigStore.load();
                final boolean next = !config.enabled;
                config.enabled = next;
                final boolean saved = ConfigStore.save(GameTileService.this, config);
                if (saved && !ConfigStore.daemonRunning()) {
                    ConfigStore.restartDaemon();
                }
                handler.post(new Runnable() {
                    @Override
                    public void run() {
                        applyTileState(saved ? next : !next);
                        Toast.makeText(GameTileService.this,
                                saved ? (next ? "DLSS5 on" : "DLSS5 off")
                                        : "DLSS5 cannot write the config (root?)",
                                Toast.LENGTH_SHORT).show();
                    }
                });
            }
        }).start();
    }

    private void refreshFromConfig() {
        new Thread(new Runnable() {
            @Override
            public void run() {
                final Config config = ConfigStore.load();
                handler.post(new Runnable() {
                    @Override
                    public void run() {
                        applyTileState(config.enabled);
                    }
                });
            }
        }).start();
    }

    private void applyTileState(boolean enabled) {
        Tile tile = getQsTile();
        if (tile == null) {
            return;
        }
        tile.setState(enabled ? Tile.STATE_ACTIVE : Tile.STATE_INACTIVE);
        tile.setLabel(enabled ? "DLSS5 on" : "DLSS5 off");
        tile.setIcon(Icon.createWithResource(this,
                enabled ? R.drawable.ic_tile_on : R.drawable.ic_tile_off));
        tile.updateTile();
    }
}

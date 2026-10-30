package com.mmunki.dlss5;

import android.content.Context;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.graphics.drawable.Drawable;

import com.mmunki.dlss5.core.GameFilter;

import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.List;

/** The list of user-installed apps, games first. */
public final class AppCatalog {

    public static final class Entry {
        public final String pkg;
        public final String label;
        public final Drawable icon;
        public final boolean game;

        Entry(String pkg, String label, Drawable icon, boolean game) {
            this.pkg = pkg;
            this.label = label;
            this.icon = icon;
            this.game = game;
        }
    }

    private AppCatalog() {
    }

    public static List<Entry> load(Context context) {
        PackageManager manager = context.getPackageManager();
        List<ApplicationInfo> apps = manager.getInstalledApplications(0);
        String self = context.getPackageName();
        List<Entry> entries = new ArrayList<Entry>();
        for (ApplicationInfo info : apps) {
            if (!GameFilter.eligible(info.packageName, info.flags, self)) {
                continue;
            }
            String label;
            Drawable icon;
            try {
                label = manager.getApplicationLabel(info).toString();
                icon = manager.getApplicationIcon(info);
            } catch (Exception e) {
                label = info.packageName;
                icon = null;
            }
            entries.add(new Entry(info.packageName, label, icon,
                    GameFilter.isGame(info.category)));
        }
        Collections.sort(entries, new Comparator<Entry>() {
            @Override
            public int compare(Entry a, Entry b) {
                if (a.game != b.game) {
                    return a.game ? -1 : 1;
                }
                return a.label.compareToIgnoreCase(b.label);
            }
        });
        return entries;
    }
}

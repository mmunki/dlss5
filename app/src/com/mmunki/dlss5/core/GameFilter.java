package com.mmunki.dlss5.core;

/**
 * The rules that keep DLSS5 away from system apps.
 *
 * The integer constants are the values of
 * {@code android.content.pm.ApplicationInfo.FLAG_SYSTEM} and
 * {@code android.content.pm.ApplicationInfo.CATEGORY_GAME}.  They are
 * duplicated here so the filter is plain Java and unit-testable on the host.
 */
public final class GameFilter {

    public static final int FLAG_SYSTEM = 0x00000001;
    public static final int CATEGORY_GAME = 2;

    private GameFilter() {
    }

    /** A package is eligible only when it is a user app and not DLSS5 itself. */
    public static boolean eligible(String pkg, int flags, String selfPackage) {
        if (!validPackage(pkg)) {
            return false;
        }
        if (pkg.equals(selfPackage)) {
            return false;
        }
        return (flags & FLAG_SYSTEM) == 0;
    }

    public static boolean isGame(int category) {
        return category == CATEGORY_GAME;
    }

    public static boolean validPackage(String pkg) {
        if (pkg == null) {
            return false;
        }
        int len = pkg.length();
        if (len == 0 || len >= 128) {
            return false;
        }
        int dots = 0;
        for (int i = 0; i < len; i++) {
            char c = pkg.charAt(i);
            if (c == '.') {
                if (i == 0 || i + 1 == len || pkg.charAt(i - 1) == '.') {
                    return false;
                }
                dots++;
                continue;
            }
            boolean ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                    || (c >= '0' && c <= '9') || c == '_';
            if (!ok) {
                return false;
            }
        }
        return dots >= 1;
    }
}

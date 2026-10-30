import com.mmunki.dlss5.core.Config;
import com.mmunki.dlss5.core.GameFilter;
import com.mmunki.dlss5.core.InstallScript;
import com.mmunki.dlss5.core.MiniJson;
import com.mmunki.dlss5.core.Paths;
import com.mmunki.dlss5.core.Status;

import java.util.List;
import java.util.Map;

/**
 * Host tests for the pure Java core.  No Android class is touched, so this
 * runs with a plain javac/java toolchain in CI.
 */
public final class CoreTests {

    private static int checks;
    private static int failures;

    private static void check(boolean condition, String what) {
        checks++;
        if (!condition) {
            failures++;
            System.err.println("FAIL: " + what);
        }
    }

    public static void main(String[] args) throws Exception {
        if (args.length == 2 && "--dump-install-script".equals(args[0])) {
            // Used by scripts/module-test.sh to execute the exact script the
            // app runs, with /data/adb redirected into a sandbox.
            System.out.print(InstallScript.build(args[1]));
            return;
        }
        json();
        config();
        scale();
        packages();
        status();
        install();
        System.out.println(checks + " checks, " + failures + " failure(s)");
        if (failures != 0) {
            System.exit(1);
        }
    }

    private static void json() throws Exception {
        Map<String, Object> root = MiniJson.parseObject(
                "{\"a\": 1, \"b\": [true, false, \"x\\u00e9\"], \"c\": {\"d\": -2.5}}");
        check(MiniJson.number(root, "a", 0) == 1.0, "number");
        List<Object> list = MiniJson.list(root, "b");
        check(list != null && list.size() == 3, "array length");
        check(Boolean.TRUE.equals(list.get(0)), "true literal");
        check(Boolean.FALSE.equals(list.get(1)), "false literal");
        check("x\u00e9".equals(list.get(2)), "unicode escape");
        Object nested = root.get("c");
        check(nested instanceof Map, "nested object");
        check(MiniJson.quote("a\"b\\c\n").equals("\"a\\\"b\\\\c\\n\""), "quote round trip");
        boolean threw = false;
        try {
            MiniJson.parseObject("{\"a\": }");
        } catch (MiniJson.ParseException e) {
            threw = true;
        }
        check(threw, "invalid json is rejected");
        check(MiniJson.number(MiniJson.parseObject("{\"n\": 7}"), "n", 0) == 7.0, "int parse");
        check(MiniJson.bool(MiniJson.parseObject("{\"b\": true}"), "b", false), "bool parse");
    }

    private static void config() {
        Config config = Config.defaults();
        check(config.enabled && config.ai && config.gpuBoost, "defaults on");
        check(config.cpuBoost && config.hud, "ultra defaults on");
        check(Math.abs(config.downscale - 0.90f) < 1e-6, "default scale");
        check(config.targetFps == 90 && config.saturation == 100, "default fps/colour");
        check(config.msaa == 4, "default msaa");
        check(config.refreshLock && config.refreshRate == 90, "default refresh");
        check(config.uiDensityDelta == 0, "default ui shift");

        config.enabled = false;
        config.ai = false;
        config.gpuBoost = false;
        config.cpuBoost = false;
        config.hud = false;
        config.downscale = 0.75f;
        config.targetFps = 45;
        config.saturation = 92;
        config.msaa = 8;
        config.refreshLock = false;
        config.refreshRate = 120;
        config.uiDensityDelta = -60;
        check(config.addGame("com.example.one"), "add game");
        check(config.addGame("com.example.two"), "add second game");
        check(!config.addGame("com.example.one"), "no duplicates");
        check(!config.addGame("com.example; reboot"), "hostile package rejected");
        check(!config.addGame("nodots"), "package without a dot rejected");

        String json = config.toJson();
        Config loaded = Config.fromJson(json);
        check(!loaded.enabled && !loaded.ai && !loaded.gpuBoost, "flags survive");
        check(!loaded.cpuBoost && !loaded.hud, "ultra flags survive");
        check(Math.abs(loaded.downscale - 0.75f) < 1e-6, "scale survives");
        check(loaded.targetFps == 45, "fps survives");
        check(loaded.saturation == 92, "saturation survives");
        check(loaded.msaa == 8, "msaa survives");
        check(!loaded.refreshLock && loaded.refreshRate == 120, "refresh survives");
        check(loaded.uiDensityDelta == -60, "ui shift survives");
        check(loaded.games.size() == 2, "games survive");
        check(loaded.hasGame("com.example.two"), "hasGame");

        Config wild = Config.fromJson(
                "{\"downscale\": 5, \"target_fps\": 999, \"saturation\": -4, "
                        + "\"msaa\": 9, \"refresh_rate\": -2, \"ui_density_delta\": 9999}");
        check(wild.downscale <= 1.0f && wild.downscale >= Config.SCALE_MIN, "clamp scale");
        check(wild.targetFps == 240, "clamp fps");
        check(wild.saturation == 0, "clamp saturation");
        check(wild.msaa == 8, "clamp msaa");
        check(wild.refreshRate == 0 && wild.refreshLock, "clamp refresh");
        check(wild.uiDensityDelta == 200, "clamp ui shift");

        Config broken = Config.fromJson("{not json");
        check(broken.enabled && broken.games.isEmpty(), "broken json falls back to defaults");

        Config hostile = Config.fromJson(
                "{\"games\": [\"com.x; reboot\", \"ok.pkg\", \"ok.pkg\"]}");
        check(hostile.games.size() == 1 && hostile.hasGame("ok.pkg"),
                "hostile game list sanitised");
    }

    private static void scale() {
        check(Math.abs(Config.snapScale(0.10f) - 0.30f) < 1e-4, "snap low");
        check(Math.abs(Config.snapScale(0.71f) - 0.70f) < 1e-4, "snap 0.71");
        check(Math.abs(Config.snapScale(0.73f) - 0.75f) < 1e-4, "snap 0.73");
        check(Math.abs(Config.snapScale(0.88f) - 0.90f) < 1e-4, "snap 0.88");
        check(Math.abs(Config.snapScale(1.00f) - 1.00f) < 1e-4, "snap native");
        check(Math.abs(Config.snapScale(0.99f) - 1.00f) < 1e-4, "snap near native");
        check(Config.snapScaleText(0.71f).equals("0.70"), "snap text");
    }

    private static void packages() {
        check(GameFilter.validPackage("com.example.game"), "valid");
        check(!GameFilter.validPackage(""), "empty");
        check(!GameFilter.validPackage("nodots"), "no dot");
        check(!GameFilter.validPackage(".leading"), "leading dot");
        check(!GameFilter.validPackage("trailing."), "trailing dot");
        check(!GameFilter.validPackage("double..dot"), "double dot");
        check(!GameFilter.validPackage("bad pkg"), "space");
        check(!GameFilter.validPackage("$(reboot)"), "shell characters");

        check(GameFilter.eligible("com.example.game", 0, "com.mmunki.dlss5"), "user app");
        check(!GameFilter.eligible("com.android.settings", GameFilter.FLAG_SYSTEM,
                "com.mmunki.dlss5"), "system app excluded");
        check(!GameFilter.eligible("com.mmunki.dlss5", 0, "com.mmunki.dlss5"), "self excluded");
        check(!GameFilter.eligible("com.example.game",
                GameFilter.FLAG_SYSTEM | 0x80, "com.mmunki.dlss5"), "updated system excluded");
        check(GameFilter.isGame(GameFilter.CATEGORY_GAME), "game category");
        check(!GameFilter.isGame(0), "not a game category");
    }

    private static void status() {
        Status empty = Status.fromJson("");
        check(!empty.running, "empty status");
        check(empty.describe().contains("not running"), "describe stopped");

        Status status = Status.fromJson(
                "{\"running\": true, \"enabled\": true, \"active\": true, "
                        + "\"package\": \"com.example.game\", \"downscale\": 0.75, "
                        + "\"fps\": 60, \"saturation\": 100, \"scale_hint\": 0.8, "
                        + "\"fps_hint\": 0.9, \"msaa\": 4, \"cpu\": 3, \"gpu\": 1, "
                        + "\"refresh\": 90, \"density\": 360}");
        check(status.running && status.enabled && status.active, "status flags");
        check(status.pkg.equals("com.example.game"), "status package");
        check(Math.abs(status.downscale - 0.75f) < 1e-6, "status scale");
        check(status.fps == 60, "status fps");
        check(status.msaa == 4 && status.cpu == 3 && status.gpu == 1
                && status.refresh == 90 && status.density == 360,
                "ultra status fields");
        check(status.describe().contains("com.example.game"), "describe active");
        check(status.describe().contains("msaa 4x"), "describe shows msaa");
        check(status.describe().contains("90 Hz"), "describe shows refresh");
        check(status.describe().contains("ui 360"), "describe shows ui shift");

        Status paused = Status.fromJson("{\"running\": true, \"enabled\": false}");
        check(paused.describe().contains("paused"), "describe paused");

        Status waiting = Status.fromJson("{\"running\": true, \"enabled\": true}");
        check(waiting.describe().contains("waiting"), "describe waiting");
    }

    /**
     * The install script is the app-side twin of the module installer.  The
     * regression it guards: a recursive 0644 pass *after* the 0755 on the
     * daemon leaves dlss5d non-executable, and every existence check then
     * reports "missing" while the file is right there.
     */
    private static void install() {
        String script = InstallScript.build("/data/user/0/com.mmunki.dlss5/cache/module");
        int recursive = script.indexOf("find \"$DST\" -type f -exec chmod 0644 {} +;");
        int scripts = script.indexOf("chmod 0755 \"$DST\"/service.sh");
        check(recursive >= 0, "install script has the recursive read-only pass");
        check(scripts > recursive, "executables are restored after the recursive pass");
        check(script.indexOf("system/bin/dlss5d;") > recursive,
                "the daemon is made executable after the recursive pass");
        check(script.contains("mv -f \"$DATA/bin/dlss5d.new\" \"$DATA/bin/dlss5d\""),
                "the runtime copy is renamed into place");
        check(script.contains(Paths.RUNTIME_BIN), "runtime copy is the app daemon path");
        check(script.contains("[ -x \"" + Paths.RUNTIME_BIN + "\""),
                "install verifies the daemon is executable");
        check(script.contains("\"" + Paths.RUNTIME_BIN + "\" --version"),
                "install verifies the daemon can run");
        check(script.endsWith("MODULE-INSTALLED"), "install prints the marker last");

        check(Paths.DAEMON_CANDIDATES.length == 3, "three daemon candidates");
        check(Paths.DAEMON_CANDIDATES[0].equals(Paths.RUNTIME_BIN),
                "the executable runtime copy is tried first");
        check(Paths.candidateList().contains(Paths.MODULE_BIN),
                "the module binary is a fallback");
        check(Paths.MODULE_PROP.startsWith(Paths.MODULE_DIR), "module.prop path");
        check(Paths.RUNTIME_BIN.startsWith(Paths.DIR + "/"), "runtime binary is in the state dir");
    }
}

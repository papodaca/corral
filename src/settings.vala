public class Corral.AppSettings : Object {
    private static AppSettings? instance;
    public Settings? settings { get; private set; }

    public static AppSettings get_default () {
        if (instance == null) {
            instance = new AppSettings ();
        }
        return instance;
    }

    public int font_size {
        get {
            return settings != null ? settings.get_int ("font-size") : 10;
        }
        set {
            if (settings != null) {
                settings.set_int ("font-size", int.min (32, int.max (8, value)));
            }
        }
    }

    public int window_width {
        get {
            return settings != null ? settings.get_int ("window-width") : 1100;
        }
        set {
            if (settings != null) {
                settings.set_int ("window-width", int.max (400, value));
            }
        }
    }

    public int window_height {
        get {
            return settings != null ? settings.get_int ("window-height") : 720;
        }
        set {
            if (settings != null) {
                settings.set_int ("window-height", int.max (300, value));
            }
        }
    }

    private AppSettings () {
        var source = SettingsSchemaSource.get_default ();
        var schema = source.lookup ("dev.corral.Corral", true);
        if (schema == null) {
            warning ("GSettings schema dev.corral.Corral is missing; using defaults");
            return;
        }
        settings = new Settings.full (schema, null, null);
    }

    public void bump_font (int delta) {
        font_size = font_size + delta;
    }
}

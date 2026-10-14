public class Corral.AppSettings : Object {
    private static AppSettings? instance;
    public Settings? settings { get; private set; }

    public static AppSettings get_default () {
        if (instance == null) {
            instance = new AppSettings ();
        }
        return instance;
    }

    public string font_family {
        owned get {
            if (settings == null) {
                return "";
            }
            return settings.get_string ("font-family");
        }
        set {
            if (settings == null) {
                return;
            }
            var cleaned = sanitize_font_family (value);
            if (cleaned == font_family) {
                return;
            }
            settings.set_string ("font-family", cleaned);
        }
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

    /* Ghostty config is one key per line. A newline in the family would
     * write extra keys into the overlay. */
    private static string sanitize_font_family (string? raw) {
        if (raw == null) {
            return "";
        }
        var sb = new StringBuilder ();
        unichar c;
        for (int i = 0; raw.get_next_char (ref i, out c);) {
            if (c < 0x20 || c == 0x7f) {
                continue;
            }
            sb.append_unichar (c);
        }
        var cleaned = sb.str.strip ();
        if (cleaned.length > 200) {
            cleaned = cleaned.substring (0, 200).strip ();
        }
        return cleaned;
    }
}

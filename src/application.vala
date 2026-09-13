public class Corral.Application : Adw.Application {
    public Application () {
        Object (
            application_id: "dev.corral.Corral",
            flags: ApplicationFlags.DEFAULT_FLAGS,
            resource_base_path: "/dev/corral/Corral"
        );
    }

    construct {
        ActionEntry[] entries = {
            { "quit", on_quit },
        };
        add_action_entries (entries, this);
        set_accels_for_action ("app.quit", { "<primary>q" });
        set_accels_for_action ("win.preferences", { "<primary>comma" });
        set_accels_for_action ("win.font-increase", { "<primary>plus", "<primary>equal" });
        set_accels_for_action ("win.font-decrease", { "<primary>minus" });
        set_accels_for_action ("win.fullscreen", { "F11" });
        set_accels_for_action ("win.show-help-overlay", { "<primary>question" });
    }

    protected override void activate () {
        var win = active_window;
        if (win == null) {
            win = new Corral.Window (this);
        }
        win.present ();
    }

    private void on_quit (SimpleAction action, Variant? parameter) {
        quit ();
    }
}

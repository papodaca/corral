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
        /* Window-capture accels would eat chords before Herdr. Host
         * shortcuts run in bubble phase on the window instead. */
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

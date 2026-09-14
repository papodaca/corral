public class Corral.Window : Adw.ApplicationWindow {
    private Adw.ToolbarView toolbar_view;
    private Adw.StatusPage? status_page;
    private Ghostty? terminal;
    private bool attaching = false;

    public Window (Gtk.Application app) {
        Object (application: app);
    }

    construct {
        title = "Corral";
        var app_settings = AppSettings.get_default ();
        default_width = app_settings.window_width;
        default_height = app_settings.window_height;

        ActionEntry[] entries = {
            { "preferences", on_preferences },
            { "about", on_about },
            { "font-increase", on_font_increase },
            { "font-decrease", on_font_decrease },
            { "fullscreen", on_fullscreen },
            { "reconnect", on_reconnect },
        };
        add_action_entries (entries, this);

        try {
            var builder = new Gtk.Builder.from_resource ("/dev/corral/Corral/gtk/help-overlay.ui");
            set_help_overlay (builder.get_object ("help_overlay") as Gtk.ShortcutsWindow);
        } catch (Error e) {
            warning ("Could not load shortcuts overlay: %s", e.message);
        }

        var header_menu = new Menu ();
        header_menu.append ("Reconnect", "win.reconnect");
        header_menu.append ("Preferences", "win.preferences");
        header_menu.append ("Keyboard Shortcuts", "win.show-help-overlay");
        header_menu.append ("About Corral", "win.about");
        var menu_btn = new Gtk.MenuButton ();
        menu_btn.icon_name = "open-menu-symbolic";
        menu_btn.tooltip_text = "Main menu";
        menu_btn.menu_model = header_menu;
        menu_btn.add_css_class ("flat");

        var header = new Adw.HeaderBar ();
        header.title_widget = new Adw.WindowTitle ("Corral", "Herdr");
        header.pack_end (menu_btn);

        toolbar_view = new Adw.ToolbarView ();
        toolbar_view.add_top_bar (header);
        content = toolbar_view;
        bind_property ("fullscreened", toolbar_view, "reveal-top-bars",
                       BindingFlags.SYNC_CREATE | BindingFlags.INVERT_BOOLEAN);

        var host_keys = new Gtk.ShortcutController ();
        host_keys.propagation_phase = Gtk.PropagationPhase.BUBBLE;
        host_keys.add_shortcut (new Gtk.Shortcut (
            new Gtk.KeyvalTrigger (Gdk.Key.q, Gdk.ModifierType.CONTROL_MASK),
            new Gtk.NamedAction ("app.quit")));
        host_keys.add_shortcut (new Gtk.Shortcut (
            new Gtk.KeyvalTrigger (Gdk.Key.comma, Gdk.ModifierType.CONTROL_MASK),
            new Gtk.NamedAction ("win.preferences")));
        host_keys.add_shortcut (new Gtk.Shortcut (
            new Gtk.KeyvalTrigger (Gdk.Key.plus, Gdk.ModifierType.CONTROL_MASK),
            new Gtk.NamedAction ("win.font-increase")));
        host_keys.add_shortcut (new Gtk.Shortcut (
            new Gtk.KeyvalTrigger (Gdk.Key.equal, Gdk.ModifierType.CONTROL_MASK),
            new Gtk.NamedAction ("win.font-increase")));
        host_keys.add_shortcut (new Gtk.Shortcut (
            new Gtk.KeyvalTrigger (Gdk.Key.minus, Gdk.ModifierType.CONTROL_MASK),
            new Gtk.NamedAction ("win.font-decrease")));
        host_keys.add_shortcut (new Gtk.Shortcut (
            new Gtk.KeyvalTrigger (Gdk.Key.question, Gdk.ModifierType.CONTROL_MASK),
            new Gtk.NamedAction ("win.show-help-overlay")));
        host_keys.add_shortcut (new Gtk.Shortcut (
            new Gtk.KeyvalTrigger (Gdk.Key.F11, 0),
            new Gtk.NamedAction ("win.fullscreen")));
        add_controller (host_keys);

        close_request.connect (() => {
            app_settings.window_width = get_width ();
            app_settings.window_height = get_height ();
            return false;
        });
        if (app_settings.settings != null) {
            app_settings.settings.changed["font-size"].connect (() => {
                if (terminal != null) {
                    terminal.font_size = app_settings.font_size;
                }
            });
        }

        show_session ();
    }

    public static string? herdr_path () {
        return Environment.find_program_in_path ("herdr");
    }

    private void show_session () {
        var path = herdr_path ();
        if (path == null) {
            show_missing_herdr ();
            return;
        }
        attach_herdr (path);
    }

    private void show_missing_herdr () {
        terminal = null;
        status_page = new Adw.StatusPage ();
        status_page.icon_name = "dialog-warning-symbolic";
        status_page.title = "Herdr is not installed";
        status_page.description = "Corral is a window for Herdr. Install Herdr, then reconnect.\n\ncurl -fsSL https://herdr.dev/install.sh | sh";
        var retry = new Gtk.Button.with_label ("Look for Herdr again");
        retry.add_css_class ("suggested-action");
        retry.halign = Gtk.Align.CENTER;
        retry.clicked.connect (show_session);
        status_page.child = retry;
        toolbar_view.content = status_page;
        title = "Corral";
    }

    private void show_ended (int status) {
        terminal = null;
        status_page = new Adw.StatusPage ();
        status_page.icon_name = "utilities-terminal-symbolic";
        status_page.title = "Session ended";
        status_page.description = "Herdr exited (%d). The server is still running. Reconnect to attach again.".printf (status);
        var retry = new Gtk.Button.with_label ("Reconnect");
        retry.add_css_class ("suggested-action");
        retry.halign = Gtk.Align.CENTER;
        retry.clicked.connect (show_session);
        status_page.child = retry;
        toolbar_view.content = status_page;
        title = "Corral";
    }

    private void show_error (string message) {
        terminal = null;
        status_page = new Adw.StatusPage ();
        status_page.icon_name = "dialog-error-symbolic";
        status_page.title = "Could not start the terminal";
        status_page.description = message;
        var retry = new Gtk.Button.with_label ("Try again");
        retry.add_css_class ("suggested-action");
        retry.halign = Gtk.Align.CENTER;
        retry.clicked.connect (show_session);
        status_page.child = retry;
        toolbar_view.content = status_page;
    }

    private void attach_herdr (string path) {
        if (attaching) {
            return;
        }
        attaching = true;
        status_page = null;
        terminal = new Ghostty ();
        terminal.command = path;
        terminal.cwd = Environment.get_current_dir ();
        terminal.font_size = AppSettings.get_default ().font_size;
        terminal.title_changed.connect ((term_title) => {
            Idle.add (() => {
                title = term_title != "" ? term_title : "Corral";
                return Source.REMOVE;
            });
        });
        terminal.exited.connect ((status) => {
            Idle.add (() => {
                show_ended (status);
                return Source.REMOVE;
            });
        });
        toolbar_view.content = terminal;
        var term = terminal;
        Idle.add (() => {
            if (terminal != term) {
                return Source.REMOVE;
            }
            try {
                term.start ();
            } catch (Error e) {
                show_error (e.message);
            }
            return Source.REMOVE;
        });
        attaching = false;
    }

    private void on_reconnect (SimpleAction? action, Variant? parameter) {
        show_session ();
    }

    private void on_preferences (SimpleAction? action, Variant? parameter) {
        var prefs = new Preferences ();
        prefs.present (this);
    }

    private void on_about (SimpleAction? action, Variant? parameter) {
        var about = new Adw.AboutDialog ();
        about.application_name = "Corral";
        about.application_icon = "dev.corral.Corral";
        about.developer_name = "Corral contributors";
        about.version = Config.VERSION;
        about.comments = "A GTK window that runs Herdr inside Ghostty. Close the window to detach; the Herdr server keeps running.";
        about.developers = { "Corral contributors" };
        about.copyright = "© 2026 Corral contributors";
        about.license_type = Gtk.License.MIT_X11;
        about.website = "https://github.com/papodaca/corral";
        about.present (this);
    }

    private void on_font_increase (SimpleAction? action, Variant? parameter) {
        AppSettings.get_default ().bump_font (1);
    }

    private void on_font_decrease (SimpleAction? action, Variant? parameter) {
        AppSettings.get_default ().bump_font (-1);
    }

    private void on_fullscreen (SimpleAction? action, Variant? parameter) {
        if (fullscreened) {
            unfullscreen ();
        } else {
            fullscreen ();
        }
    }
}

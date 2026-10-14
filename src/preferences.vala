public class Corral.Preferences : Adw.PreferencesDialog {
    private Gtk.FontDialogButton font_button;
    private Gtk.Button clear_font;
    private bool pushing_family;

    public Preferences () {
        title = "Preferences";

        var app_settings = AppSettings.get_default ();

        var page = new Adw.PreferencesPage ();
        page.title = "Terminal";
        page.icon_name = "utilities-terminal-symbolic";

        var group = new Adw.PreferencesGroup ();

        var font_dialog = new Gtk.FontDialog ();
        font_dialog.title = "Terminal font";
        font_dialog.modal = true;
        font_button = new Gtk.FontDialogButton (font_dialog);
        font_button.level = Gtk.FontLevel.FAMILY;
        font_button.use_font = true;
        font_button.use_size = false;
        font_button.valign = Gtk.Align.CENTER;

        clear_font = new Gtk.Button.with_label ("Default");
        clear_font.tooltip_text = "Use Fira Code, then the desktop monospace font";
        clear_font.valign = Gtk.Align.CENTER;

        var family_row = new Adw.ActionRow ();
        family_row.title = "Font";
        family_row.subtitle = "None uses Fira Code, then the desktop monospace font";
        family_row.add_suffix (font_button);
        family_row.add_suffix (clear_font);
        family_row.activatable_widget = font_button;

        show_family (app_settings.font_family);
        font_button.notify["font-desc"].connect (() => {
            if (pushing_family) {
                return;
            }
            var desc = font_button.font_desc;
            var family = desc != null ? desc.get_family () : null;
            app_settings.font_family = family ?? "";
        });
        clear_font.clicked.connect (() => {
            app_settings.font_family = "";
        });
        if (app_settings.settings != null) {
            app_settings.settings.changed["font-family"].connect (() => {
                var family = app_settings.font_family;
                var shown = font_button.font_desc;
                var shown_family = shown != null ? shown.get_family () : null;
                if ((shown_family ?? "") != family) {
                    show_family (family);
                } else {
                    clear_font.sensitive = family != "";
                }
            });
        }

        var font_row = new Adw.SpinRow.with_range (8, 32, 1);
        font_row.title = "Font size";
        font_row.subtitle = "Ctrl+Plus and Ctrl+Minus also change this";
        font_row.digits = 0;
        font_row.value = app_settings.font_size;
        font_row.notify["value"].connect (() => {
            app_settings.font_size = (int) font_row.value;
        });
        if (app_settings.settings != null) {
            app_settings.settings.changed["font-size"].connect (() => {
                if ((int) font_row.value != app_settings.font_size) {
                    font_row.value = app_settings.font_size;
                }
            });
        }

        group.add (family_row);
        group.add (font_row);
        page.add (group);
        add (page);
    }

    private void show_family (string family) {
        pushing_family = true;
        if (family == "") {
            var empty = Value (typeof (Pango.FontDescription));
            font_button.set_property ("font-desc", empty);
        } else {
            var desc = new Pango.FontDescription ();
            desc.set_family (family);
            font_button.font_desc = desc;
        }
        clear_font.sensitive = family != "";
        pushing_family = false;
    }
}

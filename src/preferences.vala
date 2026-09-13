public class Corral.Preferences : Adw.PreferencesDialog {
    public Preferences () {
        title = "Preferences";

        var app_settings = AppSettings.get_default ();

        var page = new Adw.PreferencesPage ();
        page.title = "Terminal";
        page.icon_name = "utilities-terminal-symbolic";

        var group = new Adw.PreferencesGroup ();
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

        group.add (font_row);
        page.add (group);
        add (page);
    }
}

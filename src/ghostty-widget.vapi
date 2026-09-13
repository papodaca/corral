[CCode (cheader_filename = "ghostty-widget.h")]
namespace Corral {
    [CCode (cname = "CorralGhostty", type_id = "corral_ghostty_get_type ()")]
    public class Ghostty : Gtk.GLArea {
        [CCode (cname = "corral_ghostty_new")]
        public Ghostty ();

        public string command { get; set; }
        public string? cwd { get; set; }
        public float font_size { get; set; }

        public bool start () throws GLib.Error;

        public signal void exited (int status);
        public signal void title_changed (string title);
    }
}

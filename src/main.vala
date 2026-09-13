int main (string[] args) {
    // Ghostty GTK 4.16+: disable GLES and Vulkan so GtkGLArea uses desktop GL.
    Environment.set_variable ("GDK_DISABLE", "gles-api,vulkan", true);
    if (Environment.get_variable ("GHOSTTY_RESOURCES_DIR") == null) {
        Environment.set_variable ("GHOSTTY_RESOURCES_DIR", Corral.Config.GHOSTTY_RESOURCES_DIR, false);
    }
    var app = new Corral.Application ();
    return app.run (args);
}

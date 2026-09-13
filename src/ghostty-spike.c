#include "ghostty-widget.h"

#include <gtk/gtk.h>

static void
on_term_realize (GtkWidget *term, gpointer user_data)
{
    (void) user_data;
    GError *error = NULL;
    if (!corral_ghostty_start (CORRAL_GHOSTTY (term), &error)) {
        g_warning ("start failed: %s", error != NULL ? error->message : "unknown");
        g_clear_error (&error);
    }
}

static void
on_activate (GtkApplication *app, gpointer user_data)
{
    (void) user_data;
    GtkWidget *window = gtk_application_window_new (app);
    gtk_window_set_title (GTK_WINDOW (window), "ghostty-spike");
    gtk_window_set_default_size (GTK_WINDOW (window), 800, 500);

    GtkWidget *term = GTK_WIDGET (corral_ghostty_new ());
    corral_ghostty_set_command (CORRAL_GHOSTTY (term), "exec /bin/sh");
    gtk_window_set_child (GTK_WINDOW (window), term);
    g_signal_connect (term, "realize", G_CALLBACK (on_term_realize), NULL);
    gtk_window_present (GTK_WINDOW (window));
}

int
main (int argc, char **argv)
{
    g_setenv ("GDK_DISABLE", "gles-api,vulkan", TRUE);
    GtkApplication *app = gtk_application_new (
        "dev.corral.GhosttySpike",
        G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect (app, "activate", G_CALLBACK (on_activate), NULL);
    int status = g_application_run (G_APPLICATION (app), argc, argv);
    g_object_unref (app);
    return status;
}

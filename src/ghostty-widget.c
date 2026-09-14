#include "ghostty-widget.h"

#include <ghostty.h>

#include <epoxy/gl.h>
#include <gdk/gdk.h>
#include <glib/gstdio.h>
#include <string.h>
#include <unistd.h>

struct _CorralGhostty {
    GtkGLArea parent_instance;

    char *command;
    char *cwd;
    float font_size;
    char *config_overlay;

    ghostty_config_t config;
    ghostty_app_t app;
    ghostty_surface_t surface;
    gboolean started;
    gboolean precision_scroll;
    gboolean gpu_dropped;
    gint tick_queued;
    guint tick_source;
};

enum {
    PROP_0,
    PROP_COMMAND,
    PROP_CWD,
    PROP_FONT_SIZE,
    N_PROPS
};

enum {
    SIGNAL_EXITED,
    SIGNAL_TITLE_CHANGED,
    N_SIGNALS
};

static GParamSpec *properties[N_PROPS];
static guint signals[N_SIGNALS];
static gboolean ghostty_global_inited = FALSE;

G_DEFINE_FINAL_TYPE (CorralGhostty, corral_ghostty, GTK_TYPE_GL_AREA)

static CorralGhostty *
surface_owner (ghostty_surface_t surface)
{
    if (surface == NULL) {
        return NULL;
    }
    return CORRAL_GHOSTTY (ghostty_surface_userdata (surface));
}

static gboolean
on_tick_idle (gpointer data)
{
    CorralGhostty *self = CORRAL_GHOSTTY (data);
    g_atomic_int_set (&self->tick_queued, 0);
    g_atomic_int_set ((gint *) &self->tick_source, 0);
    /* Tick drains mailboxes and may emit GHOSTTY_ACTION_RENDER, which
     * queues a GL frame. Do not draw here: Ghostty GTK, macOS, and
     * Ghostling all keep I/O/tick off the paint path. */
    if (self->app != NULL) {
        ghostty_app_tick (self->app);
    }
    return G_SOURCE_REMOVE;
}

static void
queue_tick (CorralGhostty *self)
{
    /* wakeup_cb can run on Ghostty's IO/renderer threads. Always defer
     * to the GTK loop; never tick inline (that re-enters from tick). */
    if (!g_atomic_int_compare_and_exchange (&self->tick_queued, 0, 1)) {
        return;
    }
    guint id = g_idle_add_full (G_PRIORITY_DEFAULT,
                                on_tick_idle,
                                g_object_ref (self),
                                g_object_unref);
    g_atomic_int_set ((gint *) &self->tick_source, (gint) id);
}

static void
wakeup_cb (void *userdata)
{
    if (userdata != NULL) {
        queue_tick (CORRAL_GHOSTTY (userdata));
    }
}

static void
set_cursor_from_shape (GtkWidget *widget, ghostty_action_mouse_shape_e shape)
{
    const char *name = "text";
    switch (shape) {
    case GHOSTTY_MOUSE_SHAPE_POINTER:
        name = "pointer";
        break;
    case GHOSTTY_MOUSE_SHAPE_CONTEXT_MENU:
        name = "context-menu";
        break;
    case GHOSTTY_MOUSE_SHAPE_CROSSHAIR:
        name = "crosshair";
        break;
    case GHOSTTY_MOUSE_SHAPE_WAIT:
        name = "wait";
        break;
    case GHOSTTY_MOUSE_SHAPE_PROGRESS:
        name = "progress";
        break;
    case GHOSTTY_MOUSE_SHAPE_DEFAULT:
        name = "default";
        break;
    default:
        name = "text";
        break;
    }
    gtk_widget_set_cursor_from_name (widget, name);
}

static bool
action_cb (ghostty_app_t app, ghostty_target_s target, ghostty_action_s action)
{
    (void) app;
    CorralGhostty *self = NULL;
    if (target.tag == GHOSTTY_TARGET_SURFACE) {
        self = surface_owner (target.target.surface);
    }

    switch (action.tag) {
    case GHOSTTY_ACTION_RENDER:
        if (self != NULL && !self->gpu_dropped) {
            gtk_gl_area_queue_render (GTK_GL_AREA (self));
        }
        return true;
    case GHOSTTY_ACTION_SET_TITLE:
        if (self != NULL && action.action.set_title.title != NULL) {
            g_signal_emit (self, signals[SIGNAL_TITLE_CHANGED], 0,
                           action.action.set_title.title);
        }
        return true;
    case GHOSTTY_ACTION_SHOW_CHILD_EXITED:
        if (self != NULL) {
            g_signal_emit (self, signals[SIGNAL_EXITED], 0,
                           (int) action.action.child_exited.exit_code);
        }
        return false;
    case GHOSTTY_ACTION_OPEN_URL:
        if (action.action.open_url.url != NULL) {
            g_app_info_launch_default_for_uri_async (
                action.action.open_url.url, NULL, NULL, NULL, NULL);
        }
        return true;
    case GHOSTTY_ACTION_MOUSE_SHAPE:
        if (self != NULL) {
            set_cursor_from_shape (GTK_WIDGET (self), action.action.mouse_shape);
        }
        return true;
    case GHOSTTY_ACTION_RING_BELL:
        return true;
    default:
        return false;
    }
}

typedef struct {
    ghostty_surface_t surface;
    void *request;
} ClipboardRead;

static void
on_clipboard_text (GObject *source, GAsyncResult *result, gpointer data)
{
    ClipboardRead *read = data;
    char *text = gdk_clipboard_read_text_finish (GDK_CLIPBOARD (source), result, NULL);
    ghostty_surface_complete_clipboard_request (
        read->surface,
        text != NULL ? text : "",
        read->request,
        FALSE);
    g_free (text);
    g_free (read);
}

static bool
read_clipboard_cb (void *surface_ud, ghostty_clipboard_e clipboard, void *request)
{
    CorralGhostty *self = CORRAL_GHOSTTY (surface_ud);
    GdkClipboard *clip = gtk_widget_get_clipboard (GTK_WIDGET (self));
    if (clipboard == GHOSTTY_CLIPBOARD_SELECTION) {
        clip = gtk_widget_get_primary_clipboard (GTK_WIDGET (self));
    }
    ClipboardRead *read = g_new0 (ClipboardRead, 1);
    read->surface = self->surface;
    read->request = request;
    gdk_clipboard_read_text_async (clip, NULL, on_clipboard_text, read);
    return true;
}

static void
confirm_read_clipboard_cb (void *surface_ud,
                           const char *text,
                           void *request,
                           ghostty_clipboard_request_e kind)
{
    (void) kind;
    CorralGhostty *self = CORRAL_GHOSTTY (surface_ud);
    ghostty_surface_complete_clipboard_request (
        self->surface,
        text != NULL ? text : "",
        request,
        TRUE);
}

static void
write_clipboard_cb (void *surface_ud,
                    ghostty_clipboard_e clipboard,
                    const ghostty_clipboard_content_s *contents,
                    size_t contents_len,
                    bool confirm)
{
    (void) confirm;
    CorralGhostty *self = CORRAL_GHOSTTY (surface_ud);
    GdkClipboard *clip = gtk_widget_get_clipboard (GTK_WIDGET (self));
    if (clipboard == GHOSTTY_CLIPBOARD_SELECTION) {
        clip = gtk_widget_get_primary_clipboard (GTK_WIDGET (self));
    }
    const char *text = "";
    for (size_t i = 0; i < contents_len; i++) {
        if (contents[i].mime != NULL &&
            g_strcmp0 (contents[i].mime, "text/plain") == 0 &&
            contents[i].data != NULL) {
            text = contents[i].data;
            break;
        }
    }
    if (text[0] == '\0' && contents_len > 0 && contents != NULL &&
        contents[0].data != NULL) {
        text = contents[0].data;
    }
    gdk_clipboard_set_text (clip, text);
}

static void
close_surface_cb (void *surface_ud, bool process_alive)
{
    /* Closing the GTK window here made spike/sh vanish while the PTY was still
     * usable. Corral handles child exit via SHOW_CHILD_EXITED. */
    (void) surface_ud;
    (void) process_alive;
}

static ghostty_input_mods_e
mods_from_gdk (GdkModifierType state)
{
    ghostty_input_mods_e mods = GHOSTTY_MODS_NONE;
    if (state & GDK_SHIFT_MASK) {
        mods |= GHOSTTY_MODS_SHIFT;
    }
    if (state & GDK_CONTROL_MASK) {
        mods |= GHOSTTY_MODS_CTRL;
    }
    if (state & GDK_ALT_MASK) {
        mods |= GHOSTTY_MODS_ALT;
    }
    if (state & GDK_SUPER_MASK) {
        mods |= GHOSTTY_MODS_SUPER;
    }
    if (state & GDK_LOCK_MASK) {
        mods |= GHOSTTY_MODS_CAPS;
    }
    return mods;
}

static GdkEvent *
current_event (GtkEventController *controller)
{
    return gtk_event_controller_get_current_event (controller);
}

static GdkEvent *
current_key_event (GtkEventController *controller)
{
    GdkEvent *event = current_event (controller);
    if (event == NULL) {
        return NULL;
    }
    GdkEventType type = gdk_event_get_event_type (event);
    if (type != GDK_KEY_PRESS && type != GDK_KEY_RELEASE) {
        return NULL;
    }
    return event;
}

static ghostty_input_mods_e
mods_from_controller (GtkEventController *controller, GdkModifierType state)
{
    GdkEvent *event = current_event (controller);
    GdkDevice *device = NULL;
    if (event != NULL) {
        /* Event state can drop Control/Alt that XKB marked consumed.
         * The device still has them, which is what Ghostty GTK uses. */
        state = gdk_event_get_modifier_state (event);
        device = gdk_event_get_device (event);
        if (device != NULL) {
            state |= gdk_device_get_modifier_state (device);
        }
    }
    ghostty_input_mods_e mods = mods_from_gdk (state);
    if (device != NULL && gdk_device_get_num_lock_state (device)) {
        mods |= GHOSTTY_MODS_NUM;
    }
    return mods;
}

static ghostty_input_mods_e
consumed_mods_from_controller (GtkEventController *controller)
{
    GdkEvent *event = current_key_event (controller);
    if (event == NULL) {
        return GHOSTTY_MODS_NONE;
    }
    return mods_from_gdk (gdk_key_event_get_consumed_modifiers (event));
}

static ghostty_input_mouse_button_e
mouse_button_from_gdk (guint button)
{
    switch (button) {
    case GDK_BUTTON_PRIMARY:
        return GHOSTTY_MOUSE_LEFT;
    case GDK_BUTTON_MIDDLE:
        return GHOSTTY_MOUSE_MIDDLE;
    case GDK_BUTTON_SECONDARY:
        return GHOSTTY_MOUSE_RIGHT;
    case 4:
        return GHOSTTY_MOUSE_FOUR;
    case 5:
        return GHOSTTY_MOUSE_FIVE;
    case 6:
        return GHOSTTY_MOUSE_SIX;
    case 7:
        return GHOSTTY_MOUSE_SEVEN;
    case 8:
        return GHOSTTY_MOUSE_EIGHT;
    case 9:
        return GHOSTTY_MOUSE_NINE;
    case 10:
        return GHOSTTY_MOUSE_TEN;
    case 11:
        return GHOSTTY_MOUSE_ELEVEN;
    default:
        return GHOSTTY_MOUSE_UNKNOWN;
    }
}

static uint32_t
unshifted_codepoint (GtkWidget *widget,
                     GtkEventController *controller,
                     guint keycode)
{
    GdkEvent *event = current_key_event (controller);
    if (event == NULL) {
        return 0;
    }
    GdkDisplay *display = gtk_widget_get_display (widget);
    guint layout = gdk_key_event_get_layout (event);
    GdkKeymapKey *keys = NULL;
    guint *keyvals = NULL;
    int n_entries = 0;
    if (!gdk_display_map_keycode (display, keycode, &keys, &keyvals, &n_entries)) {
        return 0;
    }
    uint32_t result = 0;
    for (int i = 0; i < n_entries; i++) {
        if (keys[i].group == (int) layout && keys[i].level == 0) {
            result = gdk_keyval_to_unicode (keyvals[i]);
            break;
        }
    }
    g_free (keys);
    g_free (keyvals);
    return result;
}

static uint32_t
unshifted_or_lower (GtkWidget *widget,
                    GtkEventController *controller,
                    guint keyval,
                    guint keycode)
{
    uint32_t unshifted = unshifted_codepoint (widget, controller, keycode);
    if (unshifted != 0) {
        return unshifted;
    }
    return gdk_keyval_to_unicode (gdk_keyval_to_lower (keyval));
}

static gboolean
send_key (CorralGhostty *self,
          GtkEventControllerKey *controller,
          ghostty_input_action_e action,
          guint keyval,
          guint keycode,
          GdkModifierType state)
{
    if (self->surface == NULL) {
        return FALSE;
    }

    GtkEventController *ec = GTK_EVENT_CONTROLLER (controller);
    gunichar ch = gdk_keyval_to_unicode (keyval);
    char text[8] = { 0 };
    /* Control bytes belong to the encoder. Printable utf8 is still
     * passed, including with Ctrl held, matching Ghostty GTK. */
    if (action == GHOSTTY_ACTION_PRESS && ch != 0 && ch >= 0x20) {
        g_unichar_to_utf8 (ch, text);
    }

    ghostty_input_key_s event = {
        .action = action,
        .mods = mods_from_controller (ec, state),
        .consumed_mods = consumed_mods_from_controller (ec),
        .keycode = keycode,
        .text = text[0] != 0 ? text : NULL,
        .unshifted_codepoint = unshifted_or_lower (GTK_WIDGET (self), ec, keyval, keycode),
        .composing = false,
    };
    return ghostty_surface_key (self->surface, event);
}

static gboolean
on_key_pressed (GtkEventControllerKey *controller,
                guint keyval,
                guint keycode,
                GdkModifierType state,
                gpointer user_data)
{
    return send_key (CORRAL_GHOSTTY (user_data), controller,
                     GHOSTTY_ACTION_PRESS, keyval, keycode, state);
}

static gboolean
on_key_released (GtkEventControllerKey *controller,
                 guint keyval,
                 guint keycode,
                 GdkModifierType state,
                 gpointer user_data)
{
    return send_key (CORRAL_GHOSTTY (user_data), controller,
                     GHOSTTY_ACTION_RELEASE, keyval, keycode, state);
}

static void
on_motion (GtkEventControllerMotion *controller,
           double x,
           double y,
           gpointer user_data)
{
    CorralGhostty *self = CORRAL_GHOSTTY (user_data);
    if (self->surface == NULL) {
        return;
    }
    GtkEventController *ec = GTK_EVENT_CONTROLLER (controller);
    GdkModifierType state = gtk_event_controller_get_current_event_state (ec);
    ghostty_surface_mouse_pos (self->surface, x, y,
                               (int) mods_from_controller (ec, state));
}

static void
on_leave (GtkEventControllerMotion *controller, gpointer user_data)
{
    CorralGhostty *self = CORRAL_GHOSTTY (user_data);
    if (self->surface == NULL) {
        return;
    }
    GtkEventController *ec = GTK_EVENT_CONTROLLER (controller);
    GdkModifierType state = gtk_event_controller_get_current_event_state (ec);
    ghostty_surface_mouse_pos (self->surface, -1, -1,
                               (int) mods_from_controller (ec, state));
}

static void
send_click (CorralGhostty *self,
            GtkGestureClick *gesture,
            double x,
            double y,
            ghostty_input_mouse_state_e action)
{
    if (self->surface == NULL) {
        return;
    }
    GtkEventController *ec = GTK_EVENT_CONTROLLER (gesture);
    guint button = gtk_gesture_single_get_current_button (GTK_GESTURE_SINGLE (gesture));
    GdkModifierType state = gtk_event_controller_get_current_event_state (ec);
    ghostty_input_mods_e mods = mods_from_controller (ec, state);
    ghostty_surface_mouse_pos (self->surface, x, y, (int) mods);
    ghostty_surface_mouse_button (self->surface, action,
                                  mouse_button_from_gdk (button), (int) mods);
}

static void
on_click_pressed (GtkGestureClick *gesture,
                  gint n_press,
                  double x,
                  double y,
                  gpointer user_data)
{
    (void) n_press;
    gtk_widget_grab_focus (GTK_WIDGET (user_data));
    send_click (CORRAL_GHOSTTY (user_data), gesture, x, y, GHOSTTY_MOUSE_PRESS);
}

static void
on_click_released (GtkGestureClick *gesture,
                   gint n_press,
                   double x,
                   double y,
                   gpointer user_data)
{
    (void) n_press;
    send_click (CORRAL_GHOSTTY (user_data), gesture, x, y, GHOSTTY_MOUSE_RELEASE);
}

static void
on_scroll_begin (GtkEventControllerScroll *controller, gpointer user_data)
{
    (void) controller;
    CORRAL_GHOSTTY (user_data)->precision_scroll = TRUE;
}

static void
on_scroll_end (GtkEventControllerScroll *controller, gpointer user_data)
{
    (void) controller;
    CORRAL_GHOSTTY (user_data)->precision_scroll = FALSE;
}

static gboolean
on_scroll (GtkEventControllerScroll *controller,
           double dx,
           double dy,
           gpointer user_data)
{
    CorralGhostty *self = CORRAL_GHOSTTY (user_data);
    (void) controller;
    if (self->surface == NULL) {
        return FALSE;
    }
    /* Ghostty: negative is down/left. GTK's dy is the other way, so invert
     * like Ghostty's own GTK surface. */
    double scale = gtk_widget_get_scale_factor (GTK_WIDGET (self));
    double multiplier = self->precision_scroll ? 10.0 : 1.0;
    int scroll_mods = self->precision_scroll ? 1 : 0;
    ghostty_surface_mouse_scroll (
        self->surface,
        dx * -1.0 * scale * multiplier,
        dy * -1.0 * scale * multiplier,
        scroll_mods);
    return TRUE;
}

static void
on_focus_enter (GtkEventControllerFocus *controller, gpointer user_data)
{
    CorralGhostty *self = CORRAL_GHOSTTY (user_data);
    (void) controller;
    if (self->surface != NULL) {
        ghostty_surface_set_focus (self->surface, true);
    }
}

static void
on_focus_leave (GtkEventControllerFocus *controller, gpointer user_data)
{
    CorralGhostty *self = CORRAL_GHOSTTY (user_data);
    (void) controller;
    if (self->surface != NULL) {
        ghostty_surface_set_focus (self->surface, false);
    }
}

static void
cancel_tick (CorralGhostty *self)
{
    guint tick = (guint) g_atomic_int_get ((gint *) &self->tick_source);
    if (tick != 0) {
        g_source_remove (tick);
        g_atomic_int_set ((gint *) &self->tick_source, 0);
    }
    g_atomic_int_set (&self->tick_queued, 0);
}

static void
destroy_surface (CorralGhostty *self)
{
    cancel_tick (self);
    if (self->surface != NULL) {
        ghostty_surface_free (self->surface);
        self->surface = NULL;
    }
    if (self->app != NULL) {
        ghostty_app_free (self->app);
        self->app = NULL;
    }
    if (self->config != NULL) {
        ghostty_config_free (self->config);
        self->config = NULL;
    }
    self->started = FALSE;
    self->gpu_dropped = FALSE;
}

static void
strip_nested_herdr_env (void)
{
    /* Herdr refuses to start when HERDR_ENV=1 (already inside a Herdr pane).
     * Corral is a host window, not a nested client, so drop pane identity.
     * Keep socket/config so we still attach to a running server. */
    gchar **keys = g_listenv ();
    for (gchar **k = keys; k != NULL && *k != NULL; k++) {
        if (!g_str_has_prefix (*k, "HERDR_")) {
            continue;
        }
        if (g_strcmp0 (*k, "HERDR_SOCKET_PATH") == 0 ||
            g_strcmp0 (*k, "HERDR_CONFIG_PATH") == 0) {
            continue;
        }
        g_unsetenv (*k);
    }
    g_strfreev (keys);
}

static gboolean
font_family_resolves (GtkWidget *widget, const char *wanted)
{
    PangoContext *ctx = gtk_widget_get_pango_context (widget);
    PangoFontMap *map = pango_context_get_font_map (ctx);
    PangoFontDescription *desc = pango_font_description_new ();
    pango_font_description_set_family (desc, wanted);
    pango_font_description_set_size (desc, 12 * PANGO_SCALE);
    PangoFont *font = pango_font_map_load_font (map, ctx, desc);
    pango_font_description_free (desc);
    if (font == NULL) {
        return FALSE;
    }
    PangoFontDescription *got = pango_font_describe (font);
    const char *family = pango_font_description_get_family (got);
    gboolean ok = family != NULL && g_ascii_strcasecmp (family, wanted) == 0;
    pango_font_description_free (got);
    g_object_unref (font);
    return ok;
}

static void
append_font_family (GString *body, GtkWidget *widget, GHashTable *seen, const char *family)
{
    if (family == NULL || family[0] == '\0') {
        return;
    }
    char *key = g_ascii_strdown (family, -1);
    if (g_hash_table_contains (seen, key)) {
        g_free (key);
        return;
    }
    if (!font_family_resolves (widget, family)) {
        g_free (key);
        return;
    }
    g_hash_table_add (seen, key);
    g_string_append (body, "font-family = ");
    g_string_append (body, family);
    g_string_append_c (body, '\n');
}

static char *
desktop_monospace_family (void)
{
    GSettingsSchemaSource *source = g_settings_schema_source_get_default ();
    if (source == NULL) {
        return NULL;
    }
    GSettingsSchema *schema = g_settings_schema_source_lookup (
        source, "org.gnome.desktop.interface", TRUE);
    if (schema == NULL) {
        return NULL;
    }
    if (!g_settings_schema_has_key (schema, "monospace-font-name")) {
        g_settings_schema_unref (schema);
        return NULL;
    }
    g_settings_schema_unref (schema);
    GSettings *settings = g_settings_new ("org.gnome.desktop.interface");
    char *value = g_settings_get_string (settings, "monospace-font-name");
    g_object_unref (settings);
    if (value == NULL || value[0] == '\0') {
        g_free (value);
        return NULL;
    }
    PangoFontDescription *desc = pango_font_description_from_string (value);
    g_free (value);
    if (desc == NULL) {
        return NULL;
    }
    const char *family = pango_font_description_get_family (desc);
    char *copy = family != NULL ? g_strdup (family) : NULL;
    pango_font_description_free (desc);
    return copy;
}

static gboolean
write_font_overlay (CorralGhostty *self, GError **error)
{
    GString *body = g_string_new ("# Corral forces a real monospace family.\n");
    g_string_append (body, "font-family =\n");
    GHashTable *seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

    append_font_family (body, GTK_WIDGET (self), seen, "Fira Code");
    char *desktop = desktop_monospace_family ();
    append_font_family (body, GTK_WIDGET (self), seen, desktop);
    g_free (desktop);

    static const char *candidates[] = {
        "Adwaita Mono",
        "Noto Sans Mono",
        "DejaVu Sans Mono",
        "Liberation Mono",
        "Source Code Pro",
        "JetBrains Mono",
        NULL,
    };
    for (const char **family = candidates; *family != NULL; family++) {
        append_font_family (body, GTK_WIDGET (self), seen, *family);
    }
    g_hash_table_unref (seen);
    g_string_append (body, "font-family = monospace\n");
    g_string_append_printf (body, "font-size = %d\n", (int) (self->font_size + 0.5f));
    /* Herdr owns multiplexer chords (prefix+c, and so on). Ghostty's
     * default binds would eat ctrl+shift+t, alt+1, … before they reach
     * the PTY. Keep clipboard chords so the host window can still copy. */
    g_string_append (body, "keybind = clear\n");
    g_string_append (body, "keybind = copy=copy_to_clipboard\n");
    g_string_append (body, "keybind = paste=paste_from_clipboard\n");
    g_string_append (body, "keybind = ctrl+shift+c=copy_to_clipboard\n");
    g_string_append (body, "keybind = ctrl+shift+v=paste_from_clipboard\n");
    g_string_append (body, "keybind = shift+insert=paste_from_selection\n");
    g_string_append (body, "keybind = ctrl+insert=copy_to_clipboard\n");

    if (self->config_overlay == NULL) {
        int fd = g_file_open_tmp ("corral-ghostty-XXXXXX.config", &self->config_overlay, error);
        if (fd < 0) {
            g_string_free (body, TRUE);
            return FALSE;
        }
        close (fd);
    }

    gboolean ok = g_file_set_contents (self->config_overlay, body->str, (gssize) body->len, error);
    g_string_free (body, TRUE);
    return ok;
}

static void
load_embed_config (CorralGhostty *self)
{
    if (self->config != NULL) {
        ghostty_config_free (self->config);
        self->config = NULL;
    }
    self->config = ghostty_config_new ();
    ghostty_config_load_default_files (self->config);
    if (self->config_overlay != NULL) {
        ghostty_config_load_file (self->config, self->config_overlay);
    }
    ghostty_config_finalize (self->config);
}

static void
apply_font_size (CorralGhostty *self)
{
    if (self->surface == NULL || self->app == NULL) {
        return;
    }
    if (!write_font_overlay (self, NULL)) {
        return;
    }
    load_embed_config (self);
    ghostty_app_update_config (self->app, self->config);
    ghostty_surface_update_config (self->surface, self->config);
    gtk_gl_area_queue_render (GTK_GL_AREA (self));
}

static void
apply_surface_geometry (CorralGhostty *self)
{
    if (self->surface == NULL) {
        return;
    }
    int scale = gtk_widget_get_scale_factor (GTK_WIDGET (self));
    int width = gtk_widget_get_width (GTK_WIDGET (self)) * scale;
    int height = gtk_widget_get_height (GTK_WIDGET (self)) * scale;
    if (width < 1) {
        width = 800 * scale;
    }
    if (height < 1) {
        height = 600 * scale;
    }
    ghostty_surface_set_size (self->surface, (uint32_t) width, (uint32_t) height);
    ghostty_surface_set_content_scale (self->surface, scale, scale);
}

static gboolean
create_surface (CorralGhostty *self, GError **error)
{
    strip_nested_herdr_env ();
    gtk_gl_area_make_current (GTK_GL_AREA (self));
    if (gtk_gl_area_get_error (GTK_GL_AREA (self)) != NULL) {
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                             "OpenGL context is not available");
        return FALSE;
    }
    gtk_gl_area_attach_buffers (GTK_GL_AREA (self));

    if (!ghostty_global_inited) {
        char *argv0 = g_strdup ("corral");
        char *argv[] = { argv0, NULL };
        if (ghostty_init (1, argv) != 0) {
            g_free (argv0);
            g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                                 "ghostty_init failed");
            return FALSE;
        }
        g_free (argv0);
        ghostty_global_inited = TRUE;
    }

    if (!write_font_overlay (self, error)) {
        return FALSE;
    }
    load_embed_config (self);

    ghostty_runtime_config_s runtime = {
        .userdata = self,
        .supports_selection_clipboard = true,
        .wakeup_cb = wakeup_cb,
        .action_cb = action_cb,
        .read_clipboard_cb = read_clipboard_cb,
        .confirm_read_clipboard_cb = confirm_read_clipboard_cb,
        .write_clipboard_cb = write_clipboard_cb,
        .close_surface_cb = close_surface_cb,
    };
    self->app = ghostty_app_new (&runtime, self->config);
    if (self->app == NULL) {
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                             "ghostty_app_new failed");
        destroy_surface (self);
        return FALSE;
    }

    ghostty_surface_config_s surf = ghostty_surface_config_new ();
    surf.platform_tag = GHOSTTY_PLATFORM_GTK;
    surf.platform.gtk.gl_area = self;
    surf.userdata = self;
    surf.scale_factor = gtk_widget_get_scale_factor (GTK_WIDGET (self));
    surf.font_size = self->font_size;
    surf.working_directory = self->cwd;
    surf.command = self->command;
    surf.wait_after_command = true;

    self->surface = ghostty_surface_new (self->app, &surf);
    if (self->surface == NULL) {
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                             "ghostty_surface_new failed");
        destroy_surface (self);
        return FALSE;
    }

    ghostty_surface_set_focus (self->surface, true);
    apply_surface_geometry (self);
    queue_tick (self);
    return TRUE;
}

static void
on_realize (GtkGLArea *area)
{
    CorralGhostty *self = CORRAL_GHOSTTY (area);
    gtk_gl_area_make_current (area);
    if (gtk_gl_area_get_error (area) != NULL) {
        return;
    }
    /* GtkGLArea can unrealize/realize without destroying the widget
     * (scale change, moving outputs). Ghostty GTK keeps the PTY and
     * only rebuilds GPU state. Freeing the surface here joined IO
     * threads from the GTK thread and froze the window. */
    if (self->surface != NULL && self->gpu_dropped) {
        ghostty_surface_display_realized (self->surface);
        self->gpu_dropped = FALSE;
        gtk_gl_area_queue_render (area);
    }
}

static void
on_unrealize (GtkGLArea *area)
{
    CorralGhostty *self = CORRAL_GHOSTTY (area);
    if (self->surface == NULL) {
        return;
    }
    gtk_gl_area_make_current (area);
    if (gtk_gl_area_get_error (area) != NULL) {
        g_warning ("GL context unavailable on unrealize; GPU resources may leak");
        return;
    }
    ghostty_surface_display_unrealized (self->surface);
    self->gpu_dropped = TRUE;
}

static void
restore_gtk_gl (GtkGLArea *area, GdkGLContext *context)
{
    if (context != NULL) {
        gdk_gl_context_make_current (context);
    } else {
        gtk_gl_area_make_current (area);
    }
    gtk_gl_area_attach_buffers (area);
}

static gboolean
ensure_surface (CorralGhostty *self)
{
    if (!self->started || self->surface != NULL) {
        return self->surface != NULL;
    }
    GError *error = NULL;
    if (!create_surface (self, &error)) {
        g_warning ("libghostty surface failed: %s",
                   error != NULL ? error->message : "unknown");
        g_clear_error (&error);
        return FALSE;
    }
    g_debug ("libghostty surface created");
    return TRUE;
}

static gboolean
on_render (GtkGLArea *area, GdkGLContext *context)
{
    CorralGhostty *self = CORRAL_GHOSTTY (area);
    int scale = gtk_widget_get_scale_factor (GTK_WIDGET (area));
    int width = gtk_widget_get_width (GTK_WIDGET (area)) * scale;
    int height = gtk_widget_get_height (GTK_WIDGET (area)) * scale;

    ensure_surface (self);
    apply_surface_geometry (self);

    if (width > 0 && height > 0) {
        glViewport (0, 0, width, height);
    }

    if (self->surface != NULL && !self->gpu_dropped) {
        ghostty_surface_draw (self->surface);
    }

    /* Ghostty/glad load libGL and can drop GDK's current context.
     * GtkGLArea::snapshot then calls epoxy (glFenceSync) and aborts. */
    restore_gtk_gl (area, context);
    return TRUE;
}

static void
on_resize (GtkGLArea *area, int width, int height)
{
    CorralGhostty *self = CORRAL_GHOSTTY (area);
    if (width < 1 || height < 1) {
        return;
    }

    ensure_surface (self);
    apply_surface_geometry (self);
}

static void
on_scale_factor (GObject *object, GParamSpec *pspec, gpointer user_data)
{
    (void) pspec;
    (void) user_data;
    apply_surface_geometry (CORRAL_GHOSTTY (object));
}

static void
corral_ghostty_get_property (GObject *object,
                             guint prop_id,
                             GValue *value,
                             GParamSpec *pspec)
{
    CorralGhostty *self = CORRAL_GHOSTTY (object);
    switch (prop_id) {
    case PROP_COMMAND:
        g_value_set_string (value, self->command);
        break;
    case PROP_CWD:
        g_value_set_string (value, self->cwd);
        break;
    case PROP_FONT_SIZE:
        g_value_set_float (value, self->font_size);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
        break;
    }
}

static void
corral_ghostty_set_property (GObject *object,
                             guint prop_id,
                             const GValue *value,
                             GParamSpec *pspec)
{
    CorralGhostty *self = CORRAL_GHOSTTY (object);
    switch (prop_id) {
    case PROP_COMMAND:
        g_free (self->command);
        self->command = g_value_dup_string (value);
        break;
    case PROP_CWD:
        g_free (self->cwd);
        self->cwd = g_value_dup_string (value);
        break;
    case PROP_FONT_SIZE:
        self->font_size = g_value_get_float (value);
        apply_font_size (self);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
        break;
    }
}

static void
corral_ghostty_dispose (GObject *object)
{
    CorralGhostty *self = CORRAL_GHOSTTY (object);
    destroy_surface (self);
    if (self->config_overlay != NULL) {
        g_unlink (self->config_overlay);
    }
    g_clear_pointer (&self->config_overlay, g_free);
    g_clear_pointer (&self->command, g_free);
    g_clear_pointer (&self->cwd, g_free);
    G_OBJECT_CLASS (corral_ghostty_parent_class)->dispose (object);
}

static void
corral_ghostty_class_init (CorralGhosttyClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);

    object_class->get_property = corral_ghostty_get_property;
    object_class->set_property = corral_ghostty_set_property;
    object_class->dispose = corral_ghostty_dispose;

    properties[PROP_COMMAND] = g_param_spec_string (
        "command", NULL, NULL, NULL,
        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
    properties[PROP_CWD] = g_param_spec_string (
        "cwd", NULL, NULL, NULL,
        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
    properties[PROP_FONT_SIZE] = g_param_spec_float (
        "font-size", NULL, NULL, 8.0, 48.0, 10.0,
        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
    g_object_class_install_properties (object_class, N_PROPS, properties);

    signals[SIGNAL_EXITED] = g_signal_new (
        "exited",
        G_TYPE_FROM_CLASS (klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 1, G_TYPE_INT);
    signals[SIGNAL_TITLE_CHANGED] = g_signal_new (
        "title-changed",
        G_TYPE_FROM_CLASS (klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void
corral_ghostty_init (CorralGhostty *self)
{
    self->font_size = 10.0f;
    self->precision_scroll = FALSE;
    self->gpu_dropped = FALSE;
    self->tick_queued = 0;
    self->tick_source = 0;
    gtk_gl_area_set_allowed_apis (GTK_GL_AREA (self), GDK_GL_API_GL);
    gtk_gl_area_set_has_depth_buffer (GTK_GL_AREA (self), FALSE);
    gtk_gl_area_set_has_stencil_buffer (GTK_GL_AREA (self), FALSE);
    gtk_gl_area_set_auto_render (GTK_GL_AREA (self), TRUE);
    gtk_widget_set_hexpand (GTK_WIDGET (self), TRUE);
    gtk_widget_set_vexpand (GTK_WIDGET (self), TRUE);
    gtk_widget_set_focusable (GTK_WIDGET (self), TRUE);
    gtk_widget_set_focus_on_click (GTK_WIDGET (self), TRUE);

    g_signal_connect (self, "realize", G_CALLBACK (on_realize), NULL);
    g_signal_connect (self, "unrealize", G_CALLBACK (on_unrealize), NULL);
    g_signal_connect (self, "render", G_CALLBACK (on_render), NULL);
    g_signal_connect (self, "resize", G_CALLBACK (on_resize), NULL);
    g_signal_connect (self, "notify::scale-factor", G_CALLBACK (on_scale_factor), NULL);

    GtkEventController *keys = GTK_EVENT_CONTROLLER (gtk_event_controller_key_new ());
    gtk_event_controller_set_propagation_phase (keys, GTK_PHASE_CAPTURE);
    g_signal_connect (keys, "key-pressed", G_CALLBACK (on_key_pressed), self);
    g_signal_connect (keys, "key-released", G_CALLBACK (on_key_released), self);
    gtk_widget_add_controller (GTK_WIDGET (self), keys);

    GtkEventController *motion = GTK_EVENT_CONTROLLER (gtk_event_controller_motion_new ());
    g_signal_connect (motion, "motion", G_CALLBACK (on_motion), self);
    g_signal_connect (motion, "leave", G_CALLBACK (on_leave), self);
    gtk_widget_add_controller (GTK_WIDGET (self), motion);

    GtkGesture *click = gtk_gesture_click_new ();
    gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (click), 0);
    g_signal_connect (click, "pressed", G_CALLBACK (on_click_pressed), self);
    g_signal_connect (click, "released", G_CALLBACK (on_click_released), self);
    gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (click));

    GtkEventController *scroll = GTK_EVENT_CONTROLLER (
        gtk_event_controller_scroll_new (GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES));
    g_signal_connect (scroll, "scroll", G_CALLBACK (on_scroll), self);
    g_signal_connect (scroll, "scroll-begin", G_CALLBACK (on_scroll_begin), self);
    g_signal_connect (scroll, "scroll-end", G_CALLBACK (on_scroll_end), self);
    gtk_widget_add_controller (GTK_WIDGET (self), scroll);

    GtkEventController *focus = GTK_EVENT_CONTROLLER (gtk_event_controller_focus_new ());
    g_signal_connect (focus, "enter", G_CALLBACK (on_focus_enter), self);
    g_signal_connect (focus, "leave", G_CALLBACK (on_focus_leave), self);
    gtk_widget_add_controller (GTK_WIDGET (self), focus);
}

CorralGhostty *
corral_ghostty_new (void)
{
    return g_object_new (CORRAL_TYPE_GHOSTTY, NULL);
}

const char *
corral_ghostty_get_command (CorralGhostty *self)
{
    g_return_val_if_fail (CORRAL_IS_GHOSTTY (self), NULL);
    return self->command;
}

void
corral_ghostty_set_command (CorralGhostty *self, const char *command)
{
    g_return_if_fail (CORRAL_IS_GHOSTTY (self));
    g_object_set (self, "command", command, NULL);
}

const char *
corral_ghostty_get_cwd (CorralGhostty *self)
{
    g_return_val_if_fail (CORRAL_IS_GHOSTTY (self), NULL);
    return self->cwd;
}

void
corral_ghostty_set_cwd (CorralGhostty *self, const char *cwd)
{
    g_return_if_fail (CORRAL_IS_GHOSTTY (self));
    g_object_set (self, "cwd", cwd, NULL);
}

float
corral_ghostty_get_font_size (CorralGhostty *self)
{
    g_return_val_if_fail (CORRAL_IS_GHOSTTY (self), 13.0f);
    return self->font_size;
}

void
corral_ghostty_set_font_size (CorralGhostty *self, float font_size)
{
    g_return_if_fail (CORRAL_IS_GHOSTTY (self));
    g_object_set (self, "font-size", font_size, NULL);
}

gboolean
corral_ghostty_start (CorralGhostty *self, GError **error)
{
    g_return_val_if_fail (CORRAL_IS_GHOSTTY (self), FALSE);
    self->started = TRUE;
    gtk_gl_area_queue_render (GTK_GL_AREA (self));
    gtk_widget_queue_draw (GTK_WIDGET (self));
    return TRUE;
}

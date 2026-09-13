int main () {
    var settings = Corral.AppSettings.get_default ();
    assert (settings.font_size >= 8);
    assert (settings.font_size <= 32);
    assert (settings.window_width >= 400);
    assert (settings.window_height >= 300);
    settings.bump_font (1);
    settings.bump_font (-1);
    return 0;
}

int main () {
    var settings = Corral.AppSettings.get_default ();
    assert (settings.font_size >= 8);
    assert (settings.font_size <= 32);
    assert (settings.window_width >= 400);
    assert (settings.window_height >= 300);
    settings.bump_font (1);
    settings.bump_font (-1);
    var previous_family = settings.font_family;
    settings.font_family = "JetBrains Mono";
    assert (settings.font_family == "JetBrains Mono");
    settings.font_family = "Bad\nfont-size = 99";
    assert (settings.font_family == "Badfont-size = 99");
    settings.font_family = "  ";
    assert (settings.font_family == "");
    settings.font_family = previous_family;
    return 0;
}

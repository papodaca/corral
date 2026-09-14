# Corral

A GTK window for [Herdr](https://herdr.dev). It embeds [Ghostty](https://ghostty.org) and starts `herdr` as the child process. Workspaces, splits, and agent state stay in Herdr. Close the window and you detach; the Herdr server keeps running.

Application id: `dev.corral.Corral`
Binary: `corral`

## Herdr

Corral looks for `herdr` on `PATH`. If it is missing, the window says so and offers to look again.

```
curl -fsSL https://herdr.dev/install.sh | sh
```

Install details are at [herdr.dev/docs/install](https://herdr.dev/docs/install/). Close the window to detach. That does not stop the server.

## Build from source

Needs Meson 1.0+, Vala, GTK 4.14+, libadwaita 1.5+, libepoxy, and Zig 0.15.2 (Ghostty's compiler). Configure clones Ghostty v1.3.1 into `third_party/ghostty` and applies `patches/ghostty-gtk-embed.patch`. libghostty is linked statically into `corral`.

On Arch / CachyOS:

```
sudo pacman -S meson ninja vala pkgconf gtk4 libadwaita libepoxy git gcc libxml2 libpng zlib bzip2
./scripts/fetch-zig.sh
```

`fetch-zig.sh` puts Zig 0.15.2 in `third_party/zig-linux`. A matching `zig` on `PATH` works too.

```
meson setup build
meson compile -C build
meson devenv -C build ./src/corral
```

`meson devenv` points GSettings at the uninstalled schema and `GHOSTTY_RESOURCES_DIR` at `/usr/share/ghostty`. Run `build/src/corral` without that and you get default settings plus whatever Ghostty resources exist under the configured prefix.

To install:

```
meson setup build --prefix=/usr
meson compile -C build
sudo meson install -C build
```

The Arch package depends on `ghostty-shell-integration` and skips installing Ghostty's copy. A prefix install from this tree ships those scripts under `$prefix/share/ghostty`.

## Packages

Tagged releases on [GitHub](https://github.com/papodaca/corral/releases) ship an Arch `.pkg.tar.zst`, an AppImage, and a Flatpak bundle. The AppImage is built on Ubuntu 26.04, so it needs a glibc at least that new.

From a checkout on Arch:

```
cd packaging/arch
makepkg -si
```

That builds the git tree two directories up, not an AUR tarball.

Docker smokes that match CI:

```
./packaging/arch/smoke-docker.sh
./packaging/appimage/smoke-docker.sh
./packaging/flatpak/smoke-docker.sh
```

The Flatpak smoke needs `--privileged` for nested bubblewrap. The sandbox gets network because Herdr talks to agents and its server.

## Tests

```
meson test -C build --print-errorlogs
```

`herdr` checks that `herdr` is executable when it is on `PATH`. `settings` covers font size and window size GSettings.

## Shortcuts

| Key | Action |
| --- | --- |
| Ctrl+Q | Quit |
| Ctrl+, | Preferences |
| F11 | Fullscreen |
| Ctrl++ | Larger font |
| Ctrl+- | Smaller font |
| Ctrl+? | Shortcuts overlay |

Reconnect is in the header menu. Font size is also in preferences (8-32). Window size is remembered on close.

Those chords reach Corral only when Herdr is not eating the key, for example the missing-Herdr page. While a session is attached, keys go to Herdr. Close the window from the header menu if you want to detach.

## License

MIT.

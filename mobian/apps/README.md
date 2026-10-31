# Neo launcher for Phosh

Neo replaces Phosh's app grid as the home screen; Phosh keeps the lock screen, notifications, quick settings, the
power dialog, calls and the keyboard. Choosing the launcher: `gsettings set de.yesman.neo launcher neo|phosh`
(Mobile Settings device page: to do).

* `phosh/debian/patches/` — added to Debian's phosh 0.58.0-1 source as `0.58.0-1+neo1`:
  * `0100-neo-launcher-home-hook.patch`: with the `de.yesman.neo` schema installed, `launcher=neo` and
    `de.yesman.NeoLauncher` on the session bus, unfolding the home (home-bar swipe, button, all apps closed, login)
    calls Neo's `Show()` and folds the overview back; the overview stays invisible while dragged.
    `de.yesman.PhoshNeo.ShowOverview()` opens Phosh's running apps once (Neo's recents), `Fold()` folds.
    The recents permission lasts for one unfold (+neo2). In Neo mode a request to open the home (no app left,
    login, keybinding) shows Neo and the home stays folded (+neo5): Phosh pins its overview open with no app
    running (drag mode NONE), so folding it afterwards failed in a loop. The pin is decided again before a fold
    (+neo6), and the home is handed to Neo when the lock screen goes away (+neo7): while locked the unmapped home
    cannot fold. Recents shows the running apps only, no app grid (+neo8). Home bar (+neo9): a swipe up goes to
    Neo, a swipe up and hold (300 ms without moving) goes to recents, the cards fading in under the finger.
  * `0101-app-tracker-window-ends-startup.patch`: an app launch is done once a window of that app is mapped
    (Settings, Yesman, Compass, the browser sat behind the 30 s splash).
  * `0102-neo-recents-cards.patch`: Phosh's cards restyled for Neo (superseded by 0103 in Neo mode).
  * `0103-neo-draws-recents.patch` (+neo13): Neo draws the task view. `de.yesman.PhoshNeo` emits
    `RecentsDrag(d progress)` while the home bar is dragged up from an app (and while it animates after release),
    `RecentsHold` and `RecentsEnd(s "home"|"recents")`; `GetWindows()` returns `a(ssssuuub)` (id, app id, title,
    raw cairo ARGB32 thumbnail in `$XDG_RUNTIME_DIR/neo-recents/`, width, height, stride, focused); `Activate(id)`,
    `Close(id)`, `CloseAll()`. In Neo mode Phosh shows neither its cards nor the drag thumbnail.
  Build on the phone: `apt-get source phosh`, copy the patches in, add them to `debian/patches/series`,
  `dch -v 0.58.0-1+neo1`, `DEB_BUILD_OPTIONS="nocheck parallel=4" dpkg-buildpackage -b -uc -us`.
* `phoc/debian/patches/` — added to Debian's phoc 0.58.0-1 as `0.58.0-1+neo1` (built on the phone in
  /usr/src/neo/phoc-0.58.0, same steps as Phosh):
  * `0001-rotate-without-modeset-and-crossfade.patch`: a rotation (or any output config that keeps the output on in
    its mode) no longer re-sets "enabled" and the mode, which made wlroots do a full modeset and the DSI panel go
    dark; transform and scale are applied when rendering. And instead of the black shield while clients re-layout,
    the last frame is copied before the rotation, shown untransformed (so it looks as it did) and cross-faded away
    over 320 ms once the layout transaction is done.
* `neo-launcher/` — GJS + GTK4 + libadwaita + gtk4-layer-shell, a TOP-layer surface (exclusive keyboard while shown:
  phoc speaks layer-shell v3, no on-demand focus). Phase 1: pages on a 5×5 grid, page dots, dock (Phosh's
  favourites), wallpaper, a drawer sheet that follows the finger (settles by distance and speed) with search, back gesture from the screen edges (Alt+Left via `wtype`
  in apps). A second home swipe while Neo shows opens the running apps. Runs as the user service
  `neo-launcher.service` (gnome-session no longer starts autostart entries with an autostart phase).
  Home bar (homebar.js): Neo's own 15 px strip over Phosh's bar (OVERLAY, exclusive zone -1), hidden while
  locked (org.gnome.ScreenSaver). Phosh's drag progress was useless for deciding: it jumped to 1.0 ~80 ms into a
  slow swipe and sent RecentsEnd("home") a dozen times per release. Swipe up = home (nothing shows); swipe up and
  rest 220 ms (40 px up at least) = the task view.
  Task view (recents.js, One UI): an OVERLAY-layer window, shown only on the hold: the app (a grim screenshot taken
  when the swipe starts, before anything is drawn; Phosh's thumbnails are only as fresh as its last overview)
  shrinks into a centred card while the others come in. Tap a card = switch, swipe a card up = close, sideways =
  scroll, tap beside / swipe up again = home. Tapping an app that already runs activates its window over D-Bus
  (a second launch only asks the app to present itself, and Phosh raises nothing on that).
  Back gesture (edges.js): two 14 px strips on the screen edges between the top bar and the home bar; an arrow
  pill follows the finger and turns blue when letting go means back (Alt+Left via wtype in apps). Traps: an empty,
  fully transparent layer window draws no frame and is never mapped (1 % alpha background); the first configure is
  200 px wide whatever the size request; set_resizable(false) kept it 14×1 px instead of the configured height.
  The drawer search field is off by default (search-drawer-enabled). The drawer sheet stays mapped below the screen
  when closed: mapping it on the first frame of a swipe made the swipe stutter.
  Icons (dnd.js): tap launches; touch and hold then move drags it (edge rest turns the page, the dock takes
  icons too); touch and hold then let go opens its menu.
  Long press: on an icon *App info*, *Remove from home* / *Add to home* (drawer); on empty space *Launcher
  settings* (grid, dock, drawer, labels, icon pack, icon shape, wrap/shape switches, launcher) and *Wallpaper*.
  Icon packs (old Neo format: pack.json, gnome-map.json, icons/) in /usr/share/neo-launcher/iconpacks or
  ~/.local/share/neo-launcher/iconpacks, drawn by the old Neo's iconrender.js into ~/.cache/neo-launcher/icons,
  one icon per idle tick (picking a pack rendered every icon at once and froze the launcher for seconds).
  GApplication owns its bus name before startup builds the window: an early Show() is kept and done after.
* `ms-plugin-surya/` — Mobile Settings device page "POCO X3 NFC" with *Launcher: Phosh / Neo*
  (`de.yesman.neo launcher`). Built inside Debian's phosh-mobile-settings 0.58.0 source: copy the directory to
  `plugins/surya/`, add `subdir('surya')` to `plugins/meson.build`, `meson setup _b --prefix=/usr
  --libdir=lib/aarch64-linux-gnu && ninja -C _b plugins/surya/libms-plugin-surya.so`; install the .so into
  `/usr/lib/aarch64-linux-gnu/phosh-mobile-settings/plugins/`.

Defaults: home grid 4×5, dock 4, drawer 4 columns (changeable in de.yesman.neo).

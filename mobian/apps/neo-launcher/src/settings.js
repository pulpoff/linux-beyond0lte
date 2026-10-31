// Launcher settings (long press on the home screen › Launcher settings): an ordinary app window, so Phosh shows it
// like any other app while Neo steps aside. Everything is de.yesman.neo; Neo rebuilds itself on every change.
import Gio from 'gi://Gio';
import Gtk from 'gi://Gtk?version=4.0';
import Adw from 'gi://Adw?version=1';
import {listPacks} from './icons.js';

const SHAPES = [['system', 'Pack / system'], ['circle', 'Circle'], ['squircle', 'Squircle'], ['rounded', 'Rounded square'],
    ['square', 'Square'], ['teardrop', 'Teardrop'], ['cupertino', 'Cupertino'], ['cylinder', 'Cylinder'], ['egg', 'Egg'],
    ['octagon', 'Octagon'], ['hexagon', 'Hexagon'], ['diamond', 'Diamond']];

let current = null;

function spin(settings, key, title, min, max) {
    const row = new Adw.SpinRow({title, adjustment: new Gtk.Adjustment({lower: min, upper: max, step_increment: 1})});
    settings.bind(key, row, 'value', Gio.SettingsBindFlags.DEFAULT);
    return row;
}

function toggle(settings, key, title, subtitle = null) {
    const row = new Adw.SwitchRow({title, subtitle: subtitle ?? ''});
    settings.bind(key, row, 'active', Gio.SettingsBindFlags.DEFAULT);
    return row;
}

/** A combo row over string choices [[value, label]] bound to a string key. */
function choice(settings, key, title, choices) {
    const row = new Adw.ComboRow({title, model: Gtk.StringList.new(choices.map(([, label]) => label))});
    const sync = () => { const i = choices.findIndex(([v]) => v === settings.get_string(key)); row.selected = Math.max(0, i); };
    sync();
    settings.connect(`changed::${key}`, sync);
    row.connect('notify::selected', () => {
        const v = choices[row.selected]?.[0];
        if (v !== undefined && v !== settings.get_string(key)) settings.set_string(key, v);
    });
    return row;
}

function group(title, rows) {
    const g = new Adw.PreferencesGroup({title});
    for (const r of rows) g.add(r);
    return g;
}

export function showSettings(app) {
    if (current) { current.present(); return; }
    const s = app.settings;
    // a plain window with its own × : on Phosh the title buttons of a PreferencesWindow showed no close
    const win = current = new Adw.Window({application: app, title: 'Launcher settings', default_width: 360, default_height: 720});
    win.connect('close-request', () => { current = null; return false; });
    const header = new Adw.HeaderBar({show_end_title_buttons: false, show_start_title_buttons: false});
    const close = new Gtk.Button({icon_name: 'window-close-symbolic', tooltip_text: 'Close', css_classes: ['circular']});
    close.connect('clicked', () => win.close());
    header.pack_end(close);
    const toolbar = new Adw.ToolbarView();
    toolbar.add_top_bar(header);
    win.set_content(toolbar);

    const page = new Adw.PreferencesPage({title: 'Launcher', icon_name: 'go-home-symbolic'});
    page.add(group('Home screen', [
        spin(s, 'desktop-grid-columns', 'Columns', 3, 8),
        spin(s, 'desktop-grid-rows', 'Rows', 3, 8),
        toggle(s, 'desktop-hide-app-labels', 'Hide app labels'),
    ]));
    page.add(group('Dock', [spin(s, 'dock-num-icons', 'Icons', 2, 8)]));
    page.add(group('App drawer', [spin(s, 'drawer-grid-columns', 'Columns', 3, 8), toggle(s, 'search-drawer-enabled', 'Search field')]));
    const packs = [['', 'None (theme icons)'], ...listPacks().map(p => [p.id, p.title])];
    page.add(group('Icons', [
        choice(s, 'icon-pack', 'Icon pack', packs),
        choice(s, 'icon-shape', 'Icon shape', SHAPES),
        toggle(s, 'icon-wrap-unthemed', 'Wrap unthemed icons', 'Icons the pack does not cover get its background'),
        toggle(s, 'icon-shape-legacy', 'Shape legacy icons', 'Icons without pack art on a shaped background'),
    ]));
    page.add(group('Launcher', [choice(s, 'launcher', 'Home screen', [['neo', 'Neo'], ['phosh', 'Phosh app grid']])]));
    toolbar.set_content(page);
    win.present();
}

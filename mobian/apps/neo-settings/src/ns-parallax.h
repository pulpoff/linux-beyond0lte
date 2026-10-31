/* The parallax wallpapers Neo can show (de.yesman.neo wallpaper-parallax): every directory with a parallax.json
 * in /usr/share/neo-launcher/parallax and ~/.local/share/neo-launcher/parallax, as a grid of previews. */
#pragma once

#include <gtk/gtk.h>

GtkWidget *ns_parallax_grid (GSettings *neo);

/* Neo Settings' own pages; each pushes itself onto the navigation. */
#pragma once

#include <gtk/gtk.h>

/* ns-pages.c: the hubs and the small pages */
void ns_page_connections (GtkWidget *row, gpointer data);
void ns_page_display (GtkWidget *row, gpointer data);
void ns_page_battery (GtkWidget *row, gpointer data);
void ns_page_wallpaper (GtkWidget *row, gpointer data);
void ns_page_home (GtkWidget *row, gpointer data);
void ns_page_general (GtkWidget *row, gpointer data);
void ns_page_about (GtkWidget *row, gpointer data);

/* ns-net.c: NetworkManager */
void ns_page_wifi (GtkWidget *row, gpointer data);
void ns_page_mobile (GtkWidget *row, gpointer data);
void ns_page_hotspot (GtkWidget *row, gpointer data);
void ns_page_network (GtkWidget *row, gpointer data);
void ns_page_printers (GtkWidget *row, gpointer data);

/* ns-bt.c: BlueZ */
void ns_page_bluetooth (GtkWidget *row, gpointer data);
void ns_page_sharing (GtkWidget *row, gpointer data);

/* ns-sound.c: PulseAudio (PipeWire's), feedbackd, sound theme */
void ns_page_sound (GtkWidget *row, gpointer data);

/* ns-apps.c: apps, their notifications, default apps */
void ns_page_notifications (GtkWidget *row, gpointer data);
void ns_page_apps (GtkWidget *row, gpointer data);
void ns_page_default_apps (GtkWidget *row, gpointer data);
void ns_page_app (const char *id);

/* ns-shell.c: Phosh: lock screen, quick settings, keyboard */
void ns_page_lockshell (GtkWidget *row, gpointer data);
void ns_page_lock_screen (GtkWidget *row, gpointer data);
void ns_page_keyboard (GtkWidget *row, gpointer data);

/* ns-privacy.c */
void ns_page_privacy (GtkWidget *row, gpointer data);
void ns_page_location (GtkWidget *row, gpointer data);
void ns_page_accounts (GtkWidget *row, gpointer data);
void ns_page_accessibility (GtkWidget *row, gpointer data);

/* ns-system.c */
void ns_page_language (GtkWidget *row, gpointer data);
void ns_page_datetime (GtkWidget *row, gpointer data);
void ns_page_users (GtkWidget *row, gpointer data);
void ns_page_power (GtkWidget *row, gpointer data);
void ns_page_mouse (GtkWidget *row, gpointer data);
void ns_page_wallpapers (GtkWidget *row, gpointer data);
void ns_page_software (GtkWidget *row, gpointer data);
/* the "Screen scale" row of Display: filled in once the compositor has answered */
void ns_page_scale_row (GtkWidget *group);

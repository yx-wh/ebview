/* Minimal GTK2 input probe -- diagnostic only, NOT part of the EBView build.
 *
 * Purpose: on a machine where EBView's window appears but receives no
 * mouse/keyboard input, decide whether the fault is in EBView's code or in
 * the GTK2 runtime / OS environment.  This program links against the very
 * same UCRT64 GTK2 DLLs shipped in the EBView dist, contains no IME tweaks,
 * no hooks, no timers other than a 500 ms refresh, and no EBView code.
 *
 *   - clicks/keys/motions increase when you interact -> GTK2 input works
 *     here, so the freeze is specific to EBView's code.
 *   - counters stay at 0 while the window looks frozen -> the GTK2 runtime /
 *     Win7 environment is at fault, EBView's code is not the cause.
 *
 * Counters are shown in the window title (visible without any tooling) and
 * written to probe.log in the current working directory.
 */
#include <gtk/gtk.h>
#include <stdio.h>

static int clicks = 0, keys = 0, motions = 0;
static GtkWidget *win;

static void refresh(void)
{
	char title[160];
	FILE *f;

	snprintf(title, sizeof(title),
	         "GTK input probe - clicks=%d keys=%d motions=%d",
	         clicks, keys, motions);
	gtk_window_set_title(GTK_WINDOW(win), title);

	f = fopen("probe.log", "w");
	if(f != NULL){
		fprintf(f, "clicks=%d keys=%d motions=%d\n", clicks, keys, motions);
		fclose(f);
	}
}

static gboolean on_button_press(GtkWidget *w, GdkEventButton *e, gpointer d)
{
	clicks++;
	refresh();
	return FALSE;
}

static gboolean on_key_press(GtkWidget *w, GdkEventKey *e, gpointer d)
{
	keys++;
	refresh();
	return FALSE;
}

static gboolean on_motion(GtkWidget *w, GdkEventMotion *e, gpointer d)
{
	motions++;
	return FALSE;
}

static void on_clicked(GtkWidget *w, gpointer d)
{
	clicks++;
	refresh();
}

static gboolean tick(gpointer d)
{
	refresh();
	return TRUE;
}

int main(int argc, char **argv)
{
	GtkWidget *vbox, *btn, *label;

	gtk_init(&argc, &argv);

	win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_default_size(GTK_WINDOW(win), 460, 240);
	g_signal_connect(win, "destroy", G_CALLBACK(gtk_main_quit), NULL);
	g_signal_connect(win, "button-press-event", G_CALLBACK(on_button_press), NULL);
	g_signal_connect(win, "key-press-event", G_CALLBACK(on_key_press), NULL);
	g_signal_connect(win, "motion-notify-event", G_CALLBACK(on_motion), NULL);
	gtk_widget_add_events(win,
	                      GDK_BUTTON_PRESS_MASK | GDK_KEY_PRESS_MASK |
	                      GDK_POINTER_MOTION_MASK);

	vbox = gtk_vbox_new(FALSE, 8);
	gtk_container_add(GTK_CONTAINER(win), vbox);

	label = gtk_label_new(
		"Click this window, press keys, move the mouse.\n"
		"The title bar and probe.log show the counters.");
	gtk_box_pack_start(GTK_BOX(vbox), label, TRUE, TRUE, 0);

	btn = gtk_button_new_with_label("Click me");
	g_signal_connect(btn, "clicked", G_CALLBACK(on_clicked), NULL);
	gtk_box_pack_start(GTK_BOX(vbox), btn, FALSE, FALSE, 0);

	gtk_widget_show_all(win);
	g_timeout_add(500, tick, NULL);
	refresh();

	gtk_main();
	return 0;
}

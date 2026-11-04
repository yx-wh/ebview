/*  Copyright (C) 2001-2004  Kenichi Suto
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
*/


#include "defs.h"
#include "global.h"

#include "jcode.h"
#include "mainwindow.h"
#include "textview.h"
#include "statusbar.h"

gboolean active=FALSE;
static gchar g_message[1024];

void center_dialog(GtkWidget *window, GtkWidget *dialog){
	gint window_x, window_y;
	gint window_width, window_height;
	gint dialog_x, dialog_y;
	gint dialog_width, dialog_height;

	gdk_window_get_root_origin(window->window, &window_x, &window_y);
	window_width = window->allocation.width;
	window_height = window->allocation.height;
	dialog_width = dialog->allocation.width;
	dialog_height = dialog->allocation.height;

	if((window_width <= dialog_width) || 
	   (window_height <= dialog_height))
		return;

	dialog_x = window_x + (window_width - dialog_width) / 2;
	dialog_y = window_y + (window_height - dialog_height) / 2;

	gtk_window_move(GTK_WINDOW(dialog), dialog_x, dialog_y);
	
}

static void dialog_response(GtkWidget *dialog, gint response, gpointer data)
{
	gtk_widget_destroy(dialog);
}

static void dialog_destroyed(GtkWidget *dialog, gpointer data)
{
	active = FALSE;
}

gboolean idle_warning(gpointer data){
	GtkWidget *dialog;
	GtkWidget *parent;

	LOG(LOG_DEBUG, "IN : idle_warning");

	if(active == TRUE)
		goto END;

	active  = TRUE;

	parent = gtk_grab_get_current();
	if(parent == NULL)
		parent = main_window;

	dialog = gtk_message_dialog_new(GTK_WINDOW(parent),
					GTK_DIALOG_DESTROY_WITH_PARENT /* | GTK_DIALOG_NO_SEPARATOR */,
					GTK_MESSAGE_WARNING,
					GTK_BUTTONS_OK,
					"%s", g_message);
	/* Shown non-modally on purpose: gtk_dialog_run() starts a nested main
	 * loop, and if anything else holds a GTK grab (the search progress
	 * dialog does) that loop never receives input and the whole UI looks
	 * frozen until killed.  Close still works normally. */
	g_signal_connect(G_OBJECT(dialog), "response",
			 G_CALLBACK(dialog_response), NULL);
	g_signal_connect(G_OBJECT(dialog), "destroy",
			 G_CALLBACK(dialog_destroyed), NULL);
	gtk_widget_show(dialog);

 END:
	LOG(LOG_DEBUG, "OUT : idle_warning");
	return(FALSE);
}

gboolean idle_error(gpointer data){
	GtkWidget *dialog;
	GtkWidget *parent;

	LOG(LOG_DEBUG, "IN : idle_error");

	if(active == TRUE)
		goto END;

	active  = TRUE;

	parent = gtk_grab_get_current();
	if(parent == NULL)
		parent = main_window;

	dialog = gtk_message_dialog_new(GTK_WINDOW(parent),
					GTK_DIALOG_DESTROY_WITH_PARENT /* | GTK_DIALOG_NO_SEPARATOR */,
					GTK_MESSAGE_ERROR,
					GTK_BUTTONS_CLOSE,
					"%s", g_message);
					
	/* Shown non-modally on purpose: gtk_dialog_run() starts a nested main
	 * loop, and if anything else holds a GTK grab (the search progress
	 * dialog does) that loop never receives input and the whole UI looks
	 * frozen until killed.  Close still works normally. */
	g_signal_connect(G_OBJECT(dialog), "response",
			 G_CALLBACK(dialog_response), NULL);
	g_signal_connect(G_OBJECT(dialog), "destroy",
			 G_CALLBACK(dialog_destroyed), NULL);
	gtk_widget_show(dialog);

 END:
	LOG(LOG_DEBUG, "OUT : idle_error");
	return(FALSE);
}

void popup_warning(char *message){

	LOG(LOG_DEBUG, "IN : popup_warning");

	strncpy(g_message, message, sizeof(g_message) - 1);
	g_message[sizeof(g_message) - 1] = '\0';
	/* Errors can be logged from a draw callback. Running a modal dialog
	 * synchronously there re-enters GTK and may crash; defer it instead. */
	g_idle_add(idle_warning, NULL);

	LOG(LOG_DEBUG, "OUT : popup_warning");
}

void popup_error(char *message){

	LOG(LOG_DEBUG, "IN : popup_error");

	strncpy(g_message, message, sizeof(g_message) - 1);
	g_message[sizeof(g_message) - 1] = '\0';
	/* See popup_warning(): never open a modal dialog from a draw callback. */
	g_idle_add(idle_error, NULL);

	LOG(LOG_DEBUG, "OUT : popup_error");
}


extern GtkTextBuffer *text_buffer;

/* Search workers run in pthreads and log their progress with push_message().
 * GTK widgets must only be touched from the thread owning the main loop:
 * inserting into a GtkTextBuffer off-thread invalidates iterators held by
 * the UI and can crash it.  Queue the text for the main loop instead. */
static gboolean push_message_idle(gpointer data)
{
	GtkTextIter iter;
	gchar *str = data;

	gtk_text_buffer_get_end_iter(text_buffer, &iter);
	gtk_text_buffer_insert(text_buffer, &iter, str, -1);
	g_free(str);
	return(FALSE);
}

void push_message(gchar *str){
	GtkTextIter iter;

#ifdef __WIN32__
	// Windows だとなぜか死ぬ
	return;
#endif

	if(!g_main_context_is_owner(g_main_context_default())){
		g_idle_add(push_message_idle, g_strdup(str));
		return;
	}

	gtk_text_buffer_get_end_iter(text_buffer, &iter);
	gtk_text_buffer_insert(
		text_buffer, &iter,
		str, -1);
}

void clear_message(){
	clear_text_buffer();
}


gboolean popup_active()
{
	return(active);
}


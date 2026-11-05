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

#define _GLOBAL
#include <signal.h>
#include <pthread.h>

#ifndef __WIN32__
#include <sys/wait.h>
#endif

#ifdef __WIN32__
#include <winsock.h>
#else
#include <sys/socket.h>
#include <sys/un.h>
#endif

#include "defs.h"
#include "global.h"

#include <gdk/gdkkeysyms.h>
#ifndef __WIN32__
#include <X11/Xlib.h>
#include <gdk/gdkx.h>
#endif

#include "eb.h"
#include "mainwindow.h"
#include "headword.h"
#include "mainmenu.h"
#include "dictbar.h"
#include "statusbar.h"
#include "pixmap.h"
#include "selection.h"
#include "preference.h"
#include "dialog.h"
#include "shortcut.h"
#include "pref_io.h"
#include "splash.h"

static pthread_t server_tid=(pthread_t)-1;

static gint conn;
static gchar sock_name[512];
gchar *exe_path;

extern GtkWidget *note_tree;

void exit_program( GtkWidget *widget,
		   gpointer   data )
{
#if 0
#ifndef __WIN32__
	void *p;
#endif
#endif

	if(pthread_self() == server_tid)
		pthread_exit(0);

	ebook_end();

	/* main_window/note_tree only exist after create_main_window(); this
	 * function can also be reached from a signal during start-up. */
	if(main_window != NULL && note_tree != NULL){
		gdk_window_get_root_origin(main_window->window, &window_x, &window_y);
		window_width = main_window->allocation.width;
		window_height = main_window->allocation.height;

		tree_width = note_tree->allocation.width;
		tree_height = note_tree->allocation.height;
	}

	save_preference();
	gtk_main_quit ();

	if(server_tid != (pthread_t)-1)
		pthread_cancel(server_tid);

	close(conn);
	unlink(sock_name);
#if 0
#ifndef __WIN32__
	pthread_join(server_tid, &p);
#endif
#endif
//	exit(0);
}

static void sig_handler(int sig){
	switch(sig){
	case SIGTERM:
	case SIGINT:
		/* GTK/GLib/pthread calls are not async-signal-safe. Calling
		 * exit_program() here caused intermittent SIGSEGV on Ctrl+C /
		 * SIGTERM. Normal quit (menu / window close) still goes through
		 * exit_program() and saves preferences. */
		_exit(128 + sig);
	default:
		break;
	}
}

#if 0

gint g_argc;
gchar *g_argv[16];

extern GtkWidget *popup;

static void remote_command( GtkWidget *widget,
		   gpointer   data )
{
	gboolean bpopup=FALSE;
	gint i;
	gchar word[512];

	if(strcmp(g_argv[1], "--search") == 0){
		word[0] = '\0';

		for(i=2; i < g_argc ; i ++){
			strcat(word, g_argv[i]);
			strcat(word, " ");
		}

		if(strlen(word) != 0){
			gtk_entry_set_text(GTK_ENTRY(word_entry), word);
			start_search();
		}
	} else if((strcmp(g_argv[1], "--selection") == 0) || 
		  (strcmp(g_argv[1], "--popup") == 0)) {

		if(strcmp(g_argv[1], "--popup") == 0)
			bpopup = TRUE;

		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(button_popup), bpopup);
		bshow_popup = bpopup;
		copy_clipboard(NULL);
		return;
	} else if(strcmp(g_argv[1], "--close-popup") == 0){
/*
		if(popup != NULL)
			gtk_signal_emit_by_name(GTK_OBJECT (popup), 
//					"delete_event");
						"close_popup");
			gtk_signal_emit_by_name(GTK_OBJECT (popup), 
						"redraw");
*/
	}
}

#ifndef __WIN32__

static void *server_thread(void *arg)
{
	gint count=0;
	gchar buff[256];
	int len, read_len;
	gchar *p;
	gint i;
	gint state;

	struct sockaddr_un address;
	int sock;
	size_t addrLength;

//	signal(SIGINT, SIG_DFL);
	signal(SIGTERM, SIG_DFL);
#ifndef __WIN32__
	signal(SIGCHLD, SIG_DFL);
	signal(SIGPIPE, SIG_DFL);
#endif

	pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, &state);
	pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, &state);
	if ((sock = socket(PF_UNIX, SOCK_STREAM, 0)) < 0) {
		perror("socket");
		_exit(1);
	}

	/* Remove any preexisting socket (or other file) */

	address.sun_family = AF_UNIX;	/* Unix domain socket */
	sprintf(sock_name, "%s/.remote-sock", user_dir);
	strcpy(address.sun_path, sock_name);
	unlink(sock_name);

	/* The total length of the address includes the sun_family 
	   element */

#ifdef __FreeBSD__
	addrLength = sizeof(address.sun_len) + sizeof(address.sun_family) + strlen(address.sun_path) + 1;
	address.sun_len = addrLength;
#else
	addrLength = sizeof(address.sun_family) + strlen(address.sun_path);
#endif

	if (bind(sock, (struct sockaddr *) &address, addrLength)){
		perror("bind");
		_exit(1);
	}

	if (listen(sock, 5)){
		perror("listen");
		_exit(1);
	}


	sprintf(buff, "remote command %d", count);

	while ((conn = accept(sock, (struct sockaddr *) &address, 
			      &addrLength)) >= 0) {
		read_len = read(conn, buff, 1);
		if(read_len != 1){
			close(conn);
			continue;
		}

		len = (unsigned char)buff[0];
		if(len == 0){
			close(conn);
			continue;
		}

		read_len = read(conn, buff, len);
		if(read_len != len){
			perror("read");
			close(conn);
			continue;
		}

		close(conn);

		p = buff;
		g_argc = *p;
		p ++;


		for(i=0; i<g_argc; i++){
			g_argv[i] = p;
			p = strchr(p, '\0');
			p++;
		}

		gdk_threads_enter();
/*
		gtk_signal_emit(GTK_OBJECT(main_window), signal_remote_command);
*/
		gdk_threads_leave();
	}

	if (conn < 0) {
		perror("accept");
		_exit(1);
	}
	
	return(NULL);
}
#endif

void execute_remote_command(gint argc, gchar **argv){
	gint i;

	g_argc = argc;

	for(i=0; i<argc ; i++)
		g_argv[i] = argv[i];

	gdk_threads_enter();
//	gtk_signal_emit (GTK_OBJECT(main_window), signal_remote_command);
	gdk_threads_leave();

}
#endif /* #if 0 */

#ifdef __WIN32
void  my_log_handler(const gchar *log_domain, GLogLevelFlags log_level, const gchar *message, gpointer user_data)
{
}

#endif

/* Told once the dictionary groups have been read, after the main loop is
 * running -- a dialog created before gtk_main() would block start-up, which
 * is exactly why log.c suppresses popups while bstarting_up is TRUE. */
static gboolean show_unavailable_dicts(gpointer data)
{
	gint count;
	gchar *names;
	gchar *message;

	count = dictgroup_unavailable_count();
	if(count <= 0)
		return(FALSE);

	names = dictgroup_unavailable_summary();

	if(count == 1){
		message = g_strdup_printf(
			_("A dictionary in the dictionary groups could not be "
			  "opened and is kept in the list, greyed out, so that "
			  "your settings are not lost.\n\n"
			  "The dictionary may have been moved or deleted, or the "
			  "drive or network share it lives on may not be mounted "
			  "at the moment.%s"), names);
	} else {
		message = g_strdup_printf(
			_("%d dictionaries in the dictionary groups could not be "
			  "opened and are kept in the list, greyed out, so that "
			  "your settings are not lost.\n\n"
			  "They may have been moved or deleted, or the drive or "
			  "network share they live on may not be mounted at the "
			  "moment.%s"), count, names);
	}

	/* Do not LOG() this at WARNING level: log.c turns every message at or
	 * above LOG_MESSAGE into its own popup, which would double up with the
	 * one below.  Each failing book already logged its own CRITICAL line. */
	popup_warning(message);

	g_free(message);
	g_free(names);

	return(FALSE);
}

int main( int   argc,
	  char *argv[] )
{
#if 0
	gint rc;

#ifndef __WIN32__
	pthread_attr_t thread_attr;	
#endif
#endif

	g_thread_init(NULL);
#ifndef __WIN32__
	/* Never enable the GDK thread lock on Windows.  gdk_threads_init()
	 * makes every GDK entry point take gdk_threads_mutex, and the Win32
	 * backend dispatches messages from inside the modal loop that Windows
	 * runs while a window is being dragged or resized.  That loop re-enters
	 * GDK while the main loop already owns the lock, so the first mouse
	 * move after grabbing the title bar deadlocks the process for good.
	 * EBView's worker threads never touch GTK directly (they hand results
	 * back through g_idle_add), so the lock buys nothing here anyway. */
	gdk_threads_init();
#endif
	bstarting_up = TRUE;

#ifdef __WIN32__
#ifndef DEBUG
	// Suppress application messsage
	g_log_set_handler (NULL, G_LOG_LEVEL_MASK | G_LOG_FLAG_FATAL | G_LOG_FLAG_RECURSION, my_log_handler, NULL);

	// Suppress GObject messsage
	g_log_set_handler ("GLib-GObject", G_LOG_LEVEL_MASK | G_LOG_FLAG_FATAL | G_LOG_FLAG_RECURSION, my_log_handler, NULL);

	// Suppress GLib messsage
	g_log_set_handler ("GLib", G_LOG_LEVEL_MASK | G_LOG_FLAG_FATAL | G_LOG_FLAG_RECURSION, my_log_handler, NULL);

	// Suppress Gtk messsage
	g_log_set_handler ("Gtk", G_LOG_LEVEL_MASK | G_LOG_FLAG_FATAL | G_LOG_FLAG_RECURSION, my_log_handler, NULL);

	// Suppress Gdk messsage
	g_log_set_handler ("Gdk", G_LOG_LEVEL_MASK | G_LOG_FLAG_FATAL | G_LOG_FLAG_RECURSION, my_log_handler, NULL);
#endif
#endif

	LOG(LOG_DEBUG, "IN: main()");

	signal(SIGINT, sig_handler);
	signal(SIGTERM, sig_handler);
#ifndef __WIN32__
	signal(SIGCHLD, sig_handler);
	signal(SIGPIPE, sig_handler);
#endif

	gtk_init (&argc, &argv);
	exe_path = strdup(argv[0]);

	initialize_preference();

#ifdef __WIN32__
	bindtextdomain(PACKAGE,package_dir);
#else
	/* An uninstalled build keeps its catalogues next to the binary
	 * (build/locale/...), which LOCALEDIR would never point at.  Use that
	 * directory when present so translations can be tested without
	 * installing; installed builds have no such directory. */
	{
		gchar *exe_dir = g_path_get_dirname(exe_path);
		gchar *locale_dir = g_build_filename(exe_dir, "locale", NULL);
		if(g_file_test(locale_dir, G_FILE_TEST_IS_DIR))
			bindtextdomain(PACKAGE, locale_dir);
		else
			bindtextdomain(PACKAGE, LOCALEDIR);
		g_free(locale_dir);
		g_free(exe_dir);
	}
#endif

	bind_textdomain_codeset(PACKAGE, "UTF-8");
	textdomain(PACKAGE);

	ebook_start();
	load_preference();

	if(bshow_splash)
		splash_create();
	load_dictgroup();
	splash_update(_("Loading ..."), "");
	load_weblist();
	load_shortcut();
	load_stemming_en();
	load_stemming_ja();
	load_filter();
	load_history();
	load_dirlist();
	load_dirgroup();

	/* Upgrade legacy font descriptions: old X11 strings ("-*-...") and the
	 * exact Kochi defaults. Only exact matches are replaced, so a font the
	 * user picked explicitly is never overwritten. */
	if(fontset_normal[0] == '-' ||
	   strcmp(fontset_normal, "Kochi Mincho 12") == 0){
		g_free(fontset_normal);
		fontset_normal = strdup("Noto Serif CJK JP 12");
	}
	if(fontset_bold[0] == '-' ||
	   strcmp(fontset_bold, "Kochi Gothic 12") == 0){
		g_free(fontset_bold);
		fontset_bold = strdup("Noto Sans CJK JP Bold 12");
	}
	if(fontset_italic[0] == '-' ||
	   strcmp(fontset_italic, "Sans Italic 12") == 0){
		g_free(fontset_italic);
		fontset_italic = strdup("Noto Serif CJK JP Italic 12");
	}
	if(fontset_superscript[0] == '-' ||
	   strcmp(fontset_superscript, "Kochi Gothic 8") == 0){
		g_free(fontset_superscript);
		fontset_superscript = strdup("Noto Sans CJK JP 8");
	}

	check_search_method();

#if 0
	signal_remote_command =
		gtk_object_class_user_signal_new (gtk_type_class (GTK_TYPE_WIDGET),
						  "remote_command",
						  GTK_RUN_LAST | GTK_RUN_ACTION,
						  gtk_marshal_NONE__POINTER,
						  GTK_TYPE_NONE, 0);

	gtk_object_class_user_signal_new (gtk_type_class (GTK_TYPE_WIDGET),
					  "close_popup",
					  GTK_RUN_LAST | GTK_RUN_ACTION,
					  gtk_marshal_NONE__POINTER,
					  GTK_TYPE_NONE, 0);

#ifndef __WIN32__

 	pthread_attr_init (&thread_attr) ;
 	pthread_attr_setstacksize (&thread_attr, 512*1024) ;

	rc = pthread_create(&server_tid, &thread_attr, server_thread, (void *)NULL);
	if(rc != 0){
		LOG(LOG_CRITICAL, "OUT: main() pthread_create : ", strerror(errno));
		exit(1);
	}
	pthread_attr_destroy(&thread_attr);

#endif
#endif

	// Create main window
	create_main_window();

	calculate_font_size();

	search_result = NULL;
	current_result = NULL;

#if 0
	// If there are parameters, this must be called from remote command.
	if(argc != 1)
		execute_remote_command(argc, argv);

#endif

	splash_destroy();

	// main loop
#ifndef __WIN32__
	gdk_threads_enter();
#endif
	bstarting_up = FALSE;

	/* Entries that stayed in dictgroup.xml but could not be opened: warn
	 * once, from inside the main loop. */
	if(dictgroup_unavailable_count() > 0)
		g_idle_add(show_unavailable_dicts, NULL);

	gtk_main ();
#ifndef __WIN32__
	gdk_threads_leave();
#endif
	ebook_end();

	LOG(LOG_DEBUG, "OUT: main()");

	_exit(0);

}


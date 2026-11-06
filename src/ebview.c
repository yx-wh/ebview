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
#include <windows.h>
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
#include "log.h"

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
	/* The release build installs this handler to swallow GLib/GTK/Gdk
	 * warnings (lines 482-494).  When the diagnostic log is open we mirror
	 * those messages to it, because on a headless/console-hidden launch a
	 * GTK CRITICAL on Windows 7 would otherwise be invisible. */
	if(ebview_debug_fp != NULL){
		fprintf(ebview_debug_fp, "[%s] %s\n",
			log_domain ? log_domain : "ebview",
			message ? message : "(null)");
		fflush(ebview_debug_fp);
	}
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

/* The binary is a console-subsystem executable (CMakeLists.txt does not pass
 * WIN32 to add_executable), because LOG() writes to stderr and that is worth
 * keeping when the program is started from a terminal.  Double-clicking it in
 * Explorer, however, makes Windows allocate a console window of its own, which
 * then sits next to the GUI looking like a leftover terminal.  Hide it only in
 * that case: GetConsoleProcessList() returns 1 when this process is the sole
 * owner of the console, more when a shell shares it, and 0 when there is no
 * console at all (detached process).  The console still exists after hiding,
 * so stderr keeps working and nothing else changes. */
#ifdef __WIN32__
static void hide_own_console(void)
{
	DWORD owners[2];

	if(GetConsoleProcessList(owners, 2) == 1)
		ShowWindow(GetConsoleWindow(), SW_HIDE);
}
#endif

/* ----------------------------------------------------------- command line --
 *
 * "ebview 見る" (or equivalently "ebview -s 見る") looks the word up as soon
 * as the window is on screen, so a launcher or a file manager action can open
 * the program straight on an entry.  Every argument that is not an option is
 * part of the search string, which keeps "ebview foo bar" behaving like the
 * AND search the GUI would run for the same input.
 *
 * This is deliberately start-up only: unlike the remote-control protocol in
 * the #if 0 block above, it does not talk to an already running instance.
 */

static gchar *startup_search_word = NULL;

/* argv holds bytes in the locale encoding, GTK wants UTF-8.  On Windows that
 * means the ANSI code page, so the conversion is not optional there.  A
 * UTF-8 locale - the normal case on Linux - makes it a no-op, and checking
 * g_utf8_validate() first means a value GLib already converted is left
 * alone. */
static gchar *to_utf8(const gchar *text)
{
	gchar *converted;

	if(text == NULL)
		return(NULL);

	if(g_utf8_validate(text, -1, NULL))
		return(g_strdup(text));

	converted = g_locale_to_utf8(text, -1, NULL, NULL, NULL);

	return(converted != NULL ? converted : g_strdup(text));
}

static void add_startup_word(const gchar *text)
{
	gchar *word;
	gchar *joined;

	if(text == NULL)
		return;

	word = to_utf8(text);
	if(word == NULL)
		return;

	if(*word == '\0'){
		g_free(word);
		return;
	}

	if(startup_search_word == NULL){
		startup_search_word = word;
		return;
	}

	joined = g_strconcat(startup_search_word, " ", word, NULL);
	g_free(startup_search_word);
	g_free(word);
	startup_search_word = joined;
}

/* start_search() returns early unless the window is mapped, and the first
 * idle callback can run before the map event has been processed - so retry
 * for a couple of seconds instead of dropping the request. */
static gboolean lookup_startup_word(gpointer data)
{
	static gint tries = 0;

	if(startup_search_word == NULL)
		return(FALSE);

	if((main_window == NULL) || (word_entry == NULL))
		return(FALSE);

	if((GTK_WIDGET_MAPPED(main_window) != TRUE) && (tries++ < 100))
		return(TRUE);

	LOG(LOG_DEBUG, "IN : lookup_startup_word(%s)", startup_search_word);

	gtk_entry_set_text(GTK_ENTRY(word_entry), startup_search_word);
	start_search();

	g_free(startup_search_word);
	startup_search_word = NULL;

	return(FALSE);
}

int main( int   argc,
	  char *argv[] )
{
#ifdef __WIN32__
	hide_own_console();

	/* Diagnostic log.  Set EBVIEW_DEBUG_LOG=1 (or a file path) before
	 * launching to capture GTK/GLib warnings and the app's own LOG() trace
	 * to a file -- the release build hides the console and would otherwise
	 * swallow everything.  Opened unbuffered so a hang never loses the last
	 * line.  g_log / GLib messages are caught by my_log_handler(); the app's
	 * own LOG() lines are caught in log_func(). */
	{
		const char *dbg = getenv("EBVIEW_DEBUG_LOG");
		if(dbg != NULL && *dbg != '\0'){
			const char *path;
			char tmp[1024];

			if(strcmp(dbg, "1") == 0 || strcmp(dbg, "yes") == 0){
				const char *t = getenv("TEMP");
				if(t == NULL) t = getenv("TMP");
				if(t != NULL){
					snprintf(tmp, sizeof(tmp),
						 "%s\\ebview-debug.log", t);
					path = tmp;
				} else {
					path = "ebview-debug.log";
				}
			} else {
				path = dbg;
			}

			ebview_debug_fp = fopen(path, "w");
			if(ebview_debug_fp != NULL){
				setvbuf(ebview_debug_fp, NULL, _IONBF, 0);
				ebview_log_level = LOG_DEBUG;
				fprintf(ebview_debug_fp,
					"EBView debug log opened\n");
				fflush(ebview_debug_fp);
			}
		}
	}
#endif
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

	{
		/* gtk_init_with_args() parses the GTK options, then ours, and
		 * leaves the leftovers - the search words - in argv.  It also
		 * gives -h/--help for free, and handles it before opening a
		 * display, which is what makes `ebview --help` work over ssh. */
		gboolean show_version = FALSE;
		gchar *option_word = NULL;
		GOptionEntry entries[] = {
			{ "search", 's', 0, G_OPTION_ARG_STRING, &option_word,
			  N_("look WORD up as soon as the window appears"),
			  N_("WORD") },
			{ "version", 'V', 0, G_OPTION_ARG_NONE, &show_version,
			  N_("show the version and exit"), NULL },
			{ NULL, 0, 0, 0, NULL, NULL, NULL }
		};
		GError *error = NULL;
		gint i;

		if(gtk_init_with_args(&argc, &argv, N_("[WORD...]"), entries,
				      GETTEXT_PACKAGE, &error) != TRUE){
			g_printerr("%s\n", error->message);
			g_error_free(error);
			return(1);
		}

		if(show_version){
			g_print("%s\n", PACKAGE_STRING);
			g_free(option_word);
			return(0);
		}

		add_startup_word(option_word);
		g_free(option_word);

		for(i = 1; i < argc; i++)
			add_startup_word(argv[i]);
	}

	exe_path = strdup(argv[0]);

	initialize_preference();

	bindtextdomain(PACKAGE,locale_dir);

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

	/* A word given on the command line is looked up once the window is
	 * actually on screen; the dictionaries are already loaded by now. */
	if(startup_search_word != NULL)
		g_timeout_add(50, lookup_startup_word, NULL);

	gtk_main ();
#ifndef __WIN32__
	gdk_threads_leave();
#endif
	ebook_end();

	LOG(LOG_DEBUG, "OUT: main()");

#ifdef __WIN32__
	if(ebview_debug_fp != NULL){
		fclose(ebview_debug_fp);
		ebview_debug_fp = NULL;
	}
#endif

	_exit(0);

}


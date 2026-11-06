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

#include <stdio.h>
#include <stdarg.h>
#include <glib.h>
#include <glib/gprintf.h>
#include <string.h>

#include "dialog.h"
#include "log.h"

extern gint bstarting_up;
extern gint thread_running;

#define STDERR stderr


gint ebview_log_level = LOG_MESSAGE;
//gint ebview_log_level = LOG_DEBUG;

/* See log.h.  NULL unless main.c opened it in response to EBVIEW_DEBUG_LOG. */
FILE *ebview_debug_fp = NULL;

void set_log_level(gint level){
	ebview_log_level = level;
}

void log_func(const gchar *file, gint line, LOG_LEVEL level, const gchar *message, ...){
	va_list ap;
	gchar str[1024];

	if(level <= ebview_log_level) {
		const gchar *prefix;

		switch(level){
		case LOG_ERROR:
			prefix = " ERROR : ";
			break;
		case LOG_CRITICAL:
			prefix = " CRITICAL : ";
			break;
		case LOG_WARNING:
			prefix = " WARNING : ";
			break;
		case LOG_MESSAGE:
			prefix = " MESSAGE : ";
			break;
		case LOG_INFO:
			prefix = " INFO : ";
			break;
		case LOG_DEBUG:
			prefix = " DEBUG : ";
			break;
		default:
			prefix = " : ";
			break;
		}

		/* Build "<file>:<line><level> : <formatted message>" once, then
		 * send it to the (hidden) console and, when enabled, the debug
		 * file.  The file is opened unbuffered so a deadlock or hard
		 * crash that never reaches _exit still leaves every line on disk. */
		{
			gchar hdr[1024];
			gchar body[4096];
			gchar out[6144];

			snprintf(hdr, sizeof(hdr), "%s:%d%s", file, line, prefix);

			va_start(ap, message);
			vsnprintf(body, sizeof(body), message, ap);
			va_end(ap);

			snprintf(out, sizeof(out), "%s%s", hdr, body);

			g_printf("%s\n", out);
			fflush(stdout);

			if(ebview_debug_fp != NULL){
				fprintf(ebview_debug_fp, "%s\n", out);
				fflush(ebview_debug_fp);
			}
		}
		//g_logv(G_LOG_DOMAIN, level, format, ap);
	}

	// Show dialog box. During start-up the main window does not exist yet,
	// so a modal dialog here would block (or crash) before gtk_main().
	// A broken dictionary can emit thousands of errors while rendering;
	// show each source location at most once and cap the total, so modal
	// dialogs can never monopolise the main loop and hang the UI.
	/* The search progress dialog owns a GTK grab; a modal dialog opened
	 * during a search would never receive input and freeze the UI. */
	if(level <= LOG_MESSAGE && !bstarting_up && !thread_running){
		static GHashTable *popup_seen = NULL;
		static guint popup_count = 0;
		const guint MAX_ERROR_POPUPS = 5;
		gchar *key;

		if(popup_seen == NULL)
			popup_seen = g_hash_table_new_full(g_str_hash, g_str_equal,
							   g_free, NULL);
		key = g_strdup_printf("%s:%d", file, line);
		if(g_hash_table_contains(popup_seen, key) ||
		   popup_count >= MAX_ERROR_POPUPS){
			g_free(key);
			return;
		}
		g_hash_table_add(popup_seen, key);
		popup_count++;

		va_start(ap, message);
		vsprintf(str, message, ap);
		va_end(ap);

		switch(level){
		case LOG_ERROR:
		case LOG_CRITICAL:
			popup_error(str);
			break;
		case LOG_WARNING:
		case LOG_MESSAGE:
			popup_warning(str);
			break;
		default:
			break;
		}

	}
}



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

/* File -> Search Disk for Dictionaries...
 *
 * One step: pick a folder, and the whole tree below it is scanned for
 * EPWING/EB books.  The original program (2002) asked the user to paste a
 * path and choose a small numeric depth because disks were slow; on a modern
 * SSD a full recursive scan is both fast and what people actually want.
 * The settings-page entry still offers an explicit Depth, using the same
 * depth semantics (0 = unlimited).  Both share scan_dictionaries(). */

#include "defs.h"
#include "global.h"

#include "dialog.h"
#include "dictbar.h"
#include "disksearch.h"
#include "pref_dictgroup.h"
#include "pref_io.h"

#include <glib/gstdio.h>

void show_disk_search_dialog(void)
{
	GtkWidget *chooser;
	gchar *dirname;
	gchar *msg;
	gint found;
	gint skipped = 0;

	chooser = gtk_file_chooser_dialog_new(
		_("Select Folder Containing Dictionaries"),
		GTK_WINDOW(main_window),
		GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
		GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
		GTK_STOCK_OPEN, GTK_RESPONSE_ACCEPT,
		NULL);

	gtk_file_chooser_set_local_only(GTK_FILE_CHOOSER(chooser), TRUE);
	gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(chooser),
					   g_get_home_dir());

	if(gtk_dialog_run(GTK_DIALOG(chooser)) != GTK_RESPONSE_ACCEPT){
		gtk_widget_destroy(chooser);
		return;
	}

	dirname = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser));
	gtk_widget_destroy(chooser);

	if(dirname == NULL)
		return;

	if(g_file_test(dirname, G_FILE_TEST_IS_DIR) != TRUE){
		popup_warning(_("Directory does not exist. Check the path."));
		g_free(dirname);
		return;
	}

	/* Flush pending redraws so the watch cursor is visible during the
	 * (blocking) scan. */
	while(gtk_events_pending())
		gtk_main_iteration();

	/* 0 = recurse into everything below the selected folder. */
	found = scan_dictionaries(dirname, 0, main_window, &skipped);

	if(found == 0){
		popup_warning(_("No dictionary was found. Check the path and "
				"the search depth."));
	} else {
		/* Persist immediately: unlike the settings page there is no Ok
		 * button to trigger pref_end_dictgroup() later. */
		save_dictgroup();
		/* Also brings the dictionary bar (note_bar page 0) to the front,
		 * so the new group is visible instead of the File Search bar. */
		update_dict_bar();

		if(skipped > 0){
			msg = g_strdup_printf(
				_("Added %d dictionaries. %d could not be used "
				  "(no search method, or the book could not be read) "
				  "and were skipped."),
				found, skipped);
		} else {
			msg = g_strdup_printf(_("Added %d dictionaries."), found);
		}
		popup_warning(msg);
		g_free(msg);
	}

	g_free(dirname);
}

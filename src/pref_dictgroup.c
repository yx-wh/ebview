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
#include "eb.h"
#include "xml.h"
#include "dialog.h"
#include "dictbar.h"
#include "jcode.h"
#include "pref_io.h"
#include "dirtree.h"

#include <glib/gstdio.h>
#include <sys/stat.h>
#include <gdk/gdkkeysyms.h>

static GtkWidget *dict_view;
static GtkWidget *entry_group_name;
static GtkWidget *entry_directory_name;
static GtkWidget *spin_search_depth;
static GtkWidget *entry_title;
static GtkWidget *label_book_path;
static GtkWidget *entry_appendix_path;
static GtkWidget *entry_subbook_no;
static GtkWidget *entry_appendix_subbook_no;
static GtkWidget *label_color;
static GtkWidget *colorsel_dlg;;

static gchar fg_color[16];
static gchar bg_color[16];
static gint color_no;

static GtkTreeIter last_iter;
static BOOK_INFO *last_binfo=NULL;
static gboolean last_active;
static gboolean edited=FALSE;
static gboolean rewinding=FALSE;
/* Selection as it was when the mouse button went down, used to restore a
 * multi-row selection that GtkTreeView collapses at the start of a drag. */
static GList *pressed_paths=NULL;
/* Set while rows are being deleted in bulk.  Every gtk_tree_store_remove()
 * emits "changed", and the handler would otherwise write back to last_iter --
 * which may be one of the rows just freed, hence the segfault on multi-row
 * Remove. */
static gboolean removing_rows=FALSE;

extern GtkWidget *pref_dlg;
extern GtkWidget *web_view;

enum
{
	BOOKLIST_TITLE_COLUMN,
	BOOKLIST_PATH_COLUMN,
	BOOKLIST_SUBBOOK_NO_COLUMN,
	BOOKLIST_N_COLUMNS
};



GList *book_list=NULL;

void print_dict_group();
void my_gtk_tree_store_swap (GtkTreeStore *tree_store, GtkTreeIter  *a, GtkTreeIter  *b);
static gboolean update_last_dictionary();
static void activate_disk_search_group(void);
static void expand_disk_search_group(void);
static GList *selected_paths_reversed(GtkTreeSelection *selection);
static GtkTreePath *dict_parent_path(GtkTreePath *path);
static void remove_item(GtkWidget *widget, gpointer *data);
static void up_item(GtkWidget *widget, gpointer *data);
static void down_item(GtkWidget *widget, gpointer *data);

extern GtkWidget *combo_method;

extern void remove_space(gchar *f);

/* --- Cancel support -------------------------------------------------------
 * The page edits the global dict_store in place, so Cancel has to rebuild it.
 * A snapshot of every row is taken when the page is built and put back if the
 * user cancels; nothing is written to dictgroup.xml on that path.  Only the
 * column values are copied -- BOOK_INFO pointers are reused, the books stay
 * loaded, so restoring does not re-bind 50 books. */

typedef struct {
	gint type;
	gchar *title;
	gchar *path;
	gint subbook_no;
	gchar *appendix_path;
	gint appendix_subbook_no;
	gint active;
	BOOK_INFO *binfo;
	gint editable;
	gchar *fg;
	gchar *bg;
	gint available;
} DICT_ROW_SNAPSHOT;

typedef struct {
	DICT_ROW_SNAPSHOT *row;
	GList *children;
} DICT_GROUP_SNAPSHOT;

static GList *dict_snapshot = NULL;

static DICT_ROW_SNAPSHOT *dict_row_snapshot_new(GtkTreeIter *iter)
{
	DICT_ROW_SNAPSHOT *s = g_new0(DICT_ROW_SNAPSHOT, 1);

	gtk_tree_model_get(GTK_TREE_MODEL(dict_store), iter,
			   DICT_TYPE_COLUMN, &s->type,
			   DICT_TITLE_COLUMN, &s->title,
			   DICT_PATH_COLUMN, &s->path,
			   DICT_SUBBOOK_NO_COLUMN, &s->subbook_no,
			   DICT_APPENDIX_PATH_COLUMN, &s->appendix_path,
			   DICT_APPENDIX_SUBBOOK_NO_COLUMN, &s->appendix_subbook_no,
			   DICT_ACTIVE_COLUMN, &s->active,
			   DICT_MEMBER_COLUMN, &s->binfo,
			   DICT_EDITABLE_COLUMN, &s->editable,
			   DICT_FGCOLOR_COLUMN, &s->fg,
			   DICT_BGCOLOR_COLUMN, &s->bg,
			   DICT_AVAILABLE_COLUMN, &s->available,
			   -1);
	return(s);
}

static void dict_row_snapshot_free(DICT_ROW_SNAPSHOT *s)
{
	if(s == NULL)
		return;
	g_free(s->title);
	g_free(s->path);
	g_free(s->appendix_path);
	g_free(s->fg);
	g_free(s->bg);
	g_free(s);
}

static void dict_snapshot_free(void)
{
	GList *g, *c;

	for(g = dict_snapshot; g != NULL; g = g->next){
		DICT_GROUP_SNAPSHOT *gs = (DICT_GROUP_SNAPSHOT *)g->data;

		dict_row_snapshot_free(gs->row);
		for(c = gs->children; c != NULL; c = c->next)
			dict_row_snapshot_free((DICT_ROW_SNAPSHOT *)c->data);
		g_list_free(gs->children);
		g_free(gs);
	}
	g_list_free(dict_snapshot);
	dict_snapshot = NULL;
}

static void dict_snapshot_save(void)
{
	GtkTreeIter group, child;

	dict_snapshot_free();

	if(gtk_tree_model_get_iter_first(GTK_TREE_MODEL(dict_store), &group) == FALSE)
		return;

	do {
		DICT_GROUP_SNAPSHOT *gs = g_new0(DICT_GROUP_SNAPSHOT, 1);

		gs->row = dict_row_snapshot_new(&group);
		if(gtk_tree_model_iter_children(GTK_TREE_MODEL(dict_store),
						&child, &group) == TRUE){
			do {
				gs->children = g_list_append(
					gs->children,
					dict_row_snapshot_new(&child));
			} while(gtk_tree_model_iter_next(GTK_TREE_MODEL(dict_store),
							 &child) == TRUE);
		}
		dict_snapshot = g_list_append(dict_snapshot, gs);
	} while(gtk_tree_model_iter_next(GTK_TREE_MODEL(dict_store),
					 &group) == TRUE);
}

static void dict_snapshot_set(GtkTreeIter *iter, DICT_ROW_SNAPSHOT *s)
{
	gtk_tree_store_set(dict_store, iter,
			   DICT_TYPE_COLUMN, s->type,
			   DICT_TITLE_COLUMN, s->title,
			   DICT_PATH_COLUMN, s->path,
			   DICT_SUBBOOK_NO_COLUMN, s->subbook_no,
			   DICT_APPENDIX_PATH_COLUMN, s->appendix_path,
			   DICT_APPENDIX_SUBBOOK_NO_COLUMN, s->appendix_subbook_no,
			   DICT_ACTIVE_COLUMN, s->active,
			   DICT_MEMBER_COLUMN, s->binfo,
			   DICT_EDITABLE_COLUMN, s->editable,
			   DICT_FGCOLOR_COLUMN, s->fg,
			   DICT_BGCOLOR_COLUMN, s->bg,
			   DICT_AVAILABLE_COLUMN, s->available,
			   -1);
}

/* Put the tree back the way it was when the page was built. */
void pref_cancel_dictgroup()
{
	GList *g, *c;
	GtkTreeIter group, child;

	LOG(LOG_DEBUG, "IN : pref_cancel_dictgroup()");

	if(dict_store == NULL)
		return;

	/* Nothing was captured (page never built): do not clear the list. */
	if(dict_snapshot == NULL)
		return;

	removing_rows = TRUE;
	gtk_tree_store_clear(dict_store);

	for(g = dict_snapshot; g != NULL; g = g->next){
		DICT_GROUP_SNAPSHOT *gs = (DICT_GROUP_SNAPSHOT *)g->data;

		gtk_tree_store_append(dict_store, &group, NULL);
		dict_snapshot_set(&group, gs->row);
		for(c = gs->children; c != NULL; c = c->next){
			gtk_tree_store_append(dict_store, &child, &group);
			dict_snapshot_set(&child, (DICT_ROW_SNAPSHOT *)c->data);
		}
	}
	removing_rows = FALSE;

	edited = FALSE;
	last_binfo = NULL;

	if(dict_view != NULL)
		gtk_tree_view_expand_all(GTK_TREE_VIEW(dict_view));

	LOG(LOG_DEBUG, "OUT : pref_cancel_dictgroup()");
}

gboolean pref_end_dictgroup()
{

#if 0
	GtkTreeIter parent_iter;
	gchar *title;
#endif

	LOG(LOG_DEBUG, "IN : pref_end_dictgroup()");

	if(update_last_dictionary() == FALSE)
		return(FALSE);

#if 0
	// "Disk Search Result" というグループがあったら削除する
	if(gtk_tree_model_get_iter_first(GTK_TREE_MODEL(dict_store), &parent_iter) == TRUE){
		do { 
			gtk_tree_model_get(GTK_TREE_MODEL(dict_store), 
					   &parent_iter, 
					   DICT_TITLE_COLUMN, &title,
					   -1);
			
			if(strcmp(title, "Disk Search Result") == 0){
				g_free(title);
				gtk_tree_store_remove(GTK_TREE_STORE(dict_store), &parent_iter);
				break;
			}
			g_free(title);
		} while (gtk_tree_model_iter_next(GTK_TREE_MODEL(dict_store), &parent_iter) == TRUE);
	}
#endif

	save_dictgroup();
	update_dict_bar();

	/* Committed: the snapshot taken when the page was built is stale. */
	dict_snapshot_free();

	LOG(LOG_DEBUG, "OUT : pref_end_dictgroup()");
	return(TRUE);
}

#if 0
static void add_dict(GtkWidget *widget,gpointer *data)
{
	gint type;
	const gchar *title;
	const gchar *path;
	gint subbook_no;
	gchar *appendix_path=NULL;
	gint appendix_subbook_no=0;

	BOOK_INFO *binfo=NULL;


	GtkTreeIter iter;
	GtkTreeIter child_iter;
	GtkTreeSelection *selection;
	GtkTreePath *tree_path;

	LOG(LOG_DEBUG, "IN : add_dict()");

	selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(booklist_view));

	if (gtk_tree_selection_get_selected(selection, NULL, &iter))
	{
		gtk_tree_model_get(GTK_TREE_MODEL(booklist_store), 
				   &iter, 
				   BOOKLIST_TITLE_COLUMN, &title,
				   BOOKLIST_PATH_COLUMN, &path,
				   BOOKLIST_SUBBOOK_NO_COLUMN, &subbook_no,
				   -1);
	}


	selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(dict_view));
	if (gtk_tree_selection_get_selected(selection, NULL, &iter) == FALSE){
		popup_warning(_("Please select group."));
		LOG(LOG_DEBUG, "OUT : add_dict()");
		return;
	}
		

	gtk_tree_model_get(GTK_TREE_MODEL(dict_store), 
			   &iter, 
			   DICT_TYPE_COLUMN, &type,
			   -1);

	//binfo = load_book(path, atoi(subbook_no));
	binfo = load_book(path, subbook_no,
			  appendix_path, appendix_subbook_no, NULL, NULL);
	if(binfo == NULL){
		LOG(LOG_CRITICAL, _("Failed to load dictionary."));
		return;
	}

	if(type == 0){
		gtk_tree_store_append(dict_store, &child_iter, &iter);
	} else {
		gtk_tree_store_insert_after(dict_store, &child_iter, NULL, &iter);
	}

	gtk_tree_store_set (dict_store, &child_iter,
			    DICT_TYPE_COLUMN, 1,
			    DICT_TITLE_COLUMN, title,
			    DICT_ACTIVE_COLUMN, FALSE,
			    DICT_MEMBER_COLUMN, binfo,
			    DICT_FGCOLOR_COLUMN, NULL,
			    DICT_BGCOLOR_COLUMN, NULL,
			    -1);

	tree_path = gtk_tree_model_get_path(GTK_TREE_MODEL(dict_store), &iter);

	gtk_tree_view_expand_row(GTK_TREE_VIEW(dict_view), tree_path, FALSE);
	gtk_tree_path_free (tree_path);

	LOG(LOG_DEBUG, "OUT : add_dict()");
}
#endif

static void cell_edited(GtkCellRendererText *cell,
			const gchar         *path_string,
			const gchar         *new_text,
			gpointer             data)
{

	GtkTreePath *path = gtk_tree_path_new_from_string (path_string);
	GtkTreeIter iter;
	gint column;

	gchar *book_path;
	gint subbook_no;
	gchar *appendix_path=NULL;
	gint appendix_subbook_no=0;
	gchar *fs_book_path;
	gchar *fs_appendix_path;
	gchar *fg;
	gchar *bg;
	gint type;

	BOOK_INFO *binfo;

	column = GPOINTER_TO_INT(g_object_get_data(G_OBJECT (cell), "column"));

	gtk_tree_model_get_iter(GTK_TREE_MODEL(dict_store), &iter, path);

	switch(column)
	{
	case DICT_TITLE_COLUMN:
	case DICT_PATH_COLUMN:
	case DICT_APPENDIX_PATH_COLUMN:
	{
		gchar *old_text;

		gtk_tree_model_get(GTK_TREE_MODEL(dict_store), &iter, column, &old_text, -1);
		g_free (old_text);
		gtk_tree_store_set(GTK_TREE_STORE(dict_store), &iter,
				   column, new_text,
				   -1);
	}
	break;
	case DICT_SUBBOOK_NO_COLUMN:
	case DICT_APPENDIX_SUBBOOK_NO_COLUMN:
	{
		gtk_tree_store_set(GTK_TREE_STORE(dict_store), &iter,
				   column, atoi(new_text),
				   -1);
	}
	break;
	case DICT_FGCOLOR_COLUMN:
	case DICT_BGCOLOR_COLUMN:
	{
		gint i;
		gchar *old_text;

		if(new_text[0] != '#')
			return;

//		remove_space(new_text);
		if(strlen(new_text) != 7)
			return;

		if(strlen(new_text) != 0){
			for(i=1; i<7 ; i++){
				if((('0' <= new_text[i]) &&  (new_text[i] <= '9')) ||
				   (('a' <= new_text[i]) &&  (new_text[i] <= 'f'))){
				   } else {
					   return;
				   }
			}
		}

		gtk_tree_model_get(GTK_TREE_MODEL(dict_store), &iter, column, &old_text, -1);
		g_free (old_text);
		gtk_tree_store_set(GTK_TREE_STORE(dict_store), &iter,
				   column, new_text,
				   -1);
	}
	break;
	}

	gtk_tree_model_get(GTK_TREE_MODEL(dict_store), &iter, 
			   DICT_TYPE_COLUMN, &type,
			   DICT_PATH_COLUMN, &book_path,
			   DICT_SUBBOOK_NO_COLUMN, &subbook_no,
			   DICT_APPENDIX_PATH_COLUMN, &appendix_path,
			   DICT_APPENDIX_SUBBOOK_NO_COLUMN, &appendix_subbook_no,
			   DICT_MEMBER_COLUMN, &binfo,
			   DICT_FGCOLOR_COLUMN, &fg,
			   DICT_BGCOLOR_COLUMN, &bg,
			   -1);

	// Group
	if(type == 0)
		return;

//	if(binfo != NULL)
//		unload_book(binfo);

	binfo = NULL;

	if(book_path)
		fs_book_path = unicode_to_fs(book_path);
	else
		fs_book_path = NULL;

	if((appendix_path) && (strlen(appendix_path) != 0))
		fs_appendix_path = unicode_to_fs(appendix_path);
	else
		fs_appendix_path = NULL;

	binfo = load_book(fs_book_path, subbook_no,
			  fs_appendix_path, appendix_subbook_no, fg, bg);


	g_free(fs_book_path);
	g_free(fs_appendix_path);

	g_free(book_path);
	g_free(appendix_path);
	g_free(fg);
	g_free(bg);

	if(binfo == NULL){
		LOG(LOG_CRITICAL, _("Failed to load dictionary."));
		/* Keep the row (and therefore the configuration entry) but mark
		 * it unavailable, so the user does not lose it by accident. */
		gtk_tree_store_set(GTK_TREE_STORE(dict_store), &iter,
				   DICT_MEMBER_COLUMN, NULL,
				   DICT_AVAILABLE_COLUMN, FALSE,
				   -1);
		gtk_tree_path_free (path);
		return;
	}

	gtk_tree_store_set(GTK_TREE_STORE(dict_store), &iter,
			   DICT_MEMBER_COLUMN, binfo,
			   DICT_AVAILABLE_COLUMN, TRUE,
			   -1);

	gtk_tree_path_free (path);
}


/* Keyboard equivalents for the Add/Remove/Up/Down buttons.  They are bound
 * to this tree view rather than to the global shortcut table: those commands
 * only make sense while the preferences dialog is open, and the dialog is
 * modal, so main-window accelerators never fire here. */
static gboolean dict_view_key_press(GtkWidget *widget, GdkEventKey *event,
				    gpointer user_data)
{
	gboolean ctrl = (event->state & GDK_CONTROL_MASK) != 0;

	switch(event->keyval){
	case GDK_Delete:
	case GDK_KP_Delete:
		remove_item(NULL, NULL);
		return(TRUE);
	case GDK_Up:
		if(ctrl){
			up_item(NULL, NULL);
			return(TRUE);
		}
		return(FALSE);
	case GDK_Down:
		if(ctrl){
			down_item(NULL, NULL);
			return(TRUE);
		}
		return(FALSE);
	case GDK_Insert:
	case GDK_KP_Insert:
		/* Focus the name entry; Enter there runs add_group(). */
		if(entry_group_name != NULL)
			gtk_widget_grab_focus(entry_group_name);
		return(TRUE);
	default:
		return(FALSE);
	}
}

/* --- Multi-row drag -------------------------------------------------------
 * gtk_tree_view_set_reorderable() only ever drags the row under the cursor.
 * To move a whole Ctrl/Shift selection we provide our own drag source that
 * serialises every selected path, and our own dest that moves the rows.
 *
 * The payload is a NUL-separated list of path strings.  Rows are moved one by
 * one with my_gtk_tree_store_swap(), which preserves the row contents and
 * any children. */

#define DICT_DRAG_TARGET "application/x-ebview-dict-rows"

static gint dict_path_compare(gconstpointer a, gconstpointer b)
{
	return(gtk_tree_path_compare((const GtkTreePath *)a,
				     (const GtkTreePath *)b));
}

/* Ascending path order, so rows are handled top-to-bottom. */
static GList *selected_paths_ascending(GtkTreeSelection *selection)
{
	GList *rows, *node;
	GList *paths = NULL;

	rows = gtk_tree_selection_get_selected_rows(selection, NULL);
	for(node = rows; node != NULL; node = node->next)
		paths = g_list_prepend(paths, gtk_tree_path_copy(node->data));
	g_list_foreach(rows, (GFunc)gtk_tree_path_free, NULL);
	g_list_free(rows);

	paths = g_list_sort(paths, dict_path_compare);
	return(paths);
}

static void dict_drag_data_get(GtkWidget *widget, GdkDragContext *context,
			       GtkSelectionData *data, guint info, guint time,
			       gpointer user_data)
{
	GtkTreeSelection *selection;
	GList *paths, *node;
	GString *payload;

	selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(dict_view));
	paths = selected_paths_ascending(selection);

	payload = g_string_new("");
	for(node = paths; node != NULL; node = node->next){
		gchar *str = gtk_tree_path_to_string((GtkTreePath *)node->data);
		if(str != NULL){
			if(payload->len > 0)
				g_string_append_c(payload, '\0');
			g_string_append(payload, str);
			g_free(str);
		}
	}

	gtk_selection_data_set(data, gtk_selection_data_get_target(data),
			       8, (const guchar *)payload->str,
			       payload->len + 1);

	g_string_free(payload, TRUE);
	g_list_foreach(paths, (GFunc)gtk_tree_path_free, NULL);
	g_list_free(paths);
}

/* Groups are the only rows that can act as a drop container; dictionaries are
 * leaves.  Type 0 is a group, see load_dictgroup(). */
static gboolean dict_path_is_group(GtkTreePath *path)
{
	GtkTreeIter iter;
	gint type;

	if(path == NULL || gtk_tree_path_get_depth(path) != 1 ||
	   gtk_tree_model_get_iter(GTK_TREE_MODEL(dict_store), &iter, path) == FALSE)
		return(FALSE);

	gtk_tree_model_get(GTK_TREE_MODEL(dict_store), &iter,
			   DICT_TYPE_COLUMN, &type, -1);
	return(type == 0);
}

/* Depth of the rows being dragged: 2 for dictionaries, 1 for groups. */
static gint dict_drag_source_depth(void)
{
	GtkTreeSelection *selection;
	GList *rows;
	gint depth = 0;

	selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(dict_view));
	rows = gtk_tree_selection_get_selected_rows(selection, NULL);
	if(rows != NULL){
		depth = gtk_tree_path_get_depth((GtkTreePath *)rows->data);
		g_list_foreach(rows, (GFunc)gtk_tree_path_free, NULL);
		g_list_free(rows);
	}
	return(depth);
}

static gboolean dict_drag_motion(GtkWidget *widget, GdkDragContext *context,
				 gint x, gint y, guint time, gpointer user_data)
{
	GtkTreePath *path = NULL;
	GtkTreeViewDropPosition pos;

	if(gtk_tree_view_get_dest_row_at_pos(GTK_TREE_VIEW(dict_view), x, y,
					     &path, &pos) == FALSE){
		gdk_drag_status(context, 0, time);
		return(TRUE);
	}

	if(dict_drag_source_depth() == 2){
		/* Dictionaries can be reordered inside their group, or moved
		 * into another one by dropping them on its header.  The header
		 * is the only drop target an empty group offers. */
		pos = dict_path_is_group(path) ? GTK_TREE_VIEW_DROP_INTO_OR_BEFORE
					       : GTK_TREE_VIEW_DROP_BEFORE;
	} else {
		/* Groups move relative to other groups; hovering a dictionary
		 * targets the group it belongs to, so the whole child area of a
		 * group acts as its drop zone.  Which half of the row the
		 * pointer is in decides before/after. */
		GtkTreePath *group;

		group = dict_path_is_group(path) ? gtk_tree_path_copy(path)
						 : dict_parent_path(path);
		gtk_tree_path_free(path);
		if(group == NULL){
			gdk_drag_status(context, 0, time);
			return(TRUE);
		}
		if(pos != GTK_TREE_VIEW_DROP_BEFORE){
			/* Show the line at the next group's header, which is where
			 * this group will land.  GTK's AFTER indicator would be
			 * drawn between the header and its children. */
			GtkTreePath *next = gtk_tree_path_copy(group);
			GtkTreeIter tmp;

			gtk_tree_path_next(next);
			if(gtk_tree_model_get_iter(GTK_TREE_MODEL(dict_store),
						   &tmp, next)){
				gtk_tree_path_free(group);
				group = next;
				pos = GTK_TREE_VIEW_DROP_BEFORE;
			} else {
				gtk_tree_path_free(next);
				pos = GTK_TREE_VIEW_DROP_AFTER;
			}
		} else {
			pos = GTK_TREE_VIEW_DROP_BEFORE;
		}

		gtk_tree_view_set_drag_dest_row(GTK_TREE_VIEW(dict_view), group, pos);
		gdk_drag_status(context, GDK_ACTION_MOVE, time);
		gtk_tree_path_free(group);
		return(TRUE);
	}

	gtk_tree_view_set_drag_dest_row(GTK_TREE_VIEW(dict_view), path, pos);
	gdk_drag_status(context, GDK_ACTION_MOVE, time);
	gtk_tree_path_free(path);
	return(TRUE);
}

static void pressed_paths_clear(void)
{
	if(pressed_paths != NULL){
		g_list_foreach(pressed_paths, (GFunc)gtk_tree_path_free, NULL);
		g_list_free(pressed_paths);
		pressed_paths = NULL;
	}
}

/* Runs before GtkTreeView's own button-press handler, so the selection is
 * still the one the user built with Ctrl/Shift. */
static gboolean dict_view_button_press(GtkWidget *widget, GdkEventButton *event,
				       gpointer user_data)
{
	GtkTreeSelection *selection;

	pressed_paths_clear();
	if(event->button != 1)
		return(FALSE);

	selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(dict_view));
	pressed_paths = selected_paths_ascending(selection);
	return(FALSE);
}

/* Starting a drag from one row of a multi-row selection made GtkTreeView
 * collapse the selection to that row, so only one dictionary was carried.
 * Put the selection back before the drag payload is built. */
static void dict_view_drag_begin(GtkWidget *widget, GdkDragContext *context,
				 gpointer user_data)
{
	GtkTreeSelection *selection;
	GList *node;

	if(pressed_paths == NULL || g_list_length(pressed_paths) < 2)
		return;

	selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(dict_view));
	if(gtk_tree_selection_count_selected_rows(selection) ==
	   (gint)g_list_length(pressed_paths))
		return;

	gtk_tree_selection_unselect_all(selection);
	for(node = pressed_paths; node != NULL; node = node->next){
		GtkTreeIter iter;
		if(gtk_tree_model_get_iter(GTK_TREE_MODEL(dict_store), &iter,
					   (GtkTreePath *)node->data))
			gtk_tree_selection_select_iter(selection, &iter);
	}
}

static void dict_view_drag_end(GtkWidget *widget, GdkDragContext *context,
			       gpointer user_data)
{
	pressed_paths_clear();
}

static gint dict_path_index(GtkTreePath *path)
{
	gint *indices = gtk_tree_path_get_indices(path);
	return(indices[gtk_tree_path_get_depth(path) - 1]);
}

static GtkTreePath *dict_parent_path(GtkTreePath *path)
{
	GtkTreePath *parent = gtk_tree_path_copy(path);
	if(gtk_tree_path_up(parent) == FALSE){
		gtk_tree_path_free(parent);
		return(NULL);
	}
	return(parent);
}

/* Swap two sibling rows by index.  parent == NULL means the root level. */
static void dict_swap_siblings(GtkTreePath *parent, gint a, gint b)
{
	GtkTreePath *pa, *pb;
	GtkTreeIter ia, ib;

	pa = parent != NULL ? gtk_tree_path_copy(parent) : gtk_tree_path_new();
	gtk_tree_path_append_index(pa, a);
	pb = parent != NULL ? gtk_tree_path_copy(parent) : gtk_tree_path_new();
	gtk_tree_path_append_index(pb, b);

	if(gtk_tree_model_get_iter(GTK_TREE_MODEL(dict_store), &ia, pa) &&
	   gtk_tree_model_get_iter(GTK_TREE_MODEL(dict_store), &ib, pb))
		my_gtk_tree_store_swap(dict_store, &ia, &ib);

	gtk_tree_path_free(pa);
	gtk_tree_path_free(pb);
}

/* Copy one dictionary row, all columns, under a new parent. */
static void dict_copy_row(GtkTreeIter *parent, GtkTreeIter *src,
			  GtkTreeIter *dest)
{
	gint type, subbook_no, appendix_subbook_no, active, editable;
	gboolean available;
	gchar *title, *book_path, *appendix_path, *fg, *bg;
	BOOK_INFO *binfo;

	gtk_tree_model_get(GTK_TREE_MODEL(dict_store), src,
			   DICT_TYPE_COLUMN, &type,
			   DICT_TITLE_COLUMN, &title,
			   DICT_PATH_COLUMN, &book_path,
			   DICT_SUBBOOK_NO_COLUMN, &subbook_no,
			   DICT_APPENDIX_PATH_COLUMN, &appendix_path,
			   DICT_APPENDIX_SUBBOOK_NO_COLUMN, &appendix_subbook_no,
			   DICT_ACTIVE_COLUMN, &active,
			   DICT_MEMBER_COLUMN, &binfo,
			   DICT_EDITABLE_COLUMN, &editable,
			   DICT_FGCOLOR_COLUMN, &fg,
			   DICT_BGCOLOR_COLUMN, &bg,
			   DICT_AVAILABLE_COLUMN, &available,
			   -1);

	gtk_tree_store_append(dict_store, dest, parent);
	gtk_tree_store_set(dict_store, dest,
			   DICT_TYPE_COLUMN, type,
			   DICT_TITLE_COLUMN, title,
			   DICT_PATH_COLUMN, book_path,
			   DICT_SUBBOOK_NO_COLUMN, subbook_no,
			   DICT_APPENDIX_PATH_COLUMN, appendix_path,
			   DICT_APPENDIX_SUBBOOK_NO_COLUMN, appendix_subbook_no,
			   DICT_ACTIVE_COLUMN, active,
			   DICT_MEMBER_COLUMN, binfo,
			   DICT_EDITABLE_COLUMN, editable,
			   DICT_FGCOLOR_COLUMN, fg,
			   DICT_BGCOLOR_COLUMN, bg,
			   DICT_AVAILABLE_COLUMN, available,
			   -1);

	g_free(title);
	g_free(book_path);
	g_free(appendix_path);
	g_free(fg);
	g_free(bg);
}

/* Move the dragged dictionaries into a group, keeping their order.  Dropping
 * on the group header is the only way to fill a group that has no children
 * yet -- an empty group offers no row to drop *before* -- and the swap-based
 * reordering below cannot change a row's parent. */
static gboolean dict_move_rows_into_group(GList *src_paths,
					  GtkTreePath *dest_group)
{
	GtkTreeIter group_iter, iter, new_iter;
	GtkTreePath *src_parent;
	GList *node, *added = NULL, *src_iters = NULL;
	gboolean moved = FALSE;

	if(gtk_tree_model_get_iter(GTK_TREE_MODEL(dict_store),
				   &group_iter, dest_group) == FALSE)
		return(FALSE);

	/* Resolve the source rows to iters before touching anything: deleting a
	 * row shifts the indices of every path after it in the same group,
	 * which is why dragging three dictionaries used to move only some of
	 * them.  GtkTreeIters stay valid while other rows are removed. */
	for(node = src_paths; node != NULL; node = node->next){
		src_parent = dict_parent_path((GtkTreePath *)node->data);
		if(src_parent != NULL &&
		   gtk_tree_path_compare(src_parent, dest_group) == 0){
			/* Dropped on the header of its own group: already there. */
			gtk_tree_path_free(src_parent);
			continue;
		}
		gtk_tree_path_free(src_parent);

		if(gtk_tree_model_get_iter(GTK_TREE_MODEL(dict_store), &iter,
					   (GtkTreePath *)node->data))
			src_iters = g_list_append(src_iters,
						  gtk_tree_iter_copy(&iter));
	}

	removing_rows = TRUE;
	for(node = src_iters; node != NULL; node = node->next){
		dict_copy_row(&group_iter, (GtkTreeIter *)node->data, &new_iter);
		added = g_list_append(added, gtk_tree_iter_copy(&new_iter));
	}
	for(node = src_iters; node != NULL; node = node->next){
		gtk_tree_store_remove(dict_store, (GtkTreeIter *)node->data);
		moved = TRUE;
	}
	removing_rows = FALSE;

	g_list_foreach(src_iters, (GFunc)gtk_tree_iter_free, NULL);
	g_list_free(src_iters);

	/* last_iter pointed at one of the rows just deleted. */
	edited = FALSE;
	last_binfo = NULL;

	if(added != NULL){
		GtkTreeSelection *selection =
			gtk_tree_view_get_selection(GTK_TREE_VIEW(dict_view));

		/* A group that had no children yet has no expander, and a newly
		 * inserted child would start out collapsed: expand so the moved
		 * rows are actually visible. */
		gtk_tree_view_expand_row(GTK_TREE_VIEW(dict_view), dest_group, FALSE);
		/* The rows were copied, so the old selection points at deleted
		 * rows; select the new ones to keep Up/Down working on them. */
		gtk_tree_selection_unselect_all(selection);
		for(node = added; node != NULL; node = node->next)
			gtk_tree_selection_select_iter(selection,
						       (GtkTreeIter *)node->data);
		g_list_foreach(added, (GFunc)gtk_tree_iter_free, NULL);
		g_list_free(added);
	}

	return(moved);
}

/* Move the dragged block so that it sits where it was dropped.  All rows must
 * share the same parent.  The new order is computed up front and then applied
 * with gtk_tree_store_swap(), which also moves children of group rows. */
static void dict_drag_data_received(GtkWidget *widget, GdkDragContext *context,
				    gint x, gint y,
				    GtkSelectionData *data, guint info,
				    guint time, gpointer user_data)
{
	const gchar *p;
	GList *src_paths = NULL, *node;
	GtkTreePath *dest_path = NULL, *parent = NULL, *src_parent = NULL;
	GtkTreeViewDropPosition pos;
	gint n, i, k, dest_index, insert_at, written;
	gint *order = NULL, *desired = NULL;
	gboolean *selected = NULL;
	gboolean ok = FALSE;
	gboolean src_is_dict = FALSE;

	if(gtk_selection_data_get_length(data) <= 0 ||
	   gtk_selection_data_get_format(data) != 8)
		goto DONE;

	p = (const gchar *)gtk_selection_data_get_data(data);
	while(p != NULL && *p != '\0'){
		src_paths = g_list_append(src_paths,
					  gtk_tree_path_new_from_string(p));
		p = p + strlen(p) + 1;
	}
	if(src_paths == NULL)
		goto DONE;

	if(gtk_tree_view_get_dest_row_at_pos(GTK_TREE_VIEW(dict_view), x, y,
					     &dest_path, &pos) == FALSE)
		goto DONE;

	src_is_dict = (gtk_tree_path_get_depth((GtkTreePath *)src_paths->data)
		       == 2);

	/* Dropped on a group header: move the dictionaries into that group.
	 * This is a change of parent, so it takes a different path than the
	 * sibling reordering below. */
	if(src_is_dict && dict_path_is_group(dest_path)){
		ok = dict_move_rows_into_group(src_paths, dest_path);
		goto DONE;
	}

	if(src_is_dict){
		/* Reordering inside one group only: moving a dictionary to a
		 * different group would silently change what it belongs to. */
		if(gtk_tree_path_get_depth(dest_path) != 2)
			goto DONE;
		parent = dict_parent_path(dest_path);
		src_parent = dict_parent_path((GtkTreePath *)src_paths->data);
		if(parent == NULL || src_parent == NULL ||
		   gtk_tree_path_compare(parent, src_parent) != 0)
			goto DONE;
		dest_index = dict_path_index(dest_path);
	} else {
		/* A group is one unit: dropping anywhere inside a group targets
		 * that group, and which half of the row the pointer is in
		 * decides before/after.  Otherwise a group could only be
		 * dropped on another group's one-line header. */
		GtkTreePath *group;

		group = dict_path_is_group(dest_path)
			? gtk_tree_path_copy(dest_path)
			: dict_parent_path(dest_path);
		if(group == NULL)
			goto DONE;
		dest_index = dict_path_index(group);
		if(pos == GTK_TREE_VIEW_DROP_AFTER)
			dest_index++;
		gtk_tree_path_free(group);
		parent = NULL;
	}

	if(parent != NULL){
		GtkTreeIter piter;
		if(gtk_tree_model_get_iter(GTK_TREE_MODEL(dict_store),
					   &piter, parent) == FALSE)
			goto DONE;
		n = gtk_tree_model_iter_n_children(GTK_TREE_MODEL(dict_store),
						   &piter);
	} else {
		n = gtk_tree_model_iter_n_children(GTK_TREE_MODEL(dict_store),
						   NULL);
	}

	if(n <= 0)
		goto DONE;

	order = g_new0(gint, n);
	desired = g_new0(gint, n);
	selected = g_new0(gboolean, n);
	for(i = 0; i < n; i++)
		order[i] = i;

	for(node = src_paths; node != NULL; node = node->next){
		gint idx = dict_path_index((GtkTreePath *)node->data);
		if(idx >= 0 && idx < n)
			selected[idx] = TRUE;
	}

	/* dest_index == n means "after the last row". */
	if(dest_index < 0 || dest_index > n)
		goto DONE;
	if(dest_index < n && selected[dest_index])
		goto DONE;	/* dropped onto itself */

	/* Rows before the destination that stay put, then the block, then the
	 * rest; this is what "insert the selection here" means. */
	insert_at = 0;
	for(i = 0; i < dest_index; i++)
		if(selected[i] == FALSE)
			insert_at++;

	written = 0;
	for(i = 0; i < n; i++){
		if(selected[i])
			continue;
		if(written == insert_at){
			for(k = 0; k < n; k++)
				if(selected[k])
					desired[written++] = k;
		}
		desired[written++] = i;
	}
	while(written < n){
		for(k = 0; k < n; k++)
			if(selected[k])
				desired[written++] = k;
	}

	/* Transform [0..n-1] into `desired` using swaps. */
	for(i = 0; i < n; i++){
		gint want = desired[i];
		gint at = -1;
		for(k = i; k < n; k++)
			if(order[k] == want){
				at = k;
				break;
			}
		if(at < 0 || at == i)
			continue;
		dict_swap_siblings(parent, i, at);
		k = order[i];
		order[i] = order[at];
		order[at] = k;
	}

	ok = TRUE;

 DONE:
	gtk_drag_finish(context, ok, FALSE, time);
	if(parent != NULL)
		gtk_tree_path_free(parent);
	if(src_parent != NULL)
		gtk_tree_path_free(src_parent);
	if(dest_path != NULL)
		gtk_tree_path_free(dest_path);
	g_free(order);
	g_free(desired);
	g_free(selected);
	if(src_paths != NULL){
		g_list_foreach(src_paths, (GFunc)gtk_tree_path_free, NULL);
		g_list_free(src_paths);
	}
}

/* gtk_tree_selection_get_selected() only fills the iter in SINGLE/BROWSE
 * mode.  Both tree views are GTK_SELECTION_MULTIPLE now, so it returned FALSE
 * even with a row selected -- which is why Up/Down kept asking to select a
 * dictionary.  Take the first selected row instead. */
static gboolean selection_first_iter(GtkTreeSelection *selection,
				     GtkTreeModel *model, GtkTreeIter *iter)
{
	GList *rows;
	gboolean found = FALSE;

	rows = gtk_tree_selection_get_selected_rows(selection, NULL);
	if(rows != NULL){
		found = gtk_tree_model_get_iter(model, iter,
						(GtkTreePath *)rows->data);
		g_list_foreach(rows, (GFunc)gtk_tree_path_free, NULL);
		g_list_free(rows);
	}
	return(found);
}

static gboolean row_drop_possible (GtkTreeDragDest   *drag_dest,
                                       GtkTreePath       *dest,
                                       GtkSelectionData  *selection_data)
{
	GtkTreeSelection *selection;
	GtkTreeIter iter;
	GtkTreePath *src;

	LOG(LOG_DEBUG, "IN : row_drop_possible()");

	if(dest == NULL)
		return(FALSE);

	if(GTK_TREE_STORE(drag_dest) == GTK_TREE_STORE(dict_store))
		selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(dict_view));
	else if(GTK_TREE_STORE(drag_dest) == GTK_TREE_STORE(web_store))
		selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(web_view));
	else
		return(FALSE);

	if(selection_first_iter(selection, GTK_TREE_MODEL(drag_dest),
				&iter) == FALSE)
		return(FALSE);

	src = gtk_tree_model_get_path(GTK_TREE_MODEL(drag_dest), &iter);

/*
	printf("src = %s, dest = %s\n", 
	       gtk_tree_path_to_string(src),
	       gtk_tree_path_to_string(dest));
*/

	if(gtk_tree_path_get_depth(src) == gtk_tree_path_get_depth(dest))
		return(TRUE);

	/* A dictionary may be dropped on a group header, which moves it into
	 * that group.  This is the only way to put something into a group that
	 * has no children yet. */
	if(gtk_tree_path_get_depth(src) == 2 && dict_path_is_group(dest))
		return(TRUE);

	/* A group dropped on a dictionary targets that dictionary's group. */
	if(gtk_tree_path_get_depth(src) == 1 &&
	   gtk_tree_path_get_depth(dest) == 2)
		return(TRUE);

	return(FALSE);

}

/* Detach from *list every path that shares a parent with `ref`, keeping their
 * order.  The caller owns the returned list of paths. */
static GList *take_same_parent(GList **list, GtkTreePath *ref)
{
	GList *taken = NULL, *node;
	GtkTreePath *ref_parent = dict_parent_path(ref);

	node = *list;
	while(node != NULL){
		GList *next = node->next;
		GtkTreePath *path = (GtkTreePath *)node->data;
		GtkTreePath *parent = dict_parent_path(path);
		gboolean same;

		if(ref_parent == NULL || parent == NULL)
			same = (ref_parent == NULL && parent == NULL);
		else
			same = (gtk_tree_path_compare(ref_parent, parent) == 0);

		if(parent != NULL)
			gtk_tree_path_free(parent);

		if(same){
			taken = g_list_append(taken, path);
			*list = g_list_delete_link(*list, node);
		}
		node = next;
	}

	if(ref_parent != NULL)
		gtk_tree_path_free(ref_parent);

	return(taken);
}

/* Move one block of same-parent rows one slot up or down.
 *
 * Order matters.  Since each step swaps two sibling nodes, the row being
 * swapped must be visited in the direction of travel: moving up must start
 * from the topmost row, moving down from the bottom-most.  Doing it the other
 * way lets the last row be swapped again and again, which is what made a
 * block "spread out" before moving. */
static void move_block_in_list(GList *block, gboolean up)
{
	GList *node;
	GtkTreeIter iter, other_iter;
	GtkTreePath *other;
	gboolean moved_first = FALSE;

	if(block == NULL)
		return;

	/* If the leading row cannot move, this block is already against the
	 * edge; moving only part of it would look broken. */
	other = gtk_tree_path_copy((GtkTreePath *)block->data);
	if(up){
		moved_first = gtk_tree_path_prev(other);
	} else {
		/* gtk_tree_path_next() returns void; an out-of-range index is
		 * caught by the get_iter() check below. */
		gtk_tree_path_next(other);
		moved_first = TRUE;
	}
	if(moved_first == FALSE ||
	   gtk_tree_path_get_depth(other) !=
	   gtk_tree_path_get_depth((GtkTreePath *)block->data) ||
	   gtk_tree_model_get_iter(GTK_TREE_MODEL(dict_store),
				   &other_iter, other) == FALSE){
		gtk_tree_path_free(other);
		return;
	}
	gtk_tree_path_free(other);

	for(node = block; node != NULL; node = node->next){
		other = gtk_tree_path_copy((GtkTreePath *)node->data);
		if(up){
			moved_first = gtk_tree_path_prev(other);
		} else {
			gtk_tree_path_next(other);
			moved_first = TRUE;
		}

		if(moved_first == FALSE ||
		   gtk_tree_path_get_depth(other) !=
		   gtk_tree_path_get_depth((GtkTreePath *)node->data) ||
		   gtk_tree_model_get_iter(GTK_TREE_MODEL(dict_store),
					   &other_iter, other) == FALSE ||
		   gtk_tree_model_get_iter(GTK_TREE_MODEL(dict_store), &iter,
					   (GtkTreePath *)node->data) == FALSE){
			gtk_tree_path_free(other);
			continue;
		}
		my_gtk_tree_store_swap(dict_store, &iter, &other_iter);
		gtk_tree_path_free(other);
	}
}

/* Move a multi-row selection one slot up or down.
 *
 * Rows sharing a parent are one block; rows in different groups move on their
 * own.  A single global block made a dictionary that had reached the top of
 * its own group freeze every other group in the selection.
 *
 * Deeper levels are handled first: swapping two group rows also rewrites the
 * paths of their children, so the dictionary paths have to be used before the
 * groups are touched. */
static void move_selection_block(GtkTreeSelection *selection, gboolean up)
{
	GList *paths, *block, *node;
	gint max_depth = 1, level;

	/* Up: top-down (ascending).  Down: bottom-up (descending). */
	paths = up ? selected_paths_ascending(selection)
		   : selected_paths_reversed(selection);

	for(node = paths; node != NULL; node = node->next){
		gint depth = gtk_tree_path_get_depth((GtkTreePath *)node->data);
		if(depth > max_depth)
			max_depth = depth;
	}

	for(level = max_depth; level >= 1; level--){
		while(TRUE){
			GList *first = NULL;

			for(node = paths; node != NULL; node = node->next){
				if(gtk_tree_path_get_depth((GtkTreePath *)node->data)
				   == level){
					first = node;
					break;
				}
			}
			if(first == NULL)
				break;

			block = take_same_parent(&paths,
						 (GtkTreePath *)first->data);
			move_block_in_list(block, up);
			g_list_foreach(block, (GFunc)gtk_tree_path_free, NULL);
			g_list_free(block);
		}
	}
}

static gint selection_count(GtkTreeSelection *selection)
{
	return(gtk_tree_selection_count_selected_rows(selection));
}

/* In MULTIPLE mode select_iter() only *adds* to the selection.  Re-selecting
 * a moved row without clearing would leave two rows highlighted, and the next
 * Up/Down would suddenly act on a two-row block. */
static void selection_select_only(GtkTreeSelection *selection, GtkTreeIter *iter)
{
	gtk_tree_selection_unselect_all(selection);
	gtk_tree_selection_select_iter(selection, iter);
}

static void up_item(GtkWidget *widget,gpointer *data)
{
	GtkTreeIter iter;
	GtkTreeIter prev_iter;
	GtkTreeSelection *selection;
	GtkTreePath *path;
	GtkTreePath *path_orig;

	LOG(LOG_DEBUG, "IN : up_item()");

	edited = FALSE;


	selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(dict_view));

	if(selection_count(selection) > 1){
		move_selection_block(selection, TRUE);
		LOG(LOG_DEBUG, "OUT : up_item() (block)");
		return;
	}

	if (selection_first_iter(selection, GTK_TREE_MODEL(dict_store),
				 &iter) == FALSE)
	{
		popup_warning(_("Please select dictionary."));
		return;
	}

	path = gtk_tree_model_get_path(GTK_TREE_MODEL(dict_store), &iter);
	path_orig = gtk_tree_path_copy(path);

	gtk_tree_path_prev(path);
	if((gtk_tree_path_compare(path, path_orig) != 0) &&
	   gtk_tree_model_get_iter(GTK_TREE_MODEL(dict_store),
				   &prev_iter,
				   path))
	{
		my_gtk_tree_store_swap(dict_store, &iter, &prev_iter);
		selection_select_only(gtk_tree_view_get_selection(GTK_TREE_VIEW(dict_view)),
					       &iter);

	} else if(gtk_tree_path_get_depth(path) > 1){
		gtk_tree_path_up(path);
		if(gtk_tree_path_compare(path, path_orig) != 0){
			gtk_tree_path_free(path_orig);
			path_orig = gtk_tree_path_copy(path);
			gtk_tree_path_prev(path);
			if(gtk_tree_path_compare(path, path_orig) != 0){
				GtkTreeIter parent;
				if(gtk_tree_model_get_iter(GTK_TREE_MODEL(dict_store),
							   &parent,
							   path))
				{
					
					GtkTreeIter new_iter;
					gint type;
					gchar *title;
					gchar *book_path;
					gint subbook_no;
					gchar *appendix_path;
					gint appendix_subbook_no;
					gboolean active;
					BOOK_INFO *binfo;
					gboolean editable;
					gboolean available;
					gchar *fg, *bg;


					gtk_tree_store_append(GTK_TREE_STORE(dict_store),
							      &new_iter,
							      &parent);


					gtk_tree_model_get(GTK_TREE_MODEL(dict_store), 
							   &iter, 
							   DICT_TYPE_COLUMN, &type,
							   DICT_TITLE_COLUMN, &title,
							   DICT_PATH_COLUMN, &book_path,
							   DICT_SUBBOOK_NO_COLUMN, &subbook_no,
							   DICT_APPENDIX_PATH_COLUMN, &appendix_path,
							   DICT_APPENDIX_SUBBOOK_NO_COLUMN, &appendix_subbook_no,
							   DICT_ACTIVE_COLUMN, &active,
							   DICT_MEMBER_COLUMN, &binfo,
							   DICT_EDITABLE_COLUMN, &editable,
							   DICT_FGCOLOR_COLUMN, &fg,
							   DICT_BGCOLOR_COLUMN, &bg,
							   DICT_AVAILABLE_COLUMN, &available,
							   -1);

					gtk_tree_store_set(dict_store, &new_iter,
							   DICT_TYPE_COLUMN, type,
							   DICT_TITLE_COLUMN, title,
							   DICT_PATH_COLUMN, book_path,
							   DICT_SUBBOOK_NO_COLUMN, subbook_no,
							   DICT_APPENDIX_PATH_COLUMN, appendix_path,
							   DICT_APPENDIX_SUBBOOK_NO_COLUMN, appendix_subbook_no,
							   DICT_ACTIVE_COLUMN, active,
							   DICT_MEMBER_COLUMN, binfo,
							   DICT_EDITABLE_COLUMN, editable,
							   DICT_FGCOLOR_COLUMN, fg,
							   DICT_BGCOLOR_COLUMN, bg,
							   DICT_AVAILABLE_COLUMN, available,

							   -1);
					g_free(title);
					g_free(book_path);
					g_free(appendix_path);
					g_free(fg);
					g_free(bg);

					gtk_tree_store_remove(GTK_TREE_STORE(dict_store),
							      &iter);

					gtk_tree_view_expand_row(GTK_TREE_VIEW(dict_view), path, TRUE);
					selection_select_only(gtk_tree_view_get_selection(GTK_TREE_VIEW(dict_view)),
								       &new_iter);

				}
			}
		}
	}

	gtk_tree_path_free(path);
	gtk_tree_path_free(path_orig);

	LOG(LOG_DEBUG, "OUT : up_item()");
	return;
}


static void down_item(GtkWidget *widget,gpointer *data)
{
	GtkTreeIter iter;
	GtkTreeIter next_iter;
	GtkTreeSelection *selection;
	GtkTreePath *path;
	GtkTreePath *path_orig;

	LOG(LOG_DEBUG, "IN : down_item()");

	edited = FALSE;


	selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(dict_view));

	if(selection_count(selection) > 1){
		move_selection_block(selection, FALSE);
		LOG(LOG_DEBUG, "OUT : down_item() (block)");
		return;
	}

	if (selection_first_iter(selection, GTK_TREE_MODEL(dict_store),
				 &iter) == FALSE)
	{
		popup_warning(_("Please select dictionary."));
		return;
	}

	path = gtk_tree_model_get_path(GTK_TREE_MODEL(dict_store), &iter);
	path_orig = gtk_tree_path_copy(path);

	gtk_tree_path_next(path);
	if((gtk_tree_path_compare(path, path_orig) != 0) &&
	   gtk_tree_model_get_iter(GTK_TREE_MODEL(dict_store),
				   &next_iter, path))
	{
			my_gtk_tree_store_swap(dict_store, &iter, &next_iter);
			selection_select_only(gtk_tree_view_get_selection(GTK_TREE_VIEW(dict_view)),
						       &iter);
	} else if(gtk_tree_path_get_depth(path) > 1){
		gtk_tree_path_up(path);
		if(gtk_tree_path_compare(path, path_orig) != 0){
			gtk_tree_path_free(path_orig);
			path_orig = gtk_tree_path_copy(path);
			gtk_tree_path_next(path);
			if(gtk_tree_path_compare(path, path_orig) != 0){
				GtkTreeIter parent;
				if(gtk_tree_model_get_iter(GTK_TREE_MODEL(dict_store),
							   &parent,
							   path))
				{
					
					GtkTreeIter new_iter;
					gint type;
					gchar *title;
					gchar *book_path;
					gint subbook_no;
					gchar *appendix_path;
					gint appendix_subbook_no;
					gboolean active;
					BOOK_INFO *binfo;
					gboolean editable;
					gboolean available;
					gchar *fg, *bg;

					gtk_tree_store_prepend(GTK_TREE_STORE(dict_store),
							      &new_iter,
							      &parent);


					gtk_tree_model_get(GTK_TREE_MODEL(dict_store), 
							   &iter, 
							   DICT_TYPE_COLUMN, &type,
							   DICT_TITLE_COLUMN, &title,
							   DICT_PATH_COLUMN, &book_path,
							   DICT_SUBBOOK_NO_COLUMN, &subbook_no,
							   DICT_APPENDIX_PATH_COLUMN, &appendix_path,
							   DICT_APPENDIX_SUBBOOK_NO_COLUMN, &appendix_subbook_no,
							   DICT_ACTIVE_COLUMN, &active,
							   DICT_MEMBER_COLUMN, &binfo,
							   DICT_EDITABLE_COLUMN, &editable,
							   DICT_FGCOLOR_COLUMN, &fg,
							   DICT_BGCOLOR_COLUMN, &bg,
							   DICT_AVAILABLE_COLUMN, &available,
							   -1);

					gtk_tree_store_set(dict_store, &new_iter,
							   DICT_TYPE_COLUMN, type,
							   DICT_TITLE_COLUMN, title,
							   DICT_PATH_COLUMN, book_path,
							   DICT_SUBBOOK_NO_COLUMN, subbook_no,
							   DICT_APPENDIX_PATH_COLUMN, appendix_path,
							   DICT_APPENDIX_SUBBOOK_NO_COLUMN, appendix_subbook_no,
							   DICT_ACTIVE_COLUMN, active,
							   DICT_MEMBER_COLUMN, binfo,
							   DICT_EDITABLE_COLUMN, editable,
							   DICT_FGCOLOR_COLUMN, fg,
							   DICT_BGCOLOR_COLUMN, bg,
							   DICT_AVAILABLE_COLUMN, available,
							   -1);
					g_free(title);
					g_free(book_path);
					g_free(appendix_path);
					g_free(fg);
					g_free(bg);

					gtk_tree_store_remove(GTK_TREE_STORE(dict_store),
							      &iter);

					gtk_tree_view_expand_row(GTK_TREE_VIEW(dict_view), path, TRUE);
					selection_select_only(gtk_tree_view_get_selection(GTK_TREE_VIEW(dict_view)),
								       &new_iter);

				}
			}
		}
	}

	gtk_tree_path_free(path);
	gtk_tree_path_free(path_orig);

	LOG(LOG_DEBUG, "OUT : down_item()");
	return;
}

/* Collect the selected rows as paths, deepest/last first.  Working from the
 * end keeps the remaining paths valid while rows are removed or swapped. */
static GList *selected_paths_reversed(GtkTreeSelection *selection)
{
	GList *rows, *node;
	GList *paths = NULL;

	rows = gtk_tree_selection_get_selected_rows(selection, NULL);
	for(node = rows; node != NULL; node = node->next)
		paths = g_list_prepend(paths, gtk_tree_path_copy(node->data));
	g_list_foreach(rows, (GFunc)gtk_tree_path_free, NULL);
	g_list_free(rows);

	return(paths);
}

static void remove_item(GtkWidget *widget, gpointer *data)
{

	GtkTreeSelection *selection;
	GList *paths, *node;
	GtkTreeIter iter;

	LOG(LOG_DEBUG, "IN : remove_item()");

	selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(dict_view));

	paths = selected_paths_reversed(selection);
	if(paths == NULL){
		LOG(LOG_DEBUG, "OUT : remove_item() (nothing selected)");
		return;
	}

	edited = FALSE;

	/* Descending order: removing a later row does not shift earlier paths.
	 * Paths returned by get_selected_rows() are already in ascending order,
	 * and selected_paths_reversed() reversed them. */
	removing_rows = TRUE;
	for(node = paths; node != NULL; node = node->next){
		if(gtk_tree_model_get_iter(GTK_TREE_MODEL(dict_store), &iter,
					   (GtkTreePath *)node->data) == FALSE)
			continue;
		gtk_tree_store_remove(GTK_TREE_STORE(dict_store), &iter);
	}
	removing_rows = FALSE;

	/* The property panel still describes a row that no longer exists. */
	edited = FALSE;
	last_binfo = NULL;

	g_list_foreach(paths, (GFunc)gtk_tree_path_free, NULL);
	g_list_free(paths);

	LOG(LOG_DEBUG, "OUT : remove_item()");
}



static void add_group(GtkWidget *widget,gpointer *data)
{

	GtkTreeIter iter;
	char *name;

	LOG(LOG_DEBUG, "IN : add_grop()");

	name = (gchar *)gtk_entry_get_text(GTK_ENTRY(entry_group_name));
	name = g_strdup(name);
	remove_space(name);
	if(strlen(name) == 0){
	  return;
	}
	gtk_tree_store_append(GTK_TREE_STORE(dict_store), &iter, NULL);
	gtk_tree_store_set(GTK_TREE_STORE(dict_store), &iter,
			   DICT_TYPE_COLUMN, 0,
			   DICT_TITLE_COLUMN, name,
			   DICT_EDITABLE_COLUMN, TRUE,
			   DICT_AVAILABLE_COLUMN, TRUE,
			   -1);

	gtk_entry_set_text(GTK_ENTRY(entry_group_name), "");

	g_free(name);
	
	LOG(LOG_DEBUG, "OUT : add_grop()");
}

/* Number of dictionaries added by the last "Search Disk" run.  Used to tell
 * the user when a scan found nothing instead of silently doing nothing. */
static int books_found = 0;

/* Books that could not be used (bind failed, no search method, ...).  Counted
 * instead of reported one dialog at a time: scanning a network share easily
 * hits dozens of these, and a dialog per file would bury the result. */
static int books_skipped = 0;

static int enumerate_subbook(char *path)
{
	EB_Book book;
	EB_Error_Code error_code;
	int subcount, i;
	EB_Subbook_Code sublist[EB_MAX_SUBBOOKS];
	char buff[512];
	BOOK_INFO *binfo;
	EB_Multi_Search_Code multi_list[EB_MAX_MULTI_SEARCHES];
	int multi_count;

	GtkTreeIter parent_iter;
	GtkTreeIter child_iter;
	gchar *utf_str;
	gboolean group_exist;
	gchar *title;
	gchar *utf_path;

	LOG(LOG_DEBUG, "IN : enumerate_subbook(%s)", path);

	eb_initialize_book(&book);
	error_code = eb_bind(&book, path);
	if(error_code != EB_SUCCESS){
		/* Counted rather than reported per file.  A network share holds
		 * many directories with a CATALOG that libeb still refuses, and
		 * one dialog each would bury the actual result of the scan; the
		 * total is reported once when the scan finishes.  Keeping this at
		 * LOG_DEBUG also means a failed bind no longer silently looks
		 * like "Search Disk did nothing". */
		books_skipped++;
		LOG(LOG_DEBUG, "Failed to bind the book '%s' : %s",
		    path, ebook_error_message(error_code));
		return(1);
	}

	error_code = eb_subbook_list(&book, sublist, &subcount);
	if(error_code != EB_SUCCESS){
		books_skipped++;
		LOG(LOG_DEBUG, "Failed to get a subbook list of '%s' : %s",
		    path, ebook_error_message(error_code));
		goto failed;
	}
	for(i=0 ; i<subcount ; i++){
		error_code = eb_subbook_directory2(
			&book,
			sublist[i],
			buff);
		if (error_code != EB_SUCCESS){
			books_skipped++;
			LOG(LOG_DEBUG, "Failed to get subbook directory of '%s' : %s",
			    path, ebook_error_message(error_code));
			goto failed;
		}

		error_code = eb_subbook_title2(
			&book,
			sublist[i],
			buff);
		if (error_code != EB_SUCCESS){
			books_skipped++;
			LOG(LOG_DEBUG, "Failed to get title of '%s' : %s",
			    path, ebook_error_message(error_code));
			goto failed;
		}

		error_code = eb_set_subbook(&book, sublist[i]); 
		if (error_code != EB_SUCCESS){
			books_skipped++;
			LOG(LOG_DEBUG, "Failed to set subbook %s, %d: %s",
			    path, sublist[i],
			    ebook_error_message(error_code));
			goto failed;
		}

		eb_multi_search_list(&book, multi_list, &multi_count);
		if(eb_have_word_search(&book) ||
		   eb_have_endword_search(&book) ||
		   eb_have_exactword_search(&book) ||
		   eb_have_keyword_search(&book) ||
		   (multi_count != 0))
		{

			utf_str = iconv_convert("euc-jp", "utf-8", buff);
			binfo = load_book(path, sublist[i], NULL, 0, NULL, NULL);
			if(binfo == NULL){
				books_skipped++;
				LOG(LOG_DEBUG, "Failed to load book '%s', subbook %d",
				    path, sublist[i]);
				free(utf_str);	/* iconv_convert() uses malloc */
				goto failed;
			}


			// "Disk Search Result" というグループはあるか?
			group_exist = FALSE;

			if(gtk_tree_model_get_iter_first(GTK_TREE_MODEL(dict_store), &parent_iter) == TRUE){
				do { 
					gtk_tree_model_get(GTK_TREE_MODEL(dict_store), 
							   &parent_iter, 
							   DICT_TITLE_COLUMN, &title,
							   -1);

					if(strcmp(title, "Disk Search Result") == 0){
						group_exist = TRUE;
						g_free(title);
						break;
					}
					g_free(title);
				} while (gtk_tree_model_iter_next(GTK_TREE_MODEL(dict_store), &parent_iter) == TRUE);
		       }

			if(group_exist == FALSE){

				gtk_tree_store_append(GTK_TREE_STORE(dict_store), &parent_iter, NULL);
				gtk_tree_store_set(GTK_TREE_STORE(dict_store), &parent_iter,
						   DICT_TYPE_COLUMN, 0,
						   DICT_TITLE_COLUMN, "Disk Search Result",
						   DICT_EDITABLE_COLUMN, TRUE,
						   DICT_AVAILABLE_COLUMN, TRUE,
						   -1);

			}

			utf_path = fs_to_unicode(path);

			gtk_tree_store_append(GTK_TREE_STORE(dict_store), &child_iter, &parent_iter);

			gtk_tree_store_set (GTK_TREE_STORE(dict_store), &child_iter,
					    DICT_TYPE_COLUMN, 1,
					    DICT_TITLE_COLUMN, utf_str,
					    DICT_PATH_COLUMN, utf_path,
					    DICT_SUBBOOK_NO_COLUMN, sublist[i],
					    DICT_APPENDIX_PATH_COLUMN, NULL,
					    DICT_APPENDIX_SUBBOOK_NO_COLUMN, 0,
					    DICT_ACTIVE_COLUMN, TRUE,
					    DICT_MEMBER_COLUMN, binfo,
					    DICT_EDITABLE_COLUMN, FALSE,
					    DICT_FGCOLOR_COLUMN, NULL,
					    DICT_BGCOLOR_COLUMN, NULL,
					    DICT_AVAILABLE_COLUMN, TRUE,
					    -1);

			/* Expanding the group in the settings tree is a purely visual
			 * concern and is done by the caller (which owns dict_view).
			 * Keeping it out of the scan makes scan_dictionaries() usable
			 * from outside the preferences dialog. */
			free(utf_str);
			books_found++;
		} else {
			books_skipped++;
			LOG(LOG_DEBUG,
			    "%s: the book has no search method (word/endword/"
			    "exactword/keyword/multi), so it cannot be used "
			    "for lookups and was skipped.", path);
		}

	}
	eb_finalize_book(&book);

	LOG(LOG_DEBUG, "OUT : enumerate_subbook()");

	return(0);

 failed:
	eb_finalize_book(&book);
	LOG(LOG_DEBUG, "OUT : enumerate_subbook() (failed)");
	return(1);
}

/* Depth semantics (as seen by the user):
 *   0 = recurse without limit, 1 = only the selected folder,
 *   2 = the folder and its immediate subfolders, and so on.
 * A hard cap keeps "0" from looping forever on symlink cycles or an
 * unexpectedly deep tree. */
#define DEPTH_UNLIMITED 0
#define DEPTH_HARD_LIMIT 64

static gint max_depth = DEPTH_UNLIMITED;

static void search_dictionary_recursive(gchar *dirname, gint depth)
{
	GDir *dir;
	const gchar *name;
	gchar *fullpath;
	gboolean catalog_found = FALSE;
	GStatBuf st;

	LOG(LOG_DEBUG, "IN : search_dictionary_recursive(%s, %d)", dirname, depth);
	
	if((dir = g_dir_open(dirname, 0, NULL)) == NULL){
		LOG(LOG_DEBUG, "Failed to open directory %s", dirname);
		LOG(LOG_DEBUG, "OUT : search_dictionary_recursive()");
		return;
	}
	
	while((name = g_dir_read_name(dir)) != NULL){
		/* g_build_filename() uses the native separator (\ on Windows) and
		 * cannot overflow, unlike the old fixed 512 byte sprintf buffer. */
		fullpath = g_build_filename(dirname, name, NULL);

		/* One g_stat() instead of two g_file_test() calls.  On a Samba
		 * mount each call is a network round trip, so halving them halves
		 * the time the UI spends frozen.  g_stat() (not stat()) is used
		 * because it takes a UTF-8 path on Windows. */
		if(g_stat(fullpath, &st) != 0){
			g_free(fullpath);
			continue;
		}

		if(S_ISREG(st.st_mode)){
			/* Only enumerate once per directory: some discs ship both
			 * a CATALOG (EB) and a CATALOGS (EPWING) file. */
			if(catalog_found == FALSE &&
			   ((g_ascii_strcasecmp(name, "catalog") == 0) ||
			    (g_ascii_strcasecmp(name, "catalogs") == 0))){
				catalog_found = TRUE;
				enumerate_subbook(dirname);
			}
		} else if(S_ISDIR(st.st_mode)){
			if(depth < DEPTH_HARD_LIMIT &&
			   (max_depth == DEPTH_UNLIMITED || depth < max_depth))
				search_dictionary_recursive(fullpath, depth+1);
		}
		g_free(fullpath);
	}
	g_dir_close(dir);

	LOG(LOG_DEBUG, "OUT : search_dictionary_recursive()");
}

/* Walk a network share and every stat() is a round trip, so the scan runs for
 * a long time with the main loop completely blocked.  A wait cursor at least
 * tells the user the window is busy rather than hung. */
static void set_scan_cursor(GtkWidget *window, gboolean busy)
{
	GdkCursor *cursor;

	if(window == NULL || GTK_IS_WIDGET(window) == FALSE ||
	   GTK_WIDGET(window)->window == NULL)
		return;

	cursor = busy ? gdk_cursor_new(GDK_WATCH) : NULL;
	gdk_window_set_cursor(GTK_WIDGET(window)->window, cursor);
	if(cursor != NULL)
		gdk_cursor_unref(cursor);
	gdk_flush();
}

/* Scan a directory tree for EPWING/EB books and append every usable one to
 * dict_store (into the "Disk Search Result" group).  This is deliberately
 * independent of the preferences dialog so it can also be driven from the
 * File menu.  Returns the number of dictionaries added; *skipped receives
 * the count of unusable books.  The caller owns reporting and persistence. */
gint scan_dictionaries(const gchar *dirname, gint depth,
		       GtkWidget *busy_window, gint *skipped)
{
	LOG(LOG_DEBUG, "IN : scan_dictionaries(%s, %d)", dirname, depth);

	max_depth = depth;
	books_found = 0;
	books_skipped = 0;

	set_scan_cursor(busy_window, TRUE);
	/* depth starts at 1: the selected folder itself is level 1. */
	search_dictionary_recursive((gchar *)dirname, 1);
	set_scan_cursor(busy_window, FALSE);

	/* The newly created "Disk Search Result" group has no DICT_ACTIVE_COLUMN
	 * set, so the dictionary bar would keep showing the previously active
	 * group and none of the new dictionaries.  Make the new group the one
	 * and only active group, exactly like group_changed() does when the
	 * user picks a group from the combo. */
	if(books_found > 0)
		activate_disk_search_group();

	if(skipped != NULL)
		*skipped = books_skipped;

	LOG(LOG_DEBUG, "OUT : scan_dictionaries() = %d", books_found);
	return(books_found);
}

/* Make "Disk Search Result" the active group.  add_dict_buttons() only shows
 * the dictionaries of the active group, and the combo box shows the active
 * group's name, so without this a successful scan looks like it did nothing. */
static void activate_disk_search_group(void)
{
	GtkTreeIter iter;
	gchar *title;
	gboolean found = FALSE;

	if(gtk_tree_model_get_iter_first(GTK_TREE_MODEL(dict_store), &iter) == FALSE)
		return;

	/* First clear every group, then activate the one we just filled.  This
	 * also matches the "exactly one active group" invariant that
	 * add_dict_buttons() relies on. */
	do {
		gtk_tree_store_set(GTK_TREE_STORE(dict_store), &iter,
				   DICT_ACTIVE_COLUMN, FALSE, -1);
	} while(gtk_tree_model_iter_next(GTK_TREE_MODEL(dict_store), &iter) == TRUE);

	if(gtk_tree_model_get_iter_first(GTK_TREE_MODEL(dict_store), &iter) == FALSE)
		return;

	do {
		gtk_tree_model_get(GTK_TREE_MODEL(dict_store), &iter,
				   DICT_TITLE_COLUMN, &title, -1);
		if(title != NULL && strcmp(title, "Disk Search Result") == 0){
			g_free(title);
			gtk_tree_store_set(GTK_TREE_STORE(dict_store), &iter,
					   DICT_ACTIVE_COLUMN, TRUE, -1);
			found = TRUE;
			break;
		}
		g_free(title);
	} while(gtk_tree_model_iter_next(GTK_TREE_MODEL(dict_store), &iter) == TRUE);

	LOG(LOG_DEBUG, "activate_disk_search_group(): found=%d", found);
}

/* Expand the "Disk Search Result" group after a scan, if the settings tree
 * is currently alive.  Purely visual; harmless when called from elsewhere. */
static void expand_disk_search_group(void)
{
	GtkTreeIter iter;
	gchar *title;
	GtkTreePath *tree_path;

	if(dict_view == NULL || GTK_IS_WIDGET(dict_view) == FALSE)
		return;
	if(gtk_tree_model_get_iter_first(GTK_TREE_MODEL(dict_store), &iter) == FALSE)
		return;

	do {
		gtk_tree_model_get(GTK_TREE_MODEL(dict_store), &iter,
				   DICT_TITLE_COLUMN, &title, -1);
		if(title != NULL && strcmp(title, "Disk Search Result") == 0){
			g_free(title);
			tree_path = gtk_tree_model_get_path(GTK_TREE_MODEL(dict_store),
							   &iter);
			gtk_tree_view_expand_row(GTK_TREE_VIEW(dict_view),
						 tree_path, TRUE);
			gtk_tree_path_free(tree_path);
			return;
		}
		g_free(title);
	} while(gtk_tree_model_iter_next(GTK_TREE_MODEL(dict_store), &iter) == TRUE);
}

static void search_disk(GtkWidget *widget, gpointer *data)
{
	gchar *dirname;
	gint found;
	gint skipped;

	LOG(LOG_DEBUG, "IN : search_disk()");

	dirname = (gchar *)gtk_entry_get_text(GTK_ENTRY(entry_directory_name));
	dirname = g_strdup(dirname);
	remove_space(dirname);

#ifdef __WIN32__
	if((strlen(dirname) != 1) &&(dirname[strlen(dirname) - 1] == '\\'))
#else
	if((strlen(dirname) != 1) &&(dirname[strlen(dirname) - 1] == '/'))
#endif

		dirname[strlen(dirname) - 1] = '\0';
	if(strlen(dirname) == 0){
		popup_warning(_("Please enter directory name"));
		g_free(dirname);
		return;
	}

	if(g_file_test(dirname, G_FILE_TEST_IS_DIR) != TRUE){
		LOG(LOG_WARNING, "Directory does not exist: %s", dirname);
		popup_warning(_("Directory does not exist. Check the path."));
		g_free(dirname);
		return;
	}

	found = scan_dictionaries(
		dirname,
		gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(spin_search_depth)),
		pref_dlg, &skipped);

	expand_disk_search_group();

	if(found == 0){
		LOG(LOG_WARNING, "No dictionary was found under %s (depth=%d)",
		    dirname, max_depth);
		popup_warning(_("No dictionary was found. Check the path and "
				"the search depth."));
	} else if(skipped > 0){
		/* One dialog for the whole run.  Reporting every rejected book
		 * separately turned a scan of a Samba share into a wall of modal
		 * warnings that had to be dismissed one by one. */
		gchar *msg = g_strdup_printf(
			_("Added %d dictionaries. %d could not be used "
			  "(no search method, or the book could not be read) "
			  "and were skipped."),
			found, skipped);
		LOG(LOG_DEBUG, "%s", msg);
		popup_warning(msg);
		g_free(msg);
	}
	g_free(dirname);

	LOG(LOG_DEBUG, "OUT : search_disk()");
}

static gboolean drag_data_received(GtkTreeDragDest   *drag_dest,
				   GtkTreePath       *dest,
				   GtkSelectionData  *selection_data)
{
	LOG(LOG_DEBUG, "IN : drag_data_received()");
	last_binfo = NULL;
	LOG(LOG_DEBUG, "OUT : drag_data_received()");
	return(FALSE);
	
}

static gboolean update_last_dictionary()
{
	gchar *title;
	gchar *book_path;
	gint subbook_no;
	gchar *appendix_path;
	gint appendix_subbook_no;
	gchar *fg, *bg;
	GtkTreeSelection *select;
	gchar *fs_book_path;
	gchar *fs_appendix_path;
	BOOK_INFO *binfo=NULL;
	gchar *tmp;

	LOG(LOG_DEBUG, "IN : update_last_dictionary()");

	if(edited == FALSE)
		goto END;

	if(popup_active())
		goto END;

	if(last_binfo == NULL) {
		goto END;
	}

	/* last_iter may point at a row that has been removed since it was
	 * stored; using it would dereference a freed tree node. */
	if(gtk_tree_store_iter_is_valid(dict_store, &last_iter) == FALSE)
		goto END;

	title = (gchar *)gtk_entry_get_text(GTK_ENTRY(entry_title));
	if(!title || (strlen(title) == 0)) {
		popup_error(_("Please specify title."));
		goto FAILED;
	}

	book_path = (gchar *)gtk_label_get_text(GTK_LABEL(label_book_path));
	if(!book_path || (strlen(book_path) == 0)) {
		popup_error(_("Please specify book path."));
		goto FAILED;
	}

	appendix_path = (gchar *)gtk_entry_get_text(GTK_ENTRY(entry_appendix_path));

	tmp = (gchar *)gtk_entry_get_text(GTK_ENTRY(entry_subbook_no));
	if(tmp)
		subbook_no = atoi(tmp);
	else {
		popup_error(_("Subbook number incorrect."));
		goto FAILED;
	}

	tmp = (gchar *)gtk_entry_get_text(GTK_ENTRY(entry_appendix_subbook_no));
	if(tmp)
		appendix_subbook_no = atoi(tmp);
	else
		appendix_subbook_no = 0;		


	if(book_path)
		fs_book_path = unicode_to_fs(book_path);
	else
		fs_book_path = NULL;

	if((appendix_path) && (strlen(appendix_path) != 0))
		fs_appendix_path = unicode_to_fs(appendix_path);
	else
		fs_appendix_path = NULL;

	if(strlen(fg_color) == 0)
		fg = NULL;
	else
		fg = fg_color;

	if(strlen(bg_color) == 0)
		bg = NULL;
	else
		bg = bg_color;

	binfo = load_book(fs_book_path, subbook_no,
			  fs_appendix_path, appendix_subbook_no, fg, bg);

	if(binfo == NULL)
		goto FAILED;
	gtk_tree_store_set(dict_store, &last_iter,
			   DICT_TYPE_COLUMN, 1,
			   DICT_TITLE_COLUMN, title,
			   DICT_PATH_COLUMN, book_path,
			   DICT_SUBBOOK_NO_COLUMN, subbook_no,
			   DICT_APPENDIX_PATH_COLUMN, appendix_path,
			   DICT_APPENDIX_SUBBOOK_NO_COLUMN, appendix_subbook_no,
			   DICT_ACTIVE_COLUMN, last_active,
			   DICT_MEMBER_COLUMN, binfo,
			   DICT_AVAILABLE_COLUMN, TRUE,
			   DICT_EDITABLE_COLUMN, FALSE,
			   DICT_FGCOLOR_COLUMN, fg,
			   DICT_BGCOLOR_COLUMN, bg,
			   -1);

	last_binfo = binfo;

	edited = FALSE;

 END:
	LOG(LOG_DEBUG, "OUT : update_last_dictionary() = TRUE");
	return(TRUE);

 FAILED:
	/* Put the selection back on the row whose edit was rejected, with the
	 * changed-signal suppressed by rewinding.  set_reorderable() is not
	 * used any more: the view has a custom drag source, and enabling it
	 * here would add GTK's own single-row drag on top of ours. */
	rewinding = TRUE;
	select = gtk_tree_view_get_selection(GTK_TREE_VIEW(dict_view));
	gtk_tree_selection_unselect_all(select);
	gtk_tree_selection_select_iter(select, &last_iter);

	LOG(LOG_DEBUG, "OUT : update_last_dictionary() = FALSE");
	return(FALSE);

}

static void dict_selection_changed(GtkTreeSelection *selection, gpointer data)
{
	GtkTreeIter iter;

	/* Initialised because the rewinding path jumps to END before they are
	 * filled in, and END frees them. */
	gchar *title = NULL;
	gchar *book_path = NULL;
	gint  subbook_no = 0;
	gchar *appendix_path=NULL;
	gint  appendix_subbook_no=0;
	gboolean active = FALSE;
	BOOK_INFO *binfo = NULL;
	gchar *fg = NULL;
	gchar *bg = NULL;
	gint  type = 0;
	gchar buff[128];

	LOG(LOG_DEBUG, "IN : dict_selection_changed()");

	/* Rows are being deleted: the selection is in flux and the rows it
	 * names may already be gone. */
	if(removing_rows == TRUE)
		return;

	if(rewinding == TRUE){
		rewinding = FALSE;
		goto END;
	} else {
		if(update_last_dictionary() == FALSE)
			goto END;
	}

	if (selection_first_iter(selection, GTK_TREE_MODEL(dict_store),
				 &iter) == FALSE)
	{
		//popup_warning(_("Please select group."));
		LOG(LOG_DEBUG, "OUT : dict_selection_changed()");
		return;
	}

	gtk_tree_model_get(GTK_TREE_MODEL(dict_store), &iter,
			   DICT_TYPE_COLUMN, &type,
			   DICT_TITLE_COLUMN, &title,
			   DICT_PATH_COLUMN, &book_path,
			   DICT_SUBBOOK_NO_COLUMN, &subbook_no,
			   DICT_APPENDIX_PATH_COLUMN, &appendix_path,
			   DICT_APPENDIX_SUBBOOK_NO_COLUMN, &appendix_subbook_no,
			   DICT_ACTIVE_COLUMN, &active,
			   DICT_MEMBER_COLUMN, &binfo,
			   DICT_FGCOLOR_COLUMN, &fg,
			   DICT_BGCOLOR_COLUMN, &bg,
			   -1);

	// Group
	if(type == 0)
		goto END;

	if(title != NULL)
		gtk_entry_set_text(GTK_ENTRY(entry_title), title);
	else
		gtk_entry_set_text(GTK_ENTRY(entry_title), "");

	if(book_path != NULL)
		gtk_label_set_text(GTK_LABEL(label_book_path), book_path);
	else
		gtk_label_set_text(GTK_LABEL(label_book_path), "");

	if(appendix_path != NULL)
		gtk_entry_set_text(GTK_ENTRY(entry_appendix_path), appendix_path);
	else
		gtk_entry_set_text(GTK_ENTRY(entry_appendix_path), "");


	sprintf(buff, "%d", subbook_no);
	gtk_entry_set_text(GTK_ENTRY(entry_subbook_no), buff);

	sprintf(buff, "%d", appendix_subbook_no);
	gtk_entry_set_text(GTK_ENTRY(entry_appendix_subbook_no), buff);

	if(fg)
		sprintf(fg_color, "%s", fg);
	else
		fg_color[0] = '\0';

	if(bg)
		sprintf(bg_color, "%s", bg);
	else
		bg_color[0] = '\0';


	if(fg == NULL) {
		if(bg == NULL)
			sprintf(buff, "%s", _("Sample"));
		else
			sprintf(buff, "<span background=\"%s\">%s</span>",  bg_color, _("Sample"));
	} else {
		if(bg == NULL)
			sprintf(buff, "<span foreground=\"%s\">%s</span>", fg_color, _("Sample"));
		else
			sprintf(buff, "<span foreground=\"%s\" background=\"%s\">%s</span>", fg_color, bg_color, _("Sample"));
	}

	gtk_label_set_text(GTK_LABEL(label_color), buff);
	gtk_label_set_use_markup (GTK_LABEL (label_color), TRUE);

	if(selection_first_iter(selection, GTK_TREE_MODEL(dict_store),
				&last_iter) == FALSE)
		goto END;
	last_binfo = binfo;
	last_active = active;
	edited = TRUE;



 END:
	g_free(title);
	g_free(book_path);
	g_free(appendix_path);
	g_free(fg);
	g_free(bg);

	LOG(LOG_DEBUG, "OUT : dict_selection_changed()");
}

static void ok_colorsel(GtkWidget *widget,gpointer *data){

	GdkColor color;
	gchar *color_name;
	gchar buff[128];
	gchar *title;
	
	LOG(LOG_DEBUG, "IN : ok_colorsel()");

	gtk_grab_remove(colorsel_dlg);

	gtk_color_selection_get_current_color(GTK_COLOR_SELECTION(GTK_COLOR_SELECTION_DIALOG(colorsel_dlg)->colorsel), &color);
	color_name = gtk_color_selection_palette_to_string(&color, 1);

	if(color_no == 0){
		sprintf(fg_color, "%s", color_name);
	} else {
		sprintf(bg_color, "%s", color_name);
	}

	title = (gchar *)gtk_entry_get_text(GTK_ENTRY(entry_title));
	if(strlen(fg_color) == 0){
		if(strlen(bg_color) == 0)
			sprintf(buff, _("Sample"));
		else
			sprintf(buff, "<span background=\"%s\">%s</span>", bg_color, _("Sample"));
	} else {
		if(strlen(bg_color) == 0)
			sprintf(buff, "<span foreground=\"%s\">%s</span>", fg_color, _("Sample"));
			else
				sprintf(buff, "<span foreground=\"%s\" background=\"%s\">%s</span>", fg_color, bg_color, _("Sample"));
	}
	gtk_label_set_text(GTK_LABEL(label_color), buff);
	gtk_label_set_use_markup (GTK_LABEL (label_color), TRUE);

	gtk_widget_destroy(colorsel_dlg);

	LOG(LOG_DEBUG, "OUT : ok_colorsel()");
}

static void delete_colorsel( GtkWidget *widget,
		   GdkEvent  *event,
		   gpointer   data )
{
	LOG(LOG_DEBUG, "IN : delete_colorsel()");

	ok_colorsel(NULL, NULL);

	LOG(LOG_DEBUG, "OUT : delete_colorsel()");
}

static void show_colorsel(GtkWidget *widget,gpointer *data){

	GdkColor color;
	
	LOG(LOG_DEBUG, "IN : show_colorsel()");
	
	color_no = GPOINTER_TO_INT(data);


	colorsel_dlg = gtk_color_selection_dialog_new(_("Choose Color"));

	g_signal_connect (G_OBJECT(colorsel_dlg), "delete_event",
			  G_CALLBACK(delete_colorsel), NULL);

	g_signal_connect(G_OBJECT(GTK_COLOR_SELECTION_DIALOG(colorsel_dlg)->ok_button), "clicked",
			 G_CALLBACK(ok_colorsel), NULL);

	g_signal_connect_swapped(G_OBJECT(GTK_COLOR_SELECTION_DIALOG(colorsel_dlg)->cancel_button), "clicked",
				G_CALLBACK(gtk_widget_destroy), (gpointer)colorsel_dlg);


	g_assert(color_no < NUM_COLORS);

	if(color_no == 0){
		gdk_color_parse(fg_color, &color);
	} else {
		gdk_color_parse(bg_color, &color);
	}

	gtk_color_selection_set_current_color(GTK_COLOR_SELECTION(GTK_COLOR_SELECTION_DIALOG(colorsel_dlg)->colorsel), &color);

	gtk_widget_realize(colorsel_dlg);
	center_dialog(pref_dlg, colorsel_dlg);
	gtk_widget_show_all(colorsel_dlg);

	gtk_grab_add(colorsel_dlg);

	LOG(LOG_DEBUG, "OUT : show_colorsel()");
}


static void clear_color(GtkWidget *widget,gpointer *data){

	gchar buff[128];
	
	LOG(LOG_DEBUG, "IN : clear_color()");

	fg_color[0] = '\0';
	bg_color[0] = '\0';
	sprintf(buff, _("Sample"));
	gtk_label_set_text(GTK_LABEL(label_color), buff);
	gtk_label_set_use_markup (GTK_LABEL (label_color), FALSE);

	LOG(LOG_DEBUG, "OUT : clear_color()");
}

GtkWidget *pref_start_dictgroup()
{
	GtkWidget *button;
	GtkWidget *vbox_l;
	GtkWidget *vbox_r;
	GtkWidget *hbox;
	GtkWidget *hbox2;
	GtkWidget *frame;
	GtkWidget *label;
	GtkWidget *scroll;
	GtkObject *adj;

	GtkCellRenderer *renderer;
	GtkTreeViewColumn *column;
	GtkTreeSelection *select;


	GtkTreeDragDestIface *iface;


	LOG(LOG_DEBUG, "IN : pref_start_dictgroup()");


	hbox = gtk_hbox_new(FALSE,0);
	gtk_container_set_border_width(GTK_CONTAINER(hbox), 2);

	vbox_l = gtk_vbox_new(FALSE,0);
	gtk_container_set_border_width(GTK_CONTAINER(vbox_l), 2);
	gtk_box_pack_start (GTK_BOX(hbox)
			    , vbox_l,TRUE, TRUE, 0);

	frame = gtk_frame_new(NULL);
	gtk_frame_set_shadow_type(GTK_FRAME(frame), GTK_SHADOW_IN);
	gtk_box_pack_start (GTK_BOX(vbox_l)
			    , frame,TRUE, TRUE, 0);
	
	scroll = gtk_scrolled_window_new (NULL, NULL);
	gtk_container_add (GTK_CONTAINER (frame), scroll);
	gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll),
					GTK_POLICY_AUTOMATIC,
					GTK_POLICY_AUTOMATIC);

	dict_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(dict_store));
	gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(dict_view), TRUE);
	gtk_tree_view_expand_all(GTK_TREE_VIEW(dict_view));
	gtk_container_add (GTK_CONTAINER (scroll), dict_view);

	select = gtk_tree_view_get_selection(GTK_TREE_VIEW(dict_view));
	/* Ctrl/Shift click selects several dictionaries, so Remove/Up/Down and
	 * dragging can act on a whole block at once. */
	gtk_tree_selection_set_mode(select, GTK_SELECTION_MULTIPLE);
	g_signal_connect(G_OBJECT(select), "changed",
			 G_CALLBACK (dict_selection_changed),
			 NULL);

	g_signal_connect(G_OBJECT(dict_view), "key_press_event",
			 G_CALLBACK(dict_view_key_press), NULL);


/*
	gtk_tree_view_enable_model_drag_source(
		GTK_TREE_VIEW(dict_view),
		GDK_BUTTON1_MASK,  row_targets, G_N_ELEMENTS(row_targets), 
		GDK_ACTION_MOVE);

	gtk_tree_view_enable_model_drag_dest(
		GTK_TREE_VIEW(dict_view),
		row_targets, G_N_ELEMENTS(row_targets), GDK_ACTION_MOVE);
*/

	iface = GTK_TREE_DRAG_DEST_GET_IFACE (dict_store);
	iface->row_drop_possible = row_drop_possible;

	g_signal_connect(G_OBJECT(dict_view), "drag_data_received",
			 G_CALLBACK(drag_data_received), NULL);

	/* Custom drag source/dest instead of set_reorderable(), which can only
	 * drag the single row under the cursor.  See dict_drag_data_get() and
	 * dict_drag_data_received(). */
	{
		GtkTargetEntry targets[] = {
			{ (gchar *)DICT_DRAG_TARGET, GTK_TARGET_SAME_WIDGET, 0 }
		};
		gtk_tree_view_enable_model_drag_source(
			GTK_TREE_VIEW(dict_view), GDK_BUTTON1_MASK,
			targets, G_N_ELEMENTS(targets), GDK_ACTION_MOVE);
		gtk_tree_view_enable_model_drag_dest(
			GTK_TREE_VIEW(dict_view), targets, G_N_ELEMENTS(targets),
			GDK_ACTION_MOVE);
	}
	/* Keeps a multi-row drag intact: GtkTreeView collapses the selection
	 * to the row under the cursor when the drag starts. */
	g_signal_connect(G_OBJECT(dict_view), "button-press-event",
			 G_CALLBACK(dict_view_button_press), NULL);
	g_signal_connect(G_OBJECT(dict_view), "drag-begin",
			 G_CALLBACK(dict_view_drag_begin), NULL);
	g_signal_connect(G_OBJECT(dict_view), "drag-end",
			 G_CALLBACK(dict_view_drag_end), NULL);
	g_signal_connect(G_OBJECT(dict_view), "drag-data-get",
			 G_CALLBACK(dict_drag_data_get), NULL);
	g_signal_connect(G_OBJECT(dict_view), "drag-data-received",
			 G_CALLBACK(dict_drag_data_received), NULL);
	g_signal_connect(G_OBJECT(dict_view), "drag-motion",
			 G_CALLBACK(dict_drag_motion), NULL);


	renderer = gtk_cell_renderer_text_new();
	g_signal_connect(G_OBJECT(renderer), "edited",
			 G_CALLBACK(cell_edited), NULL);
	g_object_set_data(G_OBJECT(renderer), "column", (gint *)DICT_TITLE_COLUMN);

	/* gtk_tree_view_insert_column_with_attributes() returns the column
	 * *index* in GTK2, so fetch the column object afterwards.  "Name" is
	 * the first column of this view. */
	gtk_tree_view_insert_column_with_attributes (GTK_TREE_VIEW(dict_view),
						     -1, _("Name"), renderer,
						     "text", DICT_TITLE_COLUMN,
						     "editable", DICT_EDITABLE_COLUMN,
						     NULL);

	column = gtk_tree_view_get_column(GTK_TREE_VIEW(dict_view), 0);

	/* Grey out entries that are in the configuration but could not be
	 * opened, so that a vanished dictionary is visibly "unavailable"
	 * instead of silently disappearing from the list.  Binding the
	 * "sensitive" property (rather than installing a cell data function)
	 * leaves the "text"/"editable" bindings above untouched. */
	if(column != NULL)
		gtk_tree_view_column_add_attribute(column, renderer,
						   "sensitive",
						   DICT_AVAILABLE_COLUMN);

/*
	renderer = gtk_cell_renderer_text_new();
	g_signal_connect(G_OBJECT(renderer), "edited",
			 G_CALLBACK(cell_edited), NULL);
	g_object_set_data(G_OBJECT(renderer), "column", (gint *)DICT_PATH_COLUMN);

	gtk_tree_view_insert_column_with_attributes (GTK_TREE_VIEW(dict_view),
						     -1, _("Path"), renderer,
						     "text", DICT_PATH_COLUMN,
						     "visible", DICT_TYPE_COLUMN,
						     NULL);

	renderer = gtk_cell_renderer_text_new();
	g_signal_connect(G_OBJECT(renderer), "edited",
			 G_CALLBACK(cell_edited), NULL);
	g_object_set_data(G_OBJECT(renderer), "column", (gint *)DICT_SUBBOOK_NO_COLUMN);

	gtk_tree_view_insert_column_with_attributes (GTK_TREE_VIEW(dict_view),
						     -1, _("Subbook Number"), renderer,
						     "text", DICT_SUBBOOK_NO_COLUMN,
						     "visible", DICT_TYPE_COLUMN,
						     NULL);

	renderer = gtk_cell_renderer_text_new();
	g_signal_connect(G_OBJECT(renderer), "edited",
			 G_CALLBACK(cell_edited), NULL);
	g_object_set_data(G_OBJECT(renderer), "column", (gint *)DICT_APPENDIX_PATH_COLUMN);

	gtk_tree_view_insert_column_with_attributes (GTK_TREE_VIEW(dict_view),
						     -1, _("Appendix Path"), renderer,
						     "text", DICT_APPENDIX_PATH_COLUMN,
						     "editable", DICT_EDITABLE_COLUMN,
						     "visible", DICT_TYPE_COLUMN,
						     NULL);

	renderer = gtk_cell_renderer_text_new();
	g_signal_connect(G_OBJECT(renderer), "edited",
			 G_CALLBACK(cell_edited), NULL);
	g_object_set_data(G_OBJECT(renderer), "column", (gint *)DICT_APPENDIX_SUBBOOK_NO_COLUMN);

	gtk_tree_view_insert_column_with_attributes (GTK_TREE_VIEW(dict_view),
						     -1, _("Appendix Subbook Number"), renderer,
						     "text", DICT_APPENDIX_SUBBOOK_NO_COLUMN,
						     "editable", DICT_EDITABLE_COLUMN,
						     "visible", DICT_TYPE_COLUMN,
						     NULL);

//	renderer = gtk_cell_renderer_color_new();
	renderer = gtk_cell_renderer_text_new();
	g_signal_connect(G_OBJECT(renderer), "edited",
			 G_CALLBACK(cell_edited), NULL);
	g_object_set_data(G_OBJECT(renderer), "column", (gint *)DICT_BGCOLOR_COLUMN);

	gtk_tree_view_insert_column_with_attributes (GTK_TREE_VIEW(dict_view),
						     -1, _("BG Color"), renderer,
						     "text", DICT_BGCOLOR_COLUMN,
						     "editable", DICT_EDITABLE_COLUMN,
						     "cell-background", DICT_BGCOLOR_COLUMN,
						     "visible", DICT_TYPE_COLUMN,
						     NULL);



	renderer = gtk_cell_renderer_text_new();
	g_signal_connect(G_OBJECT(renderer), "edited",
			 G_CALLBACK(cell_edited), NULL);
	g_object_set_data(G_OBJECT(renderer), "column", (gint *)DICT_FGCOLOR_COLUMN);

	gtk_tree_view_insert_column_with_attributes (GTK_TREE_VIEW(dict_view),
						     -1, _("FG Color"), renderer,
						     "text", DICT_FGCOLOR_COLUMN,
						     "editable", DICT_EDITABLE_COLUMN,
						     "cell-background", DICT_FGCOLOR_COLUMN,
						     "visible", DICT_TYPE_COLUMN,
						     NULL);



	// すべてのカラムをリサイズできるようにする
	{
		gint i;
		GtkTreeViewColumn *column;

		for(i=0;;i++){
			column = gtk_tree_view_get_column(GTK_TREE_VIEW(dict_view), i);
			if(column == NULL)
				break;
			gtk_tree_view_column_set_resizable(column, TRUE);
		}
	}

*/

	hbox2 = gtk_hbox_new(FALSE, 0);
	gtk_box_pack_start(GTK_BOX(vbox_l),
			   hbox2,FALSE, FALSE, 2);

	label = gtk_label_new(_("Group name"));
	gtk_box_pack_start(GTK_BOX(hbox2),
			   label,FALSE, FALSE, 2);

	entry_group_name = gtk_entry_new();
	gtk_box_pack_start(GTK_BOX(hbox2),
			   entry_group_name,TRUE, TRUE, 2);
	g_signal_connect(G_OBJECT (entry_group_name), "activate",
			 G_CALLBACK(add_group), (gpointer)NULL);

	button = gtk_button_new_with_label(_("Add"));
	gtk_box_pack_start(GTK_BOX(hbox2),
			   button,FALSE,FALSE, 2);
	g_signal_connect(G_OBJECT (button), "clicked",
			 G_CALLBACK(add_group), (gpointer)button);


	button = gtk_button_new_with_label(_("Remove"));
	gtk_box_pack_start(GTK_BOX(hbox2),
			   button,FALSE,FALSE, 2);
	g_signal_connect(G_OBJECT (button), "clicked",
			 G_CALLBACK(remove_item), (gpointer)button);


	button = gtk_button_new_with_label(_("Up"));
	gtk_box_pack_start (GTK_BOX(hbox2),
			    button,FALSE,FALSE, 2);
	g_signal_connect(G_OBJECT (button), "clicked",
			 G_CALLBACK(up_item), (gpointer)button);

	button = gtk_button_new_with_label(_("Down"));
	gtk_box_pack_start(GTK_BOX(hbox2),
			   button,FALSE,FALSE, 2);
	g_signal_connect(G_OBJECT (button), "clicked",
			 G_CALLBACK(down_item), (gpointer)button);


	hbox2 = gtk_hbox_new(FALSE,5);
	gtk_box_pack_start(GTK_BOX(vbox_l),
			   hbox2,FALSE, FALSE, 2);

	label = gtk_label_new(_("Path"));
	gtk_box_pack_start(GTK_BOX(hbox2),
			   label,FALSE, FALSE, 2);

	entry_directory_name = gtk_entry_new();
	gtk_entry_set_width_chars(GTK_ENTRY(entry_directory_name), 28);
	gtk_box_pack_start(GTK_BOX(hbox2),
			   entry_directory_name, TRUE, TRUE, 2);
	g_signal_connect(G_OBJECT (entry_directory_name), "activate",
			 G_CALLBACK(search_disk), (gpointer)NULL);

	label = gtk_label_new(_("Depth"));
	gtk_box_pack_start(GTK_BOX(hbox2),
			   label,FALSE, FALSE, 2);

	adj = gtk_adjustment_new( DEPTH_UNLIMITED, // value: recurse fully
				  0, // lower
				  20, //upper
                                  1, // step increment
                                  10,// page_increment,
                                  (gfloat)0.0);
	spin_search_depth = gtk_spin_button_new(GTK_ADJUSTMENT(adj), 1.0, 0);
	gtk_box_pack_start(GTK_BOX(hbox2),
			   spin_search_depth,FALSE, FALSE, 2);
	gtk_widget_set_tooltip_text(spin_search_depth,
			     _("Specify search depth. 0 = recurse into all subfolders, "
			       "1 = only the specified folder, 2 = include its subfolders."));



	button = gtk_button_new_with_label(_("Search Disk"));
	gtk_box_pack_start(GTK_BOX(hbox2),
			   button,FALSE,FALSE, 2);
	g_signal_connect(G_OBJECT (button), "clicked",
			 G_CALLBACK(search_disk), (gpointer)button);


	// 右半分

	frame = gtk_frame_new(NULL);
	/* Keep the right-hand details pane wide enough for dictionary paths;
	 * the tree on the left keeps the remaining (expandable) space. */
	gtk_widget_set_size_request(frame, 320, -1);
	gtk_box_pack_start (GTK_BOX(hbox)
			    , frame,TRUE, TRUE, 0);

	vbox_r = gtk_vbox_new(FALSE,5);
	gtk_container_set_border_width(GTK_CONTAINER(vbox_r), 5);
	gtk_container_add(GTK_CONTAINER(frame), vbox_r);


	label = gtk_label_new(_("Name"));
	gtk_box_pack_start (GTK_BOX(vbox_r), label,
			    FALSE, FALSE, 0);

	entry_title = gtk_entry_new();
	gtk_box_pack_start (GTK_BOX(vbox_r), entry_title,
			    FALSE, FALSE, 0);

	label = gtk_label_new(_("Path"));
	gtk_box_pack_start (GTK_BOX(vbox_r), label,
			    FALSE, FALSE, 0);

	/* Read-only display. A bare wrapping GtkLabel under-negotiates its
	 * height in GTK2 boxes and clipped the middle lines of long paths;
	 * give it an explicit minimum box so all wrapped lines are drawn. */
	label_book_path = gtk_label_new(NULL);
	gtk_label_set_line_wrap(GTK_LABEL(label_book_path), TRUE);
	/* Paths contain no spaces, so WORD wrap alone would not break them. */
	gtk_label_set_line_wrap_mode(GTK_LABEL(label_book_path),
				    PANGO_WRAP_WORD_CHAR);
	gtk_label_set_selectable(GTK_LABEL(label_book_path), TRUE);
	gtk_misc_set_alignment(GTK_MISC(label_book_path), 0.0, 0.0);
	gtk_widget_set_size_request(label_book_path, 300, 80);
	gtk_box_pack_start (GTK_BOX(vbox_r), label_book_path,
			    FALSE, FALSE, 0);

	label = gtk_label_new(_("Subbook Number"));
	gtk_box_pack_start (GTK_BOX(vbox_r), label,
			    FALSE, FALSE, 0);

	entry_subbook_no = gtk_entry_new();
	gtk_editable_set_editable(GTK_EDITABLE(entry_subbook_no), FALSE);
	gtk_box_pack_start (GTK_BOX(vbox_r), entry_subbook_no,
			    FALSE, FALSE, 0);
	gtk_widget_set_sensitive(entry_subbook_no, FALSE);

	label = gtk_label_new(_("Appendix Path"));
	gtk_box_pack_start (GTK_BOX(vbox_r), label,
			    FALSE, FALSE, 0);

	entry_appendix_path = gtk_entry_new();
	gtk_entry_set_width_chars(GTK_ENTRY(entry_appendix_path), 28);
	gtk_box_pack_start (GTK_BOX(vbox_r), entry_appendix_path,
			    FALSE, FALSE, 0);

	label = gtk_label_new(_("Appendix Subbook Number"));
	gtk_box_pack_start (GTK_BOX(vbox_r), label,
			    FALSE, FALSE, 0);

	entry_appendix_subbook_no = gtk_entry_new();
	gtk_box_pack_start (GTK_BOX(vbox_r), entry_appendix_subbook_no,
			    FALSE, FALSE, 0);

	
	hbox2 = gtk_hbox_new(FALSE,2);
	gtk_container_set_border_width(GTK_CONTAINER(hbox2), 2);
	gtk_box_pack_start (GTK_BOX(vbox_r), hbox2,
			    FALSE, FALSE, 0);

	label_color = gtk_label_new(_("Sample"));
	gtk_label_set_use_markup (GTK_LABEL (label_color), TRUE);
	gtk_box_pack_start (GTK_BOX(hbox2), label_color,
			    TRUE, TRUE, 0);

	button = gtk_button_new_with_label(_("FG"));
	gtk_box_pack_start (GTK_BOX(hbox2), button,
			    FALSE, FALSE, 0);
	g_signal_connect(G_OBJECT (button), "clicked",
			 G_CALLBACK(show_colorsel), (gpointer)0);

	button = gtk_button_new_with_label(_("BG"));
	gtk_box_pack_start (GTK_BOX(hbox2), button,
			    FALSE, FALSE, 0);
	g_signal_connect(G_OBJECT (button), "clicked",
			 G_CALLBACK(show_colorsel), (gpointer)1);

	button = gtk_button_new_with_label(_("Clear"));
	gtk_box_pack_start (GTK_BOX(hbox2), button,
			    FALSE, FALSE, 0);
	g_signal_connect(G_OBJECT (button), "clicked",
			 G_CALLBACK(clear_color), (gpointer)1);

	fg_color[0] = '\0';
	bg_color[0] = '\0';

	/* Remember the list as it is now; Cancel restores it from here. */
	dict_snapshot_save();

	LOG(LOG_DEBUG, "OUT : pref_start_dictgroup()");

	return(hbox);

}

#define G_NODE(node) ((GNode *)node)
#define VALID_ITER(iter, tree_store) (iter!= NULL && iter->user_data != NULL && tree_store->stamp == iter->stamp)


/**
 * gtk_tree_store_swap:
 * copied from GTK+-2.2
 **/
void
my_gtk_tree_store_swap (GtkTreeStore *tree_store,
		     GtkTreeIter  *a,
		     GtkTreeIter  *b)
{
	GNode *tmp, *node_a, *node_b, *parent_node;
	GNode *a_prev, *a_next, *b_prev, *b_next;
	gint i, a_count, b_count, length, *order;
	GtkTreePath *path_a, *path_b;
	GtkTreeIter parent;

	g_return_if_fail (GTK_IS_TREE_STORE (tree_store));
	g_return_if_fail (VALID_ITER (a, tree_store));
	g_return_if_fail (VALID_ITER (b, tree_store));

	node_a = G_NODE (a->user_data);
	node_b = G_NODE (b->user_data);

	/* basic sanity checking */
	if (node_a == node_b)
		return;

	path_a = gtk_tree_model_get_path (GTK_TREE_MODEL (tree_store), a);
	path_b = gtk_tree_model_get_path (GTK_TREE_MODEL (tree_store), b);

	g_return_if_fail (path_a && path_b);

	gtk_tree_path_up (path_a);
	gtk_tree_path_up (path_b);

	if((gtk_tree_path_get_depth(path_a) != 0) ||
	   (gtk_tree_path_get_depth(path_b) != 0)){
		if (gtk_tree_path_compare (path_a, path_b))
		{
			gtk_tree_path_free (path_a);
			gtk_tree_path_free (path_b);

			g_warning ("Given childs are not in the same level\n");
			return;
		}
	}

	if(gtk_tree_path_get_depth(path_a) == 0){
		parent_node = G_NODE (tree_store->root);
	} else {
		gtk_tree_model_get_iter (GTK_TREE_MODEL (tree_store), &parent, path_a);
		parent_node = G_NODE (parent.user_data);
	}

	gtk_tree_path_free (path_b);

  /* old links which we have to keep around */
	a_prev = node_a->prev;
	a_next = node_a->next;

	b_prev = node_b->prev;
	b_next = node_b->next;

  /* fix up links if the nodes are next to eachother */
	if (a_prev == node_b)
		a_prev = node_a;
	if (a_next == node_b)
		a_next = node_a;

	if (b_prev == node_a)
		b_prev = node_b;
	if (b_next == node_a)
		b_next = node_b;

  /* counting nodes */
	tmp = parent_node->children;
	i = a_count = b_count = 0;
	while (tmp)
	{
		if (tmp == node_a)
			a_count = i;
		if (tmp == node_b)
			b_count = i;

		tmp = tmp->next;
		i++;
	}
	length = i;

  /* hacking the tree */
	if (!a_prev)
		parent_node->children = node_b;
	else
		a_prev->next = node_b;

	if (a_next)
		a_next->prev = node_b;

	if (!b_prev)
		parent_node->children = node_a;
	else
		b_prev->next = node_a;

	if (b_next)
		b_next->prev = node_a;

	node_a->prev = b_prev;
	node_a->next = b_next;

	node_b->prev = a_prev;
	node_b->next = a_next;

  /* emit signal */
	order = g_new (gint, length);
	for (i = 0; i < length; i++)
		if (i == a_count)
			order[i] = b_count;
		else if (i == b_count)
			order[i] = a_count;
		else
			order[i] = i;

	if(gtk_tree_path_get_depth(path_a) == 0){
		gtk_tree_model_rows_reordered (GTK_TREE_MODEL (tree_store),
					       path_a,
					       NULL, order);
	} else {
		gtk_tree_model_rows_reordered (GTK_TREE_MODEL (tree_store),
					       path_a,
					       &parent, order);
	}
	gtk_tree_path_free (path_a);
	g_free (order);
}


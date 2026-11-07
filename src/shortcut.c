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
#include "shortcut.h"
#include "mainmenu.h"

#include <gdk/gdkkeysyms.h>

extern struct _shortcut_command commands[];

gint menuitem_handler(GtkWidget *widget, gchar *string);

GtkAccelGroup *accel_group=NULL;

static GList *accel_item_list=NULL;

gboolean accel_handler(GtkAccelGroup *accelgroup,
		       GObject *arg1,
		       guint arg2,
		       GdkModifierType arg3,
		       gpointer user_data)
{

	GdkEventKey event;
	gboolean ret;

	LOG(LOG_DEBUG, "IN : accel_handler()");

	event.keyval = GPOINTER_TO_UINT(user_data);
	event.state = arg3;

	ret = perform_shortcut_by_event(&event);

	LOG(LOG_DEBUG, "OUT : accel_handler()");

	return(ret);
}

void install_shortcut(){
	GtkTreeIter  iter;
	guint state;
	guint keyval;
	GtkWidget *item;
	gint idx;
	gchar *str;
	struct _shortcut_command *command;
	GClosure *closure;

	LOG(LOG_DEBUG, "IN : install_shortcut()");

	idx = 0;

	if(accel_group != NULL) {
		gtk_window_remove_accel_group(GTK_WINDOW(main_window),
					      accel_group);
		g_object_unref(accel_group);
	}
		
	accel_group = gtk_accel_group_new();
	gtk_window_add_accel_group (GTK_WINDOW (main_window), accel_group);


	g_signal_connect(G_OBJECT(accel_group), "accel-activate",
			 G_CALLBACK(accel_handler),
			 (gpointer)NULL);


	if(gtk_tree_model_get_iter_first(GTK_TREE_MODEL(shortcut_store), &iter) == TRUE){
		do { 
			gtk_tree_model_get(GTK_TREE_MODEL(shortcut_store), 
					   &iter,
					   SHORTCUT_STATE_COLUMN, &state,
					   SHORTCUT_KEYVAL_COLUMN, &keyval,
					   SHORTCUT_COMMAND_COLUMN, &command,
					   -1);

//			g_snprintf(string, 64, "sc_%s", command->name);
			//str = g_strdup_printf("sc_%s", command->name);
			/* A bare Return must never become a global accelerator: GtkEntry
			 * consumes it via its "activate" signal, and registering it here
			 * would steal Enter from the word entry (and from dialogs).
			 * "Start Search" is bound to bare Return by default; that entry
			 * is honoured in entry_activate_event() instead. */
			if((state == 0) && (keyval == GDK_Return))
				continue;

			/* "Search Selected Text" is context-sensitive: the toplevel
			 * key_press_event only fires it while the result view has the
			 * focus and holds a selection.  Registering it globally here
			 * would double-fire it (or search from the word entry). */
			if(command != NULL && command->name != NULL &&
			   strcmp(command->name, "Search Selected Text") == 0)
				continue;

			closure = g_cclosure_new(G_CALLBACK(accel_handler), GINT_TO_POINTER(keyval), NULL);


			gtk_accel_group_connect(accel_group, keyval, state, GTK_ACCEL_VISIBLE, closure);
/*
			gtk_widget_add_accelerator (main_window, "activate", accel_group,
						    keyval, state, GTK_ACCEL_VISIBLE);
			accel_item_list = g_list_append(accel_item_list, item);
*/
//			gtk_widget_show(item);

		} while (gtk_tree_model_iter_next(GTK_TREE_MODEL(shortcut_store), &iter) == TRUE);
	}

	update_menu_accels();

	LOG(LOG_DEBUG, "OUT : install_shortcut()");
}

/* Attach the key configured for a command to a menu item, so GTK renders it on
 * the right-hand side of the entry.  Only the accel map entry is registered
 * for display: key dispatch still goes through install_shortcut(), otherwise a
 * shortcut would fire twice. */
void apply_shortcut_to_menuitem(GtkWidget *item, void (*func)(void))
{
	GtkTreeIter iter;
	guint state, keyval;
	gchar *keystr;
	struct _shortcut_command *command;
	const gchar *label;
	gchar *base, *tab, *new_label;

	if(item == NULL || shortcut_store == NULL)
		return;

	label = gtk_menu_item_get_label(GTK_MENU_ITEM(item));
	if(label == NULL)
		return;

	/* A previous call may have appended a key; start from the plain text. */
	base = g_strdup(label);
	tab = strchr(base, '\t');
	if(tab != NULL)
		*tab = '\0';

	new_label = NULL;

	if(gtk_tree_model_get_iter_first(GTK_TREE_MODEL(shortcut_store), &iter)){
		do {
			command = NULL;
			keystr = NULL;
			gtk_tree_model_get(GTK_TREE_MODEL(shortcut_store), &iter,
					   SHORTCUT_STATE_COLUMN, &state,
					   SHORTCUT_KEYVAL_COLUMN, &keyval,
					   SHORTCUT_KEYSTR_COLUMN, &keystr,
					   SHORTCUT_COMMAND_COLUMN, &command,
					   -1);

			if(command == NULL || command->func != func){
				g_free(keystr);
				continue;
			}

			/* Same rule as install_shortcut(): a bare Return is not
			 * treated as a shortcut. */
			if((state == 0) && (keyval == GDK_Return)){
				g_free(keystr);
				continue;
			}

			if(keystr != NULL && keystr[0] != '\0')
				new_label = g_strdup_printf("%s\t%s", base, keystr);
			g_free(keystr);
			break;

		} while(gtk_tree_model_iter_next(GTK_TREE_MODEL(shortcut_store), &iter));
	}

	/* GtkAccelLabel splits the text at the tab and right-aligns what follows,
	 * which is how GTK renders accelerator hints in menus. */
	if(new_label == NULL)
		gtk_menu_item_set_label(GTK_MENU_ITEM(item), base);
	else{
		gtk_menu_item_set_label(GTK_MENU_ITEM(item), new_label);
		g_free(new_label);
	}

	g_free(base);
}

void install_shortcut_old(){
	GtkTreeIter  iter;
	guint state;
	guint keyval;
	GtkWidget *item;
	gint idx;
	gchar *str;
	struct _shortcut_command *command;

	LOG(LOG_DEBUG, "IN : install_shortcut()");

	idx = 0;

	accel_group = gtk_accel_group_new();

	if(gtk_tree_model_get_iter_first(GTK_TREE_MODEL(shortcut_store), &iter) == TRUE){
		do { 
			gtk_tree_model_get(GTK_TREE_MODEL(shortcut_store), 
					   &iter,
					   SHORTCUT_STATE_COLUMN, &state,
					   SHORTCUT_KEYVAL_COLUMN, &keyval,
					   SHORTCUT_COMMAND_COLUMN, &command,
					   -1);

//			g_snprintf(string, 64, "sc_%s", command->name);
			str = g_strdup_printf("sc_%s", command->name);
			if((state == 0) && (keyval == GDK_Return))
				continue;

			item = gtk_menu_item_new();
			g_signal_connect(G_OBJECT(item), "activate",
					 G_CALLBACK(menuitem_handler),
					 (gpointer)str);

			gtk_widget_add_accelerator (item, "activate", accel_group,
						    keyval, state, GTK_ACCEL_VISIBLE);
			accel_item_list = g_list_append(accel_item_list, item);
			gtk_widget_show(item);

		} while (gtk_tree_model_iter_next(GTK_TREE_MODEL(shortcut_store), &iter) == TRUE);
	}

	LOG(LOG_DEBUG, "OUT : install_shortcut()");
}

void uninstall_shortcut(){
	GList *item;
	GtkWidget *widget;
	
	LOG(LOG_DEBUG, "IN : uninstall_shortcut()");

#if 0
	item = g_list_first(accel_item_list);
	while(item){
		widget = (GtkWidget *)(item->data);
		gtk_widget_destroy(widget);
		item = g_list_next(item);
	}
	g_list_free(accel_item_list);
	accel_item_list = NULL;
	g_object_unref(accel_group);
#endif

	LOG(LOG_DEBUG, "OUT : uninstall_shortcut()");
}

/* Does this key event match whatever the user configured for `name`?
 * Used by context-sensitive commands (e.g. "Search Selected Text") that are
 * handled in the toplevel key_press_event instead of by the accel group --
 * a bare Return cannot be a global accelerator without stealing Enter from
 * the word entry. */
gboolean shortcut_command_matches(const gchar *name, GdkEventKey *event)
{
	GtkTreeIter iter;
	guint state;
	guint keyval;
	struct _shortcut_command *command;

	if(name == NULL || event == NULL || shortcut_store == NULL)
		return(FALSE);

	if(gtk_tree_model_get_iter_first(GTK_TREE_MODEL(shortcut_store), &iter) == FALSE)
		return(FALSE);

	do {
		command = NULL;
		gtk_tree_model_get(GTK_TREE_MODEL(shortcut_store), &iter,
				   SHORTCUT_STATE_COLUMN, &state,
				   SHORTCUT_KEYVAL_COLUMN, &keyval,
				   SHORTCUT_COMMAND_COLUMN, &command,
				   -1);

		if(command == NULL || command->name == NULL ||
		   strcmp(command->name, name) != 0)
			continue;

		/* The three Enter keysyms are one logical key, exactly as in the
		 * old hard-wired handler. */
		if(keyval == GDK_Return){
			if(event->keyval != GDK_Return &&
			   event->keyval != GDK_KP_Enter &&
			   event->keyval != GDK_ISO_Enter)
				continue;
		} else if(keyval != event->keyval){
			continue;
		}

		if(event->state == state)
			return(TRUE);
		if(bignore_locks &&
		   ((event->state | GDK_MOD2_MASK | GDK_LOCK_MASK) ==
		    (state | GDK_MOD2_MASK | GDK_LOCK_MASK)))
			return(TRUE);

	} while(gtk_tree_model_iter_next(GTK_TREE_MODEL(shortcut_store), &iter));

	return(FALSE);
}

gboolean perform_shortcut_by_event(GdkEventKey *event){

	GtkTreeIter  iter;

	guint state;
	guint keyval;
	struct _shortcut_command *command;

	if(gtk_tree_model_get_iter_first(GTK_TREE_MODEL(shortcut_store), &iter) == TRUE){
		do { 
			gtk_tree_model_get(GTK_TREE_MODEL(shortcut_store), 
					   &iter,
					   SHORTCUT_STATE_COLUMN, &state,
					   SHORTCUT_KEYVAL_COLUMN, &keyval,
					   SHORTCUT_COMMAND_COLUMN, &command,
					   -1);

			if(event->keyval == keyval){
				if(event->state == state){
					command->func();
					return(TRUE);
				}

				if((bignore_locks) &&
				   ((event->state | GDK_MOD2_MASK | GDK_LOCK_MASK) == (state | GDK_MOD2_MASK | GDK_LOCK_MASK))){
					command->func();
					return(TRUE);
				}
			}
		} while (gtk_tree_model_iter_next(GTK_TREE_MODEL(shortcut_store), &iter) == TRUE);
	}

	return(FALSE);
}

gboolean perform_shortcut(gchar *name){

	gint i;

	LOG(LOG_DEBUG, "IN : perform_shortcut(%s)", name);

	for(i=0; ; i ++){
		if(commands[i].name == NULL)
			break;

		if(strcmp(commands[i].name, &name[3]) == 0){
			commands[i].func();
			LOG(LOG_DEBUG, "OUT : perform_shortcut() = TRUE");
			return(TRUE);
		}
	}

	LOG(LOG_DEBUG, "OUT : perform_shortcut() = FALSE");
	return(FALSE);
}

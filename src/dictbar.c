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
#include "pref_io.h"
#include "eb.h"
#include "multi.h"

extern GList *group_list;
extern GtkWidget *display_dictbar;
extern GtkWidget *note_bar;

static GtkWidget *dict_bar;
GtkWidget *dict_box;
GtkWidget *combo_group;

/* Width reserved for the group selector; also used to compute how much room
 * is left for the dictionary buttons. */
#define DICT_BAR_COMBO_WIDTH 150
#define DICT_BAR_DROPDOWN_WIDTH 36
#define DICT_BAR_SIDE_PADDING 16

/* Width currently available for dictionary buttons, kept up to date by the
 * size-allocate handler so the buttons re-flow when the window is resized. */
static gint dict_bar_available_width = 0;
static gboolean dict_bar_size_connected = FALSE;

void show_dict_bar()
{

	LOG(LOG_DEBUG, "IN : show_dict_bar()");

	gtk_widget_show(note_bar);
	bshow_dict_bar = 1;
	save_preference();

	gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(display_dictbar),
				       bshow_dict_bar);

	LOG(LOG_DEBUG, "OUT : show_dict_bar()");
}

void hide_dict_bar()
{
	LOG(LOG_DEBUG, "IN : hide_dict_bar()");

	gtk_widget_hide(note_bar);
	bshow_dict_bar = 0;
	save_preference();

	gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(display_dictbar),
				       bshow_dict_bar);

	LOG(LOG_DEBUG, "OUT : hide_dict_bar()");
}

void toggle_dict_bar(){

	LOG(LOG_DEBUG, "IN : toggle_dict_bar()");

	if(bshow_dict_bar == 1)
		hide_dict_bar();
	else 
		show_dict_bar();

	LOG(LOG_DEBUG, "OUT : toggle_dict_bar()");
}

static void dict_toggled(GtkWidget *widget, gpointer data)
{
	gboolean active;
	gboolean button_active;
	int i;
	gint idx = GPOINTER_TO_INT(data);

	GtkTreeIter   parent_iter;
	GtkTreeIter   child_iter;

	LOG(LOG_DEBUG, "IN : dict_toggled(idx=%d)", idx);

	button_active = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));


	if(gtk_tree_model_get_iter_first(GTK_TREE_MODEL(dict_store), &parent_iter) == TRUE){
		do { 
			gchar *title;

			gtk_tree_model_get(GTK_TREE_MODEL(dict_store), 
					   &parent_iter, 
					   DICT_TITLE_COLUMN, &title,
					   DICT_ACTIVE_COLUMN, &active,
					   -1);
			if(active == TRUE){
				i = 0;
				if(gtk_tree_model_iter_children(GTK_TREE_MODEL(dict_store), &child_iter, &parent_iter) == TRUE){
					do {
						if(i == idx) {
							gtk_tree_store_set(dict_store, &child_iter,
									   DICT_ACTIVE_COLUMN, button_active,
									   -1);
							goto END;
						}
						i ++;
					} while (gtk_tree_model_iter_next(GTK_TREE_MODEL(dict_store), &child_iter) == TRUE);
				}
			}
			g_free(title);
	       } while (gtk_tree_model_iter_next(GTK_TREE_MODEL(dict_store), &parent_iter) == TRUE);
	}

 END:
	if(ebook_search_method() == SEARCH_METHOD_MULTI)
		show_multi();

	save_dictgroup();
	LOG(LOG_DEBUG, "OUT : dict_toggled()");
}

static void add_dict_buttons(GtkWidget *bar)
{
	GtkWidget *toggle;
	GtkWidget *label;
	GtkWidget *row = NULL;
	GtkRequisition req;
	gint row_width = 0;
	gint button_width;
	gint available;
	int idx;
	char name[64];
	char buff[256];

	GtkTreeIter   parent_iter;
	GtkTreeIter   child_iter;

	LOG(LOG_DEBUG, "IN : add_dict_buttons()");

	/* Before the first size-allocate there is no measured width, so start
	 * from the saved window size (or a sane fallback). */
	available = dict_bar_available_width;
	if(available < 120)
		available = (window_width > 0 ? window_width : 670)
			    - DICT_BAR_COMBO_WIDTH - DICT_BAR_SIDE_PADDING;
	if(available < 120)
		available = 120;

	idx = 0;

	if(gtk_tree_model_get_iter_first(GTK_TREE_MODEL(dict_store), &parent_iter) == TRUE){
		do { 
			gchar *title;
			gchar *fg, *bg;
			gboolean active;
			BOOK_INFO *binfo;
			gchar *tip_string;

			gtk_tree_model_get (GTK_TREE_MODEL(dict_store), 
					    &parent_iter, 
					    DICT_ACTIVE_COLUMN, &active,
					    -1);
			if(active == TRUE) {
				if(idx != 0){
					LOG(LOG_CRITICAL, "multipe group active");
					return;
				}

				if(gtk_tree_model_iter_children(GTK_TREE_MODEL(dict_store), &child_iter, &parent_iter) == TRUE){
					do {
						gtk_tree_model_get(GTK_TREE_MODEL(dict_store),
								   &child_iter,
								   DICT_TITLE_COLUMN, &title,
								   DICT_ACTIVE_COLUMN, &active,
								   DICT_MEMBER_COLUMN, &binfo,
								   DICT_FGCOLOR_COLUMN, &fg,
								   DICT_BGCOLOR_COLUMN, &bg,
								   -1);

						if(binfo == NULL) {
							continue;
						}

						g_utf8_strncpy(name, title, dict_button_length);
						if(fg == NULL) {
							if(bg == NULL)
								sprintf(buff, "%s", name);
							else
								sprintf(buff, "<span background=\"%s\">%s</span>",  bg, name);
						} else {
							if(bg == NULL)
								sprintf(buff, "<span foreground=\"%s\">%s</span>", fg, name);
							else
								sprintf(buff, "<span foreground=\"%s\" background=\"%s\">%s</span>", fg, bg, name);
						}

						toggle = gtk_toggle_button_new();
						if(benable_button_color)						
							label = gtk_label_new(buff);
						else 
							label = gtk_label_new(name);
						gtk_label_set_use_markup (GTK_LABEL (label), TRUE);

						gtk_container_add (GTK_CONTAINER (toggle), label);

						gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toggle), active);
						g_signal_connect(G_OBJECT(toggle),"toggled",
								 G_CALLBACK(dict_toggled),
								 GINT_TO_POINTER(idx));
				
						/* GTK2 boxes do not wrap: start a new row hbox
						 * whenever the next button would overflow.
						 * size_request ignores children that are not shown,
						 * so show the label first; otherwise the button
						 * reports only its borders and nothing ever wraps. */
						gtk_widget_show(label);
						gtk_widget_size_request(toggle, &req);
						button_width = req.width >= 40 ? req.width : 80;
						if(row == NULL ||
						   (row_width > 0 &&
						    row_width + button_width > available)){
							row = gtk_hbox_new(FALSE, 0);
							gtk_box_pack_start(GTK_BOX(bar), row,
									   FALSE, FALSE, 0);
							row_width = 0;
						}
						gtk_box_pack_start(GTK_BOX(row), toggle,
								   FALSE, FALSE, 2);
						row_width += button_width + 4;
						if(binfo->available == FALSE) {
							gtk_widget_set_sensitive(toggle, FALSE);
						}

						tip_string = g_strconcat(_("Push to enable this dictionary."), "\n(", title, ")", NULL);
						gtk_widget_set_tooltip_text(toggle, tip_string);
						g_free(tip_string);
						g_free(title);

						idx ++;

					} while (gtk_tree_model_iter_next(GTK_TREE_MODEL(dict_store), &child_iter) == TRUE);
				}
			}
	       } while (gtk_tree_model_iter_next(GTK_TREE_MODEL(dict_store), &parent_iter) == TRUE);
	}

	LOG(LOG_DEBUG, "OUT : add_dict_buttons()");
}

static void update_dict_button()
{
	GList *rows, *node;

	LOG(LOG_DEBUG, "IN : update_dict_button()");

	gtk_widget_hide(dict_box);

	/* dict_box is a vbox of row hboxes; destroying a row also destroys
	 * the toggle buttons packed inside it. */
	rows = gtk_container_get_children(GTK_CONTAINER(dict_box));
	for(node = rows; node != NULL; node = node->next)
		gtk_widget_destroy(GTK_WIDGET(node->data));
	g_list_free(rows);

	add_dict_buttons(dict_box);
	gtk_widget_show_all(dict_box);

	LOG(LOG_DEBUG, "OUT : update_dict_button()");

}

/* Re-flow the buttons whenever the window width changes.  The width comes
 * from the parent, so rebuilding rows cannot change it again; the guard
 * below also prevents a size-allocate feedback loop. */
static void dict_bar_size_allocate(GtkWidget *widget, GtkAllocation *allocation,
				   gpointer data)
{
	gint available = allocation->width - DICT_BAR_COMBO_WIDTH
			 - DICT_BAR_SIDE_PADDING;
	gint delta;

	if(available < 120)
		return;
	delta = available - dict_bar_available_width;
	if(delta < 0)
		delta = -delta;
	if(delta < 12)
		return;

	dict_bar_available_width = available;
	if(dict_box != NULL)
		update_dict_button();
}

static gint group_changed (GtkWidget *combo){
	const gchar *text;

	GtkTreeIter   iter;

	LOG(LOG_DEBUG, "IN : group_changed()");

	text = gtk_entry_get_text(GTK_ENTRY(GTK_COMBO(combo_group)->entry));

	if(gtk_tree_model_get_iter_first(GTK_TREE_MODEL(dict_store), &iter) == TRUE){
		do { 
			gchar *title;
			gtk_tree_model_get (GTK_TREE_MODEL(dict_store), 
					    &iter, 
					    DICT_TITLE_COLUMN, &title,
					    -1);

			if(strcmp(title, text) == 0){
				gtk_tree_store_set (dict_store, &iter,
						    DICT_ACTIVE_COLUMN, TRUE,
						    -1);
			} else {
				gtk_tree_store_set (dict_store, &iter,
						    DICT_ACTIVE_COLUMN, FALSE,
						    -1);
			}


	       } while (gtk_tree_model_iter_next(GTK_TREE_MODEL(dict_store), &iter) == TRUE);
	}

	update_dict_button();

	save_dictgroup();

	if(ebook_search_method() == SEARCH_METHOD_MULTI)
		show_multi();


	LOG(LOG_DEBUG, "OUT : group_changed()");

	return(FALSE);
}


GtkWidget *create_dict_bar()
{
	GList *list=NULL;
	gchar *old_group=NULL;
	GList *children;
	GtkBoxChild *child;
	gboolean active_found;
	gboolean old_found;
	GtkTreeIter active_iter;
	GtkTreeIter old_iter;
	GtkTreeIter iter;
	gchar *title;


	LOG(LOG_DEBUG, "IN : create_dict_bar()");

	if(dict_bar){
	        old_group = strdup(gtk_entry_get_text(GTK_ENTRY(GTK_COMBO(combo_group)->entry)));

	        children = GTK_BOX(dict_bar)->children;
		while(children){
		        child = children->data;
			children = children->next;
			gtk_widget_destroy(child->widget);
		}
	} else {
	        dict_bar = gtk_hbox_new(FALSE, 0);
	}
	
	combo_group = gtk_combo_new();
	/* Give the selector the same height as a row of dictionary buttons. */
	gtk_widget_set_size_request(GTK_WIDGET(combo_group),
				    DICT_BAR_COMBO_WIDTH, 28);
	gtk_editable_set_editable(GTK_EDITABLE(GTK_COMBO(combo_group)->entry), FALSE);
	/* GtkCombo's drop-down button is barely wider than its arrow by
	 * default.  Locate it through the public container API (the matching
	 * struct field is private) and make it a comfortable click target. */
	{
		GList *combo_children, *node;

		combo_children = gtk_container_get_children(GTK_CONTAINER(combo_group));
		for(node = combo_children; node != NULL; node = node->next){
			if(GTK_IS_BUTTON(node->data))
				gtk_widget_set_size_request(GTK_WIDGET(node->data),
							    DICT_BAR_DROPDOWN_WIDTH, -1);
		}
		g_list_free(combo_children);
	}
	/* An hbox centres its children vertically.  Wrap the selector in a vbox
	 * so it stays aligned with the first row when the buttons wrap. */
	{
		GtkWidget *combo_holder = gtk_vbox_new(FALSE, 0);
		gtk_box_pack_start(GTK_BOX(combo_holder), combo_group,
				   FALSE, FALSE, 0);
		gtk_box_pack_start(GTK_BOX(dict_bar), combo_holder,
				   FALSE, FALSE, 0);
	}

	gtk_container_set_border_width(GTK_CONTAINER(dict_bar), 1);

	gtk_widget_set_tooltip_text(GTK_COMBO(combo_group)->entry, _("Select dictionary group."));


	active_found = FALSE;
	old_found = FALSE;

	if(gtk_tree_model_get_iter_first(GTK_TREE_MODEL(dict_store), &iter) == TRUE){
		do { 
			gchar *title;
			gboolean active;
			gtk_tree_model_get(GTK_TREE_MODEL(dict_store), 
					   &iter, 
					   DICT_TITLE_COLUMN, &title,
					   DICT_ACTIVE_COLUMN, &active,
					   -1);

			if(active == TRUE){
				active_found = TRUE;
				active_iter = iter;
			}
			if(old_group && (strcmp(title, old_group) == 0)){
				old_found = TRUE;
				old_iter = iter;
			}
			list = g_list_append(list, g_strdup(title));
			g_free(title);
	       } while (gtk_tree_model_iter_next(GTK_TREE_MODEL(dict_store), &iter) == TRUE);
	}

	if(g_list_length(list) != 0)
	        gtk_combo_set_popdown_strings( GTK_COMBO(combo_group), list) ;

	if(active_found == TRUE){
		gtk_tree_model_get(GTK_TREE_MODEL(dict_store), 
				   &active_iter, 
				   DICT_TITLE_COLUMN, &title,
				   -1);
		gtk_entry_set_text(GTK_ENTRY(GTK_COMBO(combo_group)->entry), title);
		g_free(title);
	} else if (old_found == TRUE){
		gtk_tree_model_get(GTK_TREE_MODEL(dict_store), 
				   &old_iter, 
				   DICT_TITLE_COLUMN, &title,
				   -1);
		gtk_entry_set_text(GTK_ENTRY(GTK_COMBO(combo_group)->entry), title);
		g_free(title);

		gtk_tree_store_set(GTK_TREE_STORE(dict_store),
				   &old_iter,
				   DICT_ACTIVE_COLUMN, TRUE,
				   -1);
		
	} else {
		if(gtk_tree_model_get_iter_first(GTK_TREE_MODEL(dict_store), &iter) == TRUE){
		gtk_tree_model_get(GTK_TREE_MODEL(dict_store), 
				   &iter, 
				   DICT_TITLE_COLUMN, &title,
				   -1);
		gtk_entry_set_text(GTK_ENTRY(GTK_COMBO(combo_group)->entry), title);
		g_free(title);

		gtk_tree_store_set(GTK_TREE_STORE(dict_store), &iter,
				   DICT_ACTIVE_COLUMN, TRUE,
				   -1);
		}
	}
  

	g_signal_connect(G_OBJECT (GTK_COMBO(combo_group)->entry), "changed",
			 G_CALLBACK(group_changed),
			 NULL);


	// Re-create buttons
	/* A vbox of hbox rows lets buttons wrap instead of disappearing past
	 * the right edge of the window.  Let it expand so size-allocate sees
	 * the full width available beside the group selector. */
	dict_box = gtk_vbox_new(FALSE, 0);
	gtk_box_pack_start(GTK_BOX (dict_bar), dict_box, TRUE, TRUE, 0);
	add_dict_buttons(dict_box);

	if(dict_bar_size_connected == FALSE){
		g_signal_connect(G_OBJECT(dict_bar), "size-allocate",
				 G_CALLBACK(dict_bar_size_allocate), NULL);
		dict_bar_size_connected = TRUE;
	}

	gtk_widget_show_all(dict_bar);

	LOG(LOG_DEBUG, "OUT : create_dict_bar()");

	return(dict_bar);
}

void update_dict_bar()
{
	
	LOG(LOG_DEBUG, "IN : update_dict_bar()");

	gtk_widget_hide(dict_bar);
	create_dict_bar();
	gtk_widget_show_all(dict_bar);

	/* The dictionary bar is note_bar page 0; File Search has its own bar
	 * on page 1.  After adding dictionaries the new group and its buttons
	 * must be visible, so bring page 0 to the front.  set_search_method()
	 * normally does this, but it runs during start-up before note_bar is
	 * created, and a refresh from the preferences dialog does not call it. */
	if(note_bar != NULL && GTK_IS_NOTEBOOK(note_bar))
		gtk_notebook_set_current_page(GTK_NOTEBOOK(note_bar), 0);

	LOG(LOG_DEBUG, "OUT : update_dict_bar()");

}



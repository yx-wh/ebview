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

#ifndef __PREF_DICTGROUP_H__
#define __PREF_DICTGROUP_H__

#include "defs.h"

GtkWidget *pref_start_dictgroup();
gboolean pref_end_dictgroup();

/* Scan dirname (recursing up to depth levels) for EPWING/EB books and add
 * every usable one to dict_store.  Returns the number added; *skipped (may
 * be NULL) receives how many books had to be skipped.  busy_window is used
 * for the wait cursor and may be NULL.  The caller is responsible for
 * reporting the result and for saving/refreshing the dictionary group. */
gint scan_dictionaries(const gchar *dirname, gint depth,
		       GtkWidget *busy_window, gint *skipped);

#endif /* __PREF_DICTGROUP_H__ */

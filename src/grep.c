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

#include <sys/types.h>
#include <glib/gstdio.h>

#ifndef __WIN32__
#include <sys/wait.h>
#endif

#include "defs.h"

#include "bmh.h"
#include "eb.h"
#include "dialog.h"
#include "dirtree.h"
#include "external.h"
#include "filter.h"
#include "global.h"
#include "headword.h"
#include "history.h"
#include "jcode.h"
#include "mainwindow.h"
#include "misc.h"
#include "pref_io.h"
#include "reg.h"
#include "textview.h"
#include "thread_search.h"


#include <pthread.h>

#define EBOOK_MAX_KEYWORDS 256

void grep_file(gchar *file, gchar *word, gint method);
static void *grep_search_thread(void *arg);
static void list_file_recursive(gchar *dirname, gint depth, gchar *pat);

static GList *grep_file_list=NULL;
static gchar *gword=NULL;

static GtkWidget *grep_bar=NULL;
GtkWidget *combo_dirgroup=NULL;

extern GtkTextBuffer *text_buffer;
extern GtkWidget *main_view;
extern GtkWidget *directory_view;
extern GtkWidget *note_tree;

void grep_search(gchar *word){
	LOG(LOG_DEBUG, "IN : grep_search()");

	clear_search_result();
/*
	grep_search_thread(word);
	show_result_tree();
	return;
*/
	if(gword != NULL)
		g_free(gword);

	gword = strdup(word);

	thread_search(TRUE, _("File Search"), grep_search_thread, gword);	

	LOG(LOG_DEBUG, "OUT : grep_search()");
}

gchar *remove_non_ascii(gchar *str)
{
	gchar *ret;
	gchar *p;

	p = ret = g_strdup(str);
	
	while(*p != '\0') {
		if(!isascii(*p))
			*p = 0x20;
		p ++;
	}
	return(ret);
}


void remember_line(gchar *file, gint page, gint line, gint offset, gchar *heading, gchar *word, gint code){
	RESULT *rp;
	gchar *tmp;
	gchar *p;
	gchar *start;
	gint i;
	gchar buff[512];
	gchar *here;

	LOG(LOG_DEBUG, "IN : remember_line(%s, %d, %s, %s, %d)", file, line, heading, word, code);

	rp = g_new0(RESULT,1);

	p = heading;
	while((*p == ' ') || (*p == '\t'))
		p++;

	switch(code){

	case KCODE_EUC:
		tmp = iconv_convert2("euc-jp", "utf-8", p);
		break;
	case KCODE_JIS:
		tmp = iconv_convert2("iso-2022-jp", "utf-8", p);
		break;
	case KCODE_SJIS:
		tmp = iconv_convert2("Shift_JIS", "utf-8", p);
		break;
	case KCODE_UTF8:
		tmp = g_strdup(p);
		break;
	case KCODE_ASCII:
		tmp = remove_non_ascii(heading);
		break;
	default:
		g_free(rp->word);
		g_free(rp);
		return;
		break;
	}

	/* The search word is already UTF-8 (see start_search()). */
	rp->word = g_strdup(word);

	// Extract 10 characters before/ after keyword

	here = simple_search(rp->word, tmp, strlen(tmp), bignore_case);
	if(here != NULL){
		p = here;
		for(i=0; i < additional_chars ; i ++){
			p = g_utf8_find_prev_char(tmp, p);
			if(p == NULL) {
				p = tmp;
				break;
			}
		}
		start = p;

		if(start != tmp){
			strcpy(buff, "....");
			g_utf8_strncpy(&buff[4], start, additional_chars*2+g_utf8_strlen(word, -1));
		} else {
			g_utf8_strncpy(buff, start, additional_chars + i + g_utf8_strlen(word, -1));
		}

		if(g_utf8_strlen(buff, -1) == additional_chars*2+g_utf8_strlen(word, -1))
			strcat(buff, "....");

		rp->heading = strdup(buff);
		g_free(tmp);
	} else {
		rp->heading = tmp;
	}

	rp->type = RESULT_TYPE_GREP;
	rp->data.grep.filename = native_to_generic(file);
//	rp->data.grep.filename = strdup(file);
	rp->data.grep.page = page;
	rp->data.grep.line = line;
	rp->data.grep.offset = offset;

	add_result(rp);

	LOG(LOG_DEBUG, "OUT : remember_line()");
}

enum {
	METHOD_REGEX,
	METHOD_BMH,
	METHOD_SIMPLE
};

BMH_TABLE *bmh_euc[EBOOK_MAX_KEYWORDS];
BMH_TABLE *bmh_sjis[EBOOK_MAX_KEYWORDS];
BMH_TABLE *bmh_utf8[EBOOK_MAX_KEYWORDS];
REG_TABLE *reg_euc=NULL;
REG_TABLE *reg_sjis=NULL;
REG_TABLE *reg_utf8=NULL;

/* A valid UTF-8 stream containing non-ASCII bytes cannot also be a valid
 * EUC-JP / Shift_JIS stream, so check for UTF-8 before the legacy
 * guess_kanji() heuristic.  Pure ASCII stays on the legacy path. */
static gint detect_code(const gchar *contents)
{
	const gchar *p;

	if(contents == NULL)
		return(KCODE_UNKNOWN);
	if(g_utf8_validate(contents, -1, NULL)){
		for(p = contents; *p != '\0'; p++)
			if((guchar)*p >= 0x80)
				return(KCODE_UTF8);
	}
	return(guess_kanji(max_bytes_to_guess, (guchar *)contents));
}

/* Directories that hold generated or third-party data, never prose. */
static gboolean grep_skip_dir(const gchar *name)
{
	static const gchar *skip[] = {
		".git", ".svn", ".hg", "node_modules", "__pycache__",
		"CMakeFiles", "build", "dist", "target", "Cache", NULL
	};
	guint i;

	for(i = 0; skip[i] != NULL; i++)
		if(strcmp(name, skip[i]) == 0)
			return(TRUE);
	return(FALSE);
}

#define MAX_GREP_FILE_SIZE (2 * 1024 * 1024)
#define MAX_GREP_DEPTH 32

/* Reject oversized and binary files before they are read into memory. */
static gboolean grep_acceptable_file(const gchar *path)
{
	GStatBuf st;
	FILE *fp;
	guchar buf[1024];
	size_t n;

	/* g_stat()/g_fopen() take UTF-8 paths on Windows (GLib converts to
	 * UTF-16 internally); plain stat()/fopen() use the ANSI codepage and
	 * fail on non-ASCII file names. */
	if(g_stat(path, &st) != 0 || !S_ISREG(st.st_mode))
		return(FALSE);
	if(st.st_size > MAX_GREP_FILE_SIZE)
		return(FALSE);
	fp = g_fopen(path, "rb");
	if(fp == NULL)
		return(FALSE);
	n = fread(buf, 1, sizeof(buf), fp);
	fclose(fp);
	if(memchr(buf, '\0', n) != NULL)
		return(FALSE);
	return(TRUE);
}

/* Convert the UTF-8 search word for a legacy-encoded matcher.  g_convert()
 * reports failures through GError instead of logging CRITICAL: a modal
 * dialog at that point deadlocks against the search progress dialog, which
 * owns a GTK grab.  Characters outside the target charset simply disable
 * that matcher; UTF-8 files are still covered by bmh_utf8/reg_utf8. */
static gchar *grep_convert_word(const gchar *word, const gchar *to_code)
{
	GError *error = NULL;
	gchar *result = g_convert(word, -1, to_code, "utf-8", NULL, NULL, &error);

	if(result == NULL){
		LOG(LOG_INFO, "grep: no %s matcher for this word: %s",
		    to_code, error ? error->message : "unknown error");
		if(error)
			g_error_free(error);
	}
	return(result);
}

void grep_file(gchar *file, gchar *word, gint method){
	gchar *contents;
	gchar *p, *pp;
	gint page;
	gint line;
	guchar c1;
	gchar *r=NULL;
	gint i;
	gint code;
	BMH_TABLE **bmh=NULL;
	REG_TABLE *reg=NULL;
	gchar *utf_filename;

	LOG(LOG_DEBUG, "IN : grep_file(%s, %s %d)", file, word, method);

	utf_filename = fs_to_unicode(file);
	push_message(utf_filename);
	g_free(utf_filename);

	contents = get_cache_file(file);
	if(contents == NULL) {
		LOG(LOG_DEBUG, "OUT : grep_file() : NOP");
		return;
	}

	// For higher performance, specially handle EUC and SJIS

	code = detect_code(contents);

	switch(code){
		gchar *tmp;
	case KCODE_EUC:
		push_message(" (EUC)");
		LOG(LOG_DEBUG, "EUC");
		bmh = bmh_euc;
		reg = reg_euc;
		break;

	case KCODE_JIS:
		push_message(" (JIS)");
		LOG(LOG_DEBUG, "JIS");
		tmp = iconv_convert2("iso-2022-jp", "euc-jp", contents);
		g_free(contents);
		contents = tmp;

		bmh = bmh_euc;
		reg = reg_euc;

		code = KCODE_EUC;
		break;

	case KCODE_SJIS:
		push_message(" (SJIS)");
		LOG(LOG_DEBUG, "SJIS");
		bmh = bmh_sjis;
		reg = reg_sjis;
		break;

	case KCODE_UTF8:
		push_message(" (UTF-8)");
		LOG(LOG_DEBUG, "UTF-8");
		bmh = bmh_utf8;
		reg = reg_utf8;
		break;

	case KCODE_ASCII:
		push_message(" (ASCII)");
		LOG(LOG_DEBUG, "ASCII");
		bmh = bmh_euc;
		reg = reg_euc;
		break;

	default:
		push_message(" (Unknown code) ... skipped.");
		LOG(LOG_INFO, "Unknown kanji code  : %s", file);
		g_free(contents);
		return;
		break;
	}

	/* A pattern outside JIS X 0208 has no EUC-JP/Shift_JIS matcher; skip
	 * those files instead of handing NULL to regex_search(). */
	if((method == METHOD_REGEX) && (reg == NULL)){
		push_message(" (pattern unsupported) ... skipped.");
		g_free(contents);
		return;
	}

	push_message(" ... ");

	// Split into lines
	p = contents;
	pp = contents;
	line = 1;
	page = 1;
	while(1){
		if(*pp == '\0'){
			if(method == METHOD_BMH) {
				for(i=0; i < EBOOK_MAX_KEYWORDS; i++) {
					if(bmh[i] == NULL)
						break;
					r = bmh_search(bmh[i], p, pp - p);
					if(r == NULL)
						break;
				}
			} else if (method == METHOD_REGEX)
				r = regex_search(reg, p);
			else
				r = simple_search(word, p, pp - p, bignore_case);

			if(r != NULL)
				remember_line(file, page, line, p - contents, p, word, code);
			break;
		} else if((*pp == 0x0a) || (*pp == 0x0d)) {
			c1 = *pp;
			*pp = '\0';

			if(method == METHOD_BMH)
				for(i=0; i < EBOOK_MAX_KEYWORDS; i++) {
					if(bmh[i] == NULL)
						break;
					r = bmh_search(bmh[i], p, pp - p);
					if(r == NULL)
						break;
				}
			else if (method == METHOD_REGEX)
				r = regex_search(reg, p);
			else
				r = simple_search(word, p, pp - p, bignore_case);

			if(r != NULL)
				remember_line(file, page, line, p - contents, p, word, code);

			*pp = c1;

			if((*pp == 0x0d) && (*(pp+1) == 0x0a)) {
				p = pp = pp + 2;
			} else {
				p = pp = pp + 1;
			}

			line ++;
		} else if(*pp == 0x0c){
			page++;
			pp++;
		} else {
			pp++;
		}

	}

	g_free(contents);

	push_message("OK\n");

	pthread_testcancel();

	LOG(LOG_DEBUG, "OUT : grep_file()");
}

static void list_file_recursive(gchar *dirname, gint depth, gchar *pat)
{
	GDir *dir;
	const gchar *name;
	gchar fullpath[512];

	//LOG(LOG_DEBUG, "IN : list_file_recursive(%s, %d, %s)", dirname, depth, pat);
	
	if((dir = g_dir_open(dirname, 0, NULL)) == NULL){
		if(g_file_test(dirname, G_FILE_TEST_IS_REGULAR) == TRUE){

			if(pat != NULL) {
				// If it matches the pattern ?
				if((strstr(dirname, pat) != NULL) &&
				   (strlen(strstr(dirname, pat))== strlen(pat)))
					grep_file_list = g_list_append(grep_file_list, strdup(dirname));
			} else {
				grep_file_list = g_list_append(grep_file_list, strdup(dirname));
			}
			return;
		}
//		LOG(LOG_CRITICAL, "Failed to determine the type of %s.", dirname);
		LOG(LOG_DEBUG, "OUT : list_file_recursive()");
		return;
	}

	while((name = g_dir_read_name(dir)) != NULL){
		/* Skip dot files when the option says so, plus generated trees
		 * that only produce noise and can be huge. */
		if((name[0] == '.') && bsuppress_hidden_files)
			continue;
		if(grep_skip_dir(name))
			continue;

		if(strcmp(dirname,"/")==0){
			sprintf(fullpath,"/%s",name);
		} else if ((dirname[strlen(dirname) -1] == '\\') ||
			   (dirname[strlen(dirname) -1] == '/')){
			sprintf(fullpath,"%s%s",dirname, name);
		} else {
			sprintf(fullpath,"%s%s%s",dirname, DIR_DELIMITER, name);
		}

		if(g_file_test(fullpath, G_FILE_TEST_IS_REGULAR) == TRUE){
			if(!grep_acceptable_file(fullpath))
				continue;
			if(pat != NULL) {

				// If it matches the pattern ?
				if((strstr(fullpath, pat) != NULL) &&
				   (strlen(strstr(fullpath, pat))== strlen(pat)))
					grep_file_list = g_list_append(grep_file_list, strdup(fullpath));
			} else {
				grep_file_list = g_list_append(grep_file_list, strdup(fullpath));
			}

		} else if(g_file_test(fullpath, G_FILE_TEST_IS_DIR) == TRUE){
			/* Symlinked directories can form cycles. */
			if(depth < MAX_GREP_DEPTH)
				list_file_recursive(fullpath, depth+1, pat);
		}
	}
	g_dir_close(dir);

	//LOG(LOG_DEBUG, "OUT : list_file_recursive()");
}

static gint compare_func(gconstpointer a, gconstpointer b){
	return(strcmp(a,b));
}

static gboolean includes_meta_char(guchar *word)
{
	if((strchr(word, '^') != NULL) ||
	   (strchr(word, '$') != NULL) ||
	   (strchr(word, '[') != NULL) ||
	   //(strchr(word, ']') != NULL) || // Because there must be '['
	   //(strchr(word, '-') != NULL) || // Because there must be '['
	   (strchr(word, '.') != NULL) ||
	   (strchr(word, '*') != NULL) ||
	   (strchr(word, '+') != NULL) ||
	   (strchr(word, '?') != NULL) ||
	   (strchr(word, '|') != NULL) ||
	   (strchr(word, '{') != NULL) ||
	   (strchr(word, '}') != NULL) ||
	   //(strchr(word, ',') != NULL) || // Because there must be '{'
	   (strchr(word, '(') != NULL) ||
	   (strchr(word, ')') != NULL)){
		return(TRUE);
	} else {
		return(FALSE);
	}
}

static void *grep_search_thread(void *arg)
{
	gint state;
	GList *l;
	gint i, j;
	gint filecount;
	gchar *word = (gchar *)arg;
	gint method;
	gchar *l_word=NULL;
	gchar *dirname=NULL;
	gchar *p;
	char *keywords[EBOOK_MAX_KEYWORDS + 1];
	gchar *sjis_word;
	GList *dir_list=NULL;
	const gchar *group;

	LOG(LOG_DEBUG, "IN : grep_search_thread()");

	pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, &state);
	pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, &state);

	// Create file list
	push_message(_("Listing files..."));

	// Clear first
	l = g_list_first(grep_file_list);
	while(l != NULL){
		g_free(l->data);
		l = g_list_next(l);
	}
	if(grep_file_list) {
		g_list_free(grep_file_list);
	}
	grep_file_list = NULL;

	group = gtk_entry_get_text(GTK_ENTRY(GTK_COMBO(combo_dirgroup)->entry));
	if(strcmp(group, _("Manual Select")) == 0) {
		dir_list = get_active_dir_list();

		dir_list = g_list_sort(dir_list, compare_func);

		l = g_list_first(dir_list);
		while(l){
			// Find recursively
			dirname = generic_to_native(l->data);
			list_file_recursive(dirname, 0, NULL);
			g_free(dirname);
			l = g_list_next(l);
		}

		// Sort by name (full path)
		grep_file_list = g_list_sort(grep_file_list, compare_func);

		// Remove duplicate file
		l = g_list_first(grep_file_list);
		while(l){
			GList *next = l->next;
			if((next != NULL) && (strcmp(l->data, next->data) == 0)){
				g_free(next->data);
				grep_file_list = g_list_delete_link(grep_file_list, next);
				continue;
			}
			l = g_list_next(l);
		}

	} else {
		GtkTreeIter iter;

		if(gtk_tree_model_get_iter_first(GTK_TREE_MODEL(dirgroup_store), &iter) == TRUE){
			do { 
				gchar *title;
				gchar *list;
				gboolean active;
				gchar *p, *pp;

				gtk_tree_model_get(GTK_TREE_MODEL(dirgroup_store), 
					   &iter, 
					   DIRGROUP_TITLE_COLUMN, &title,
					   DIRGROUP_LIST_COLUMN, &list,
					   DIRGROUP_ACTIVE_COLUMN, &active,
					   -1);
				if(active == TRUE){
					p = list;
					pp = NULL;
					while(1){
						pp = strchr(p, '\n');
						if(pp == NULL){
							dir_list = g_list_append(dir_list, g_strdup(p));
							break;
						} else {
							*pp = '\0';
							if(strlen(p) != 0)
								dir_list = g_list_append(dir_list, g_strdup(p));
							*pp = '\n';
							p = pp + 1;
						}
					}
				}
				g_free(title);
				g_free(list);
			} while (gtk_tree_model_iter_next(GTK_TREE_MODEL(dirgroup_store), &iter) == TRUE);
		}

		l = g_list_first(dir_list);
		while(l){
			gchar *p;
			// Find recursively
			dirname = unicode_to_fs(l->data);
			p = strchr(dirname, ',');
			if(p == NULL) {
				list_file_recursive(dirname, 0, NULL);
			} else {
				*p = '\0';
				p ++;
				list_file_recursive(dirname, 0, p);
			}
			g_free(dirname);
			g_free(l->data);
			l = g_list_next(l);
		}

		// Sort by name (full path)
		grep_file_list = g_list_sort(grep_file_list, compare_func);
	}

	push_message(_("done\n"));

	g_list_free(dir_list);


	// Determine if it is a regular expression
	if ((word[0] == '\"') && (word[strlen(word) -1] == '\"')){
		method = METHOD_BMH;
		l_word = g_strndup(&word[1], strlen(word) - 2);

		push_message(_("Force ordinary text.\n"));
	} else if(includes_meta_char(word) == TRUE){
		method = METHOD_REGEX;
		l_word = g_strdup(word);

		push_message(_("Seems like regular expression.\n"));

	// If the word is between / and /, it is a regular expression
	} else if ((word[0] == '/') && (word[strlen(word) -1] == '/')){
		method = METHOD_REGEX;
		l_word = g_strndup(&word[1], strlen(word) - 2);

		push_message(_("Force regular expression.\n"));
	} else {
		method = METHOD_BMH;
		l_word = g_strdup(word);

		push_message(_("Seems like ordinary text.\n"));
	}

	/* The search word is UTF-8 (see start_search()).  Derive one matcher
	 * per target encoding; UTF-8 files are matched with the original. */
	if(method == METHOD_BMH) {
		for(i=0; i < EBOOK_MAX_KEYWORDS; i++) {
			bmh_euc[i] = NULL;
			bmh_sjis[i] = NULL;
			bmh_utf8[i] = NULL;
		}

		split_word(l_word, keywords);
		for(i=0, j=0; i < EBOOK_MAX_KEYWORDS; i++){
			gchar *euc_word;

			if(keywords[i] == NULL)
				break;
			if(keywords[i][0] != '\0'){
				euc_word = grep_convert_word(keywords[i], "euc-jp");
				bmh_euc[j] = (euc_word && *euc_word)
					     ? bmh_prepare((guchar *)euc_word, bignore_case)
					     : NULL;
				g_free(euc_word);
				sjis_word = grep_convert_word(keywords[i], "Shift_JIS");
				bmh_sjis[j] = (sjis_word && *sjis_word)
					      ? bmh_prepare((guchar *)sjis_word, bignore_case)
					      : NULL;
				g_free(sjis_word);
				bmh_utf8[j] = bmh_prepare((guchar *)keywords[i], bignore_case);
				j++;
			}
		}
		free_words(keywords);
	}
	else if (method == METHOD_REGEX) {
		gchar *euc_word;

		euc_word = grep_convert_word(l_word, "euc-jp");
		reg_euc = (euc_word && *euc_word)
			  ? regex_prepare((guchar *)euc_word, bignore_case) : NULL;
		g_free(euc_word);
		sjis_word = grep_convert_word(l_word, "Shift_JIS");
		reg_sjis = (sjis_word && *sjis_word)
			   ? regex_prepare((guchar *)sjis_word, bignore_case) : NULL;
		g_free(sjis_word);
		reg_utf8 = regex_prepare((guchar *)l_word, bignore_case);
		if(reg_utf8 == NULL){
			push_message(_("Failed to compile pattern.\n"));
			goto END;
		}
	}



	push_message(_("\nSearching following files...\n"));

	// g_list_length() does not work. Why ?
	//filecount = g_list_length(grep_file_list);

	l = g_list_first(grep_file_list);
	i=0;
	while(l != NULL){
		l = g_list_next(l);
		i++;
	}
	filecount = i;

	l = g_list_first(grep_file_list);
	i=0;
	while(l != NULL){
#ifdef __WIN32__
		p = strrchr(l->data, '\\');
#else
		p = strrchr(l->data, '/');
#endif
		if(p == NULL){
			set_cancel_dlg_text(l->data);
		} else {
			set_cancel_dlg_text(p+1);
		}

		grep_file(l->data, l_word, method);

		set_progress((gfloat)(i+1) / filecount);
		l = g_list_next(l);
		i++;
	}

	if(method == METHOD_BMH) {
		for(i=0; i < EBOOK_MAX_KEYWORDS; i++) {
			if(bmh_euc[i] != NULL) 
				bmh_free(bmh_euc[i]);
			if(bmh_sjis[i] != NULL) 
				bmh_free(bmh_sjis[i]);
			if(bmh_utf8[i] != NULL)
				bmh_free(bmh_utf8[i]);
		}
	}
	else if (method == METHOD_REGEX){
		if(reg_euc != NULL) regex_free(reg_euc);
		if(reg_sjis != NULL) regex_free(reg_sjis);
		if(reg_utf8 != NULL) regex_free(reg_utf8);
	}

	push_message(_("\nFile search completed.\n"));

 END:
	if(l_word)
		g_free(l_word);

	thread_end();

	LOG(LOG_DEBUG, "OUT : grep_search_thread()");

	return(NULL);
}

void show_file(RESULT *rp)
{
	gchar *contents;
	gchar *utf_str;
	GtkTextIter iter;
	GtkTextMark *mark = NULL;
	GtkTextIter *bow, *eow;
	gchar *filename;
	GtkTextIter *bol, *eol;

	gchar *p;
	gchar *line_text;
	gchar *r;
	GtkTextIter start, end;
	gchar *segment;
	gint segment_start, segment_end;
	gint i;
	gint line_no=0;
	gint count;
	char *keywords[EBOOK_MAX_KEYWORDS + 1];
	gint code;

	g_assert(rp->type == RESULT_TYPE_GREP);

	LOG(LOG_DEBUG, "IN : show_file(%s, %d)", rp->data.grep.filename, rp->data.grep.line);

	filename = generic_to_native(rp->data.grep.filename);
	contents = get_cache_file(filename);
	if(contents == NULL)
		return;

	code = detect_code(contents);

	switch(code){
		gchar *tmp;
	case KCODE_EUC:
		break;
	case KCODE_JIS:
		tmp = iconv_convert2("iso-2022-jp", "euc-jp", contents);
		g_free(contents);
		contents = tmp;
		break;
	case KCODE_SJIS:
		break;
	case KCODE_UTF8:
		break;
	case KCODE_ASCII:
		break;
	default:
		LOG(LOG_INFO, "Unknown kanji code  : %s", filename);
		g_free(contents);
		g_free(filename);
		return;
		break;
	}

	g_free(filename);

	// Extract several lines before and after the matched line
	for(i=rp->data.grep.offset, count = 0 ; i > 0 ; i --){
		if(contents[i] == 0x0a) {
			count++;
			if((i != 0) && (contents[i -1] == 0x0d)) {
				i --;
			}
		} else if (contents[i] == 0x0d) {
			count++;
		}
		if(count > additional_lines){
			if((contents[i] == 0x0d) && (contents[i] == 0x0d))
				i += 2;
			else
				i += 1;

			line_no = count-1;
			break;
		}
	}
	if(i <= 0)
		line_no = count;

	segment_start = i;

	if(segment_start < 0)
		segment_start = 0;

	for(i=rp->data.grep.offset, count = 0 ; contents[i] != '\0' ; i ++){
		if(contents[i] == 0x0a) {
			count++;
		} else if (contents[i] == 0x0d) {
			count++;
			if(contents[i + 1] == 0x0a) {
				i ++;
			}
		}

		if(count > additional_lines){
			break;
		}
	}
	segment_end = i;

	segment = g_strndup(&contents[segment_start], segment_end - segment_start);

	if(code == KCODE_SJIS){
		utf_str = iconv_convert("Shift_JIS", "utf-8", segment);
	} else if(code == KCODE_UTF8){
		/* Already UTF-8; converting again would mojibake it. */
		utf_str = g_strdup(segment);
	} else {
		utf_str = iconv_convert("euc-jp", "utf-8", segment);
	}

	// Clear text buffer
	gtk_text_buffer_get_bounds (text_buffer, &start, &end);
	gtk_text_buffer_delete(text_buffer, &start, &end);

	gtk_text_buffer_get_start_iter (text_buffer, &iter);
	gtk_text_buffer_insert_with_tags(
		text_buffer, &iter, 
		utf_str, -1,
		tag_plain, NULL);

	g_free(contents);
	g_free(segment);
	g_free(utf_str);

/*
	gtk_text_buffer_get_start_iter (text_buffer, &iter);
	gtk_text_iter_set_line(&iter, line - 1);
*/
	gtk_text_buffer_get_iter_at_line(text_buffer, &iter, line_no);

	mark = gtk_text_buffer_create_mark(text_buffer, "mark", &iter, TRUE);

	gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(main_view), mark, 
				     0.0,
				     TRUE,
				     0.0, 0.1);

	gtk_text_buffer_delete_mark(text_buffer, mark);

	/* Iterators are invalidated by any buffer modification; scrolling can
	 * run main-loop handlers that insert text.  Re-acquire by line number. */
	gtk_text_buffer_get_iter_at_line(text_buffer, &iter, line_no);

	bol = gtk_text_iter_copy(&iter);
	gtk_text_iter_forward_line(&iter);
	eol = gtk_text_iter_copy(&iter);
	gtk_text_buffer_apply_tag(text_buffer, tag_reverse, bol, eol);
	gtk_text_iter_free(bol);
	gtk_text_iter_free(eol);

	// Emphasize keyword

	if((includes_meta_char(rp->word) == TRUE) ||
	   ((rp->word[0] == '/') && (rp->word[strlen(rp->word) -1] == '/'))){
		goto END;
	}

	split_word(rp->word, keywords);
	for(i=0; i < EBOOK_MAX_KEYWORDS; i++){
		if(keywords[i] == NULL)
			break;
		if(keywords[i][0] != '\0'){
			gtk_text_buffer_get_start_iter (text_buffer, &iter);
			while(1){
				bol = gtk_text_iter_copy(&iter);
				if(gtk_text_iter_forward_line(&iter) == FALSE)
					break;
				eol = gtk_text_iter_copy(&iter);
				line_text = gtk_text_buffer_get_text(text_buffer, bol, eol, FALSE);

				p = line_text;
				while(1){
					r = simple_search(keywords[i], (guchar *)p, strlen(p), bignore_case);
					if(r == NULL)
						break;
					gtk_text_iter_set_line_index(bol, r - line_text);
					bow = gtk_text_iter_copy(bol);
					gtk_text_iter_set_line_index(bol, r - line_text + strlen(keywords[i]));
					eow = gtk_text_iter_copy(bol);
					gtk_text_buffer_apply_tag(text_buffer, tag_colored, bow, eow);
		
					gtk_text_iter_free(bow);
					gtk_text_iter_free(eow);

					p = r + strlen(keywords[i]);
				}
				g_free(line_text);
				gtk_text_iter_free(bol);
				gtk_text_iter_free(eol);
			}



		}
	}
	free_words(keywords);

	gtk_text_buffer_get_start_iter(text_buffer, &iter);
	gtk_text_buffer_place_cursor(text_buffer, &iter);

 END:

	LOG(LOG_DEBUG, "OUT : show_file()");
}

void open_file(RESULT *rp)
{
	GtkTreeIter   iter;
	gchar *p;
	gint found = 0;
	gchar *ext;
	gchar *open_command=NULL;

	gchar buff[512];
	gchar tmp[512];
	gint i;
	gint r;
	gchar *filename;


	g_assert(rp->type == RESULT_TYPE_GREP);

	LOG(LOG_DEBUG, "IN : open_file(%s)", rp->data.grep.filename);

	filename = generic_to_native(rp->data.grep.filename);

	if(gtk_tree_model_get_iter_first(GTK_TREE_MODEL(filter_store), &iter) == TRUE){
		do { 
			gtk_tree_model_get(GTK_TREE_MODEL(filter_store), 
					   &iter, 
					   FILTER_EXT_COLUMN, &ext,
					   FILTER_OPEN_COMMAND_COLUMN, &open_command,
					   -1);

			if(match_extension(filename, ext) == TRUE){
				found = 1;
				break;
			} else {
				g_free(open_command);
				open_command = NULL;
			}
			g_free(ext);
		} while (gtk_tree_model_iter_next(GTK_TREE_MODEL(filter_store), &iter) == TRUE);
	}

	if((found == 0) ||
	   (open_command == NULL) ||
	   (strlen(open_command) == 0))	{
#ifdef __WIN32__
		HINSTANCE hi;
		gunichar2 *wfilename;
		/* filename is UTF-8; ShellExecuteA() would interpret it in the
		 * ANSI codepage and fail on non-ASCII names.  Use the wide API. */
		wfilename = g_utf8_to_utf16(filename, -1, NULL, NULL, NULL);
		hi = ShellExecuteW(NULL, L"open", (LPCWSTR)wfilename,
				   NULL, NULL, SW_SHOWNORMAL);
		g_free(wfilename);
		if((INT_PTR)hi > 32){
			LOG(LOG_DEBUG, "OUT : open_file() = ShellExecute");
			return;
		}
#endif

		LOG(LOG_DEBUG, "use default command");
		g_free(open_command);
		if((open_template == NULL) || 
		   (strlen(open_template) == 0)){
			LOG(LOG_DEBUG, "OUT : open_file() : failed");
			return;
		}
		open_command = strdup(open_template);
	}

	p = open_command;
	i = 0;
	while(1){
		if (*p == '\0'){
			buff[i] = '\0';
			break;
		}
		if(*p == '%'){
			switch (*(p+1)){
			case 'f':
#ifdef __WIN32__
				buff[i] = '\"';
				i++;
#endif
				strcpy(&buff[i], filename);
				i = i + strlen(filename);
#ifdef __WIN32__
				buff[i] = '\"';
				i++;
#endif
				p = p + 2;
				break;
			case 'p':
				sprintf(tmp, "%d", rp->data.grep.page);
				strcpy(&buff[i], tmp);
				i = i + strlen(tmp);
				p = p + 2;
				break;
			case 'l':
				sprintf(tmp, "%d", rp->data.grep.line);
				strcpy(&buff[i], tmp);
				i = i + strlen(tmp);
				p = p + 2;
				break;
			}
		} else {
			buff[i] = *p;
			p++;
			i++;
		}
	}
	g_free(open_command);
	g_free(filename);

	r = launch_external(buff, FALSE);
	
	if(r == 0){
		LOG(LOG_DEBUG, "OUT : open_file()");
		return;
	} else {
		LOG(LOG_DEBUG, "OUT : open_file() : failed");
		return;
	}


}

static gint ignore_case_toggled(GtkWidget *widget, gpointer data)
{
	LOG(LOG_DEBUG, "IN : ignore_case_toggled()");

	bignore_case = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));

	LOG(LOG_DEBUG, "OUT : ignore_case_toggled(%d)", bignore_case);

	return(FALSE);
}

static gint suppress_hidden_toggled(GtkWidget *widget, gpointer data)
{
	LOG(LOG_DEBUG, "IN : suppress_hidden_toggled()");

	bsuppress_hidden_files = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));
	refresh_directory_tree();

	LOG(LOG_DEBUG, "OUT : suppress_hidden_toggled(%d)", bignore_case);

	return(FALSE);
}

static gint dirgroup_changed (GtkWidget *combo){
	const gchar *text;
	GtkTreeIter   iter;

	LOG(LOG_DEBUG, "IN : dirgroup_changed()");

	text = gtk_entry_get_text(GTK_ENTRY(GTK_COMBO(combo_dirgroup)->entry));

	if(strcmp(text, _("Manual Select")) == 0) {
	    if((note_tree != NULL) && 
	       ( ebook_search_method() == SEARCH_METHOD_GREP) &&
	       (gtk_notebook_get_current_page(GTK_NOTEBOOK(note_tree)) != 3))
			set_tree_page(3);

		if(gtk_tree_model_get_iter_first(GTK_TREE_MODEL(dirgroup_store), &iter) == TRUE){
			do { 
				gtk_list_store_set (dirgroup_store, &iter,
						    DIRGROUP_ACTIVE_COLUMN, FALSE,
						    -1);
			} while (gtk_tree_model_iter_next(GTK_TREE_MODEL(dirgroup_store), &iter) == TRUE);
		}

		goto END;

//		gtk_widget_set_sensitive(directory_view, TRUE);
	} else {
//		gtk_widget_set_sensitive(directory_view, FALSE);
	}

	if(gtk_tree_model_get_iter_first(GTK_TREE_MODEL(dirgroup_store), &iter) == TRUE){
		do { 
			gchar *title;
			gtk_tree_model_get (GTK_TREE_MODEL(dirgroup_store), 
					    &iter, 
					    DIRGROUP_TITLE_COLUMN, &title,
					    -1);

			if(strcmp(title, text) == 0){
				gtk_list_store_set (dirgroup_store, &iter,
						    DIRGROUP_ACTIVE_COLUMN, TRUE,
						    -1);
			} else {
				gtk_list_store_set (dirgroup_store, &iter,
						    DIRGROUP_ACTIVE_COLUMN, FALSE,
						    -1);
			}


	       } while (gtk_tree_model_iter_next(GTK_TREE_MODEL(dirgroup_store), &iter) == TRUE);
	}

 END:
	save_dirgroup();

	LOG(LOG_DEBUG, "OUT : dirgroup_changed()");

	return(FALSE);
}


GtkWidget *create_grep_bar()
{
	GtkWidget *button;

	GList *list=NULL;
	gchar *old_group=NULL;
	gboolean active_found;
	gboolean old_found;
	GtkTreeIter active_iter;
	GtkTreeIter old_iter;
	GtkTreeIter iter;
	gchar *title;
	GList *children;
	GtkBoxChild *child;


	LOG(LOG_DEBUG, "IN : create_grep_bar()");

	if(grep_bar){
	        old_group = strdup(gtk_entry_get_text(GTK_ENTRY(GTK_COMBO(combo_dirgroup)->entry)));
	        children = GTK_BOX(grep_bar)->children;
		while(children){
		        child = children->data;
			children = children->next;
			gtk_widget_destroy(child->widget);
		}
	} else {
	        grep_bar = gtk_hbox_new(FALSE, 0);
	}

	gtk_container_set_border_width(GTK_CONTAINER(grep_bar), 1);
	
	combo_dirgroup = gtk_combo_new();
	gtk_widget_set_size_request(GTK_WIDGET(combo_dirgroup), 120, 10);
	gtk_editable_set_editable(GTK_EDITABLE(GTK_COMBO(combo_dirgroup)->entry), FALSE);

	g_signal_connect(G_OBJECT (GTK_COMBO(combo_dirgroup)->entry), "changed",
			 G_CALLBACK(dirgroup_changed),
			 NULL);

	gtk_box_pack_start(GTK_BOX(grep_bar), combo_dirgroup, FALSE, FALSE, 0);


	button = gtk_check_button_new_with_label(_("Suppress Hidden Files"));
	gtk_box_pack_start(GTK_BOX (grep_bar), button, FALSE, FALSE, 5);
	g_signal_connect(G_OBJECT(button), "toggled",
			 G_CALLBACK(suppress_hidden_toggled),
			 NULL);
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(button), bsuppress_hidden_files);
	gtk_widget_set_tooltip_text(button, _("Suppress files whose name start with dot."));

	button = gtk_check_button_new_with_label(_("Ignore Case"));
	gtk_box_pack_start(GTK_BOX (grep_bar), button, FALSE, FALSE, 5);
	g_signal_connect(G_OBJECT(button), "toggled",
			 G_CALLBACK(ignore_case_toggled),
			 NULL);
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(button), bignore_case);
	gtk_widget_set_tooltip_text(button, _("When checked, uppercase letters and lowercase letters are regarded as identical."));

	active_found = FALSE;
	old_found = FALSE;

	if(gtk_tree_model_get_iter_first(GTK_TREE_MODEL(dirgroup_store), &iter) == TRUE){
		do { 
			gchar *title;
			gboolean active;
			gtk_tree_model_get(GTK_TREE_MODEL(dirgroup_store), 
					   &iter, 
					   DIRGROUP_TITLE_COLUMN, &title,
					   DIRGROUP_ACTIVE_COLUMN, &active,
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
	       } while (gtk_tree_model_iter_next(GTK_TREE_MODEL(dirgroup_store), &iter) == TRUE);
	}

	list = g_list_append(list, g_strdup(_("Manual Select")));

	if(g_list_length(list) != 0)
	        gtk_combo_set_popdown_strings( GTK_COMBO(combo_dirgroup), list) ;

	if(active_found == TRUE){
		gtk_tree_model_get(GTK_TREE_MODEL(dirgroup_store), 
				   &active_iter, 
				   DIRGROUP_TITLE_COLUMN, &title,
				   -1);
		gtk_entry_set_text(GTK_ENTRY(GTK_COMBO(combo_dirgroup)->entry), title);
		g_free(title);
	} else if (old_found == TRUE){
		gtk_tree_model_get(GTK_TREE_MODEL(dirgroup_store), 
				   &old_iter, 
				   DIRGROUP_TITLE_COLUMN, &title,
				   -1);
		gtk_entry_set_text(GTK_ENTRY(GTK_COMBO(combo_dirgroup)->entry), title);
		g_free(title);

		gtk_tree_store_set(GTK_TREE_STORE(dirgroup_store),
				   &old_iter,
				   DIRGROUP_ACTIVE_COLUMN, TRUE,
				   -1);
		
	} else {
		gtk_entry_set_text(GTK_ENTRY(GTK_COMBO(combo_dirgroup)->entry), 
				   _("Manual Select"));
	}
  

	LOG(LOG_DEBUG, "OUT : create_grep_bar()");

	return(grep_bar);
}

void update_grep_bar()
{
	
	LOG(LOG_DEBUG, "IN : update_grep_bar()");

	gtk_widget_hide(grep_bar);
	create_grep_bar();
	gtk_widget_show_all(grep_bar);

	LOG(LOG_DEBUG, "OUT : update_grep_bar()");

}

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

#ifdef __WIN32__
#include <direct.h>
#endif


void beep(){
#ifdef __WIN32__
	Beep(1000, 200);
#else
	FILE *fp;

	if(!bbeep_on_nohit)
		return;

	fp = fopen("/dev/console", "w");
	if(fp == NULL){
		return;
	}
	fprintf(fp, "\a");
	fclose(fp);
#endif
}


void remove_space(gchar *f){
	char *p;
	int i;

	p = f;

	// Delete preceding spaces.
	for(i=0;;i++){
		if((f[i] == ' ') || (f[i] == '\t') || (f[i] == '\n') ||
		   (f[i] == ',') || (f[i] == '.') || (f[i] == '!') ||
		   (f[i] == ':') || (f[i] == ';') || (f[i] == '*') ||
		   (f[i] == '(') || (f[i] == ')') )
			p++;
		else
			break;
	}
	memmove(f, p, strlen(p) + 1);

	// Delete following saces
	for(i=(strlen(p) -1); i>=0 ;i--){
		if((f[i] == ' ') || (f[i] == '\t') || (f[i] == '\n') ||
		   (f[i] == ',') || (f[i] == '.') || (f[i] == '!') ||
		   (f[i] == ':') || (f[i] == ';') || (f[i] == '*') ||
		   (f[i] == '(') || (f[i] == ')') )
			f[i] = '\0';
		else
			break;
	}

/*
	if((p = (char *)index(p, '\n')) != NULL)
		*p = '\0';
*/
}

#ifdef __WIN32__
char *getdcwd(int drv, char *path, int len){
	return(_getdcwd(drv, path, len));
}
#endif

gboolean find_file(gchar *filename)
{
#if 0	
	struct stat buf;
	gint rc;
#endif

	LOG(LOG_DEBUG, "IN : find_file(%s)", filename);
#if 0	
	rc = stat(filename, &buf);
	if(rc != 0) {
		LOG(LOG_DEBUG, "OUT : find_file() = FALSE");
		return(FALSE);
	}

	LOG(LOG_DEBUG, "OUT : find_file() = TRUE");
	return(TRUE);
#endif
	if(g_file_test(filename, G_FILE_TEST_IS_REGULAR) || g_file_test(filename, G_FILE_TEST_IS_SYMLINK)){
		LOG(LOG_DEBUG, "OUT : find_file() = TRUE");
		return(TRUE);
	} else {
		LOG(LOG_DEBUG, "OUT : find_file() = FALSE");
		return(FALSE);
	}

}

/* Language tag used to choose the shipped about.<lang> file and the
 * help/<lang>/ directory, e.g. "ja", "zh_CN", "zh_TW" or "en".
 *
 * Windows has no LANG; glib derives the name from the user's locale settings
 * ("ja_JP", "zh_TW", "en_US", ...).  Elsewhere the environment decides, in the
 * same order gettext uses, so that the manual matches the UI. */
const gchar *ui_language(void)
{
	static gchar tag[16];
	const gchar *raw;
	gchar *loc = NULL;
	gint i = 0;

#ifdef __WIN32__
	loc = g_win32_getlocale();
	raw = loc;
#else
	raw = getenv("LC_ALL");
	if(raw == NULL || *raw == '\0')
		raw = getenv("LC_MESSAGES");
	if(raw == NULL || *raw == '\0')
		raw = getenv("LANG");

	/* gettext lets LANGUAGE override the locale, and that is how most
	 * users ask for a translation other than the one their locale
	 * implies (LANGUAGE=zh_CN:en with an English LANG).  Follow the same
	 * rule, including the one exception gettext documents: LANGUAGE is
	 * ignored when the locale is exactly C or POSIX, so that a user who
	 * deliberately asked for untranslated output is not overridden. */
	if(!((raw != NULL) && ((strcmp(raw, "C") == 0) ||
			       (strcmp(raw, "POSIX") == 0)))){
		const gchar *lang = getenv("LANGUAGE");
		if(lang != NULL && *lang != '\0')
			raw = lang;
	}
#endif

	if(raw == NULL || *raw == '\0'){
		g_free(loc);
		return "en";
	}

	/* "zh_CN.UTF-8@euro" -> "zh_CN", and take the first entry of a
	 * LANGUAGE priority list ("zh_CN:ja:en" -> "zh_CN"). */
	while(i < (gint)sizeof(tag) - 1 && raw[i] != '\0'
	      && raw[i] != '.' && raw[i] != '@' && raw[i] != ':'){
		tag[i] = raw[i];
		i++;
	}
	tag[i] = '\0';
	g_free(loc);

	if(strncmp(tag, "ja", 2) == 0)
		return "ja";
	if(strncmp(tag, "zh", 2) == 0){
		if(strstr(tag, "_TW") || strstr(tag, "_HK") || strstr(tag, "_MO")
		   || strstr(tag, "_Hant"))
			return "zh_TW";
		return "zh_CN";
	}
	return "en";
}

gboolean find_config_file(gchar *filename){
	gchar fullpath[512];
	gboolean ret;

	LOG(LOG_DEBUG, "IN : find_config_file(%s)", filename);

	sprintf(fullpath, "%s%s%s", user_dir, DIR_DELIMITER, filename);

        ret = find_file(fullpath);

	LOG(LOG_DEBUG, "OUT : find_config_file()");
	return(ret);
}

gboolean find_or_copy_file(gchar *filename)
{

	gchar fullpath[512];

	LOG(LOG_DEBUG, "IN : find_or_copy_file(%s)", filename);

	sprintf(fullpath, "%s%s%s", user_dir, DIR_DELIMITER, filename);

	if(find_file(fullpath) == FALSE){
		// Copy default file
		gchar srcfile[512];

		snprintf(srcfile, sizeof(srcfile), "%s%s%s", package_dir,
			 DIR_DELIMITER, filename);
		if(find_file(srcfile) != TRUE){
			LOG(LOG_CRITICAL, _("Couldn't find %s. Check installation."), filename);
			LOG(LOG_DEBUG, "IN : find_or_copy_file() = FALSE");
			return(FALSE);
		}

		/* Copy the default file with GLib instead of shelling out to
		 * `cp`: no shell quoting issues, no dependency on coreutils. */
		{
			gchar *contents = NULL;
			gsize length = 0;
			GError *error = NULL;

			if(!g_file_get_contents(srcfile, &contents, &length, &error)){
				LOG(LOG_CRITICAL, _("Couldn't read %s: %s"), srcfile,
				    error ? error->message : "unknown error");
				if(error) g_error_free(error);
				return(FALSE);
			}
			if(!g_file_set_contents(fullpath, contents, length, &error)){
				LOG(LOG_CRITICAL, _("Couldn't write %s: %s"), fullpath,
				    error ? error->message : "unknown error");
				if(error) g_error_free(error);
				g_free(contents);
				return(FALSE);
			}
			g_free(contents);
		}

		// If file does not exist after copy, then error.
		if(find_file(fullpath) == FALSE){
#ifndef __WIN32__
			LOG(LOG_CRITICAL, _("Couldn't open %s. Check installation."), filename);
#endif
			LOG(LOG_DEBUG, "IN : find_or_copy_file() = FALSE");
			return(FALSE);
		}
	}

	LOG(LOG_DEBUG, "IN : find_or_copy_file() = TRUE");
	return(TRUE);
}



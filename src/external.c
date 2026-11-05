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
#include "dialog.h"
#include <pthread.h>

#ifndef __WIN32__
#include <sys/wait.h>
#endif

gint launch_external(gchar *cmd, gboolean wait){

	gint status;
	gboolean ok;
	if (wait) {
		ok = g_spawn_command_line_sync(cmd, NULL, NULL, &status, NULL);
		if (!ok)
			popup_warning(_("Failed to execute command. Please check setting."));
		if (ok) {
			LOG(LOG_DEBUG, "Child exited with status %d\n", status);
		}
		return status;
	} else {
		ok = g_spawn_command_line_async(cmd, NULL);
		if (!ok)
			popup_warning(_("Failed to execute command. Please check setting."));
		return 0;
	}
}

#ifdef __WIN32__
#include <mmsystem.h>
#include <string.h>

/* MCI expects a DWORD_PTR for the parms pointer.  The original code cast it to
 * DWORD, which truncates any 64-bit stack address
 * (0x0000002580DFF790 -> 0x0000000080DFF790) and makes mciSendCommand fault on
 * its first field access -- that was the segfault when clicking a pronunciation
 * in Daijirin.  The parms structs are also zeroed now: MCI reads the fields
 * selected by the flags and the caller left lpstrAlias/dwCallback as garbage.
 *
 * Playback happens on its own thread so a long clip does not freeze the UI, and
 * the worker owns its own copy of the path as UTF-16: the old code shared a
 * global g_filename that the next click freed while the previous clip was still
 * playing.  mciSendCommandW is used because the path comes from GTK as UTF-8 and
 * the temporary directory sits under the user profile, which may be non-ASCII.
 */
static void *play_background(void *arg)
{
	MCI_OPEN_PARMSW open_parms;
	MCI_PLAY_PARMS play_parms;
	MCI_GENERIC_PARMS parms;
	gunichar2 *wpath = (gunichar2 *)arg;

	memset(&open_parms, 0, sizeof open_parms);
	memset(&play_parms, 0, sizeof play_parms);
	memset(&parms, 0, sizeof parms);

	open_parms.lpstrDeviceType = L"waveaudio";
	open_parms.lpstrElementName = (LPCWSTR)wpath;

	if (mciSendCommandW(0, MCI_OPEN,
			    MCI_WAIT | MCI_OPEN_TYPE | MCI_OPEN_ELEMENT,
			    (DWORD_PTR)&open_parms) == 0) {
		play_parms.dwFrom = 0;
		mciSendCommandW(open_parms.wDeviceID, MCI_PLAY,
				MCI_WAIT | MCI_FROM, (DWORD_PTR)&play_parms);
		mciSendCommandW(open_parms.wDeviceID, MCI_STOP, MCI_WAIT,
				(DWORD_PTR)&parms);
		mciSendCommandW(open_parms.wDeviceID, MCI_CLOSE, MCI_WAIT,
				(DWORD_PTR)NULL);
	} else {
		gchar *u8 = g_utf16_to_utf8(wpath, -1, NULL, NULL, NULL);
		LOG(LOG_WARNING, "Failed to open sound device for %s", u8 ? u8 : "?");
		g_free(u8);
	}

	g_free(wpath);

	return NULL;
}
#endif

void play_multimedia(gchar *filename, gint type)
{
	gchar cmd[512];
	gchar *p;
	gchar *template;

	LOG(LOG_DEBUG, "IN : play_multimedia(%s, %d)", filename, type);

	switch(type){
	case TAG_TYPE_MOVIE:
		template = strdup(mpeg_template);
		break;
	case TAG_TYPE_SOUND:
#ifdef __WIN32__
		if(bplay_sound_internally){
			pthread_attr_t thread_attr;
			pthread_t tid;
			gunichar2 *wpath;
			gint rc;

			wpath = g_utf8_to_utf16(filename, -1, NULL, NULL, NULL);
			if(wpath != NULL){
				pthread_attr_init (&thread_attr) ;
				pthread_attr_setstacksize (&thread_attr, 512*1024) ;

				LOG(LOG_DEBUG, "thread_create");
				rc = pthread_create(&tid, &thread_attr, play_background,
						    (void *)wpath);
				pthread_attr_destroy(&thread_attr);

				if(rc == 0){
					/* Nobody joins this thread; detach so its
					 * stack and handle are released when it ends. */
					pthread_detach(tid);
					LOG(LOG_DEBUG, "thread_created");
					return;
				}

				g_free(wpath);
				LOG(LOG_WARNING, "pthread_create failed : %d", rc);
			}
			/* Fall through to the external player instead of giving up. */
		}
#endif
		template = strdup(wave_template);
		break;
	default:
		return;
		break;
	}

	if((template == NULL) || 
	   (strlen(template) == 0)){
#ifdef __WIN32__
		HINSTANCE hi;
		hi = ShellExecute(NULL, "open", filename, NULL, NULL, SW_SHOWNORMAL);
		if(hi <= 32){
			LOG(LOG_CRITICAL, "ShellExecute() failed : %d", hi);
		}
#else
		if(template)
			g_free(template);
#endif
		LOG(LOG_DEBUG, "OUT : play_multimedia()");
		return;
	}

	p = strstr(template, "%f");
	if(p != NULL){
		*p = '%';
		p++;
		*p = 's';
	}

	sprintf(cmd, template, filename);

	launch_external(cmd, FALSE);

	free(template);

	LOG(LOG_DEBUG, "OUT : play_multimedia()");
}

void launch_web_browser(gchar *url){
	gchar *p;
	gchar *template;
	gchar cmd[512];


	LOG(LOG_DEBUG, "IN : launch_web_browser()");

	if((browser_template == NULL) || 
	   (strlen(browser_template) == 0)){
#ifdef __WIN32__
		HINSTANCE hi;
		hi = ShellExecute(NULL, "open", url, NULL, NULL, SW_SHOWNORMAL);
		if(hi <= 32){
			LOG(LOG_CRITICAL, "ShellExecute() failed : %d", hi);
		}
#else
		LOG(LOG_CRITICAL, _("Web browser not set"));
#endif
		return;
	}

	template = strdup(browser_template);
	p = strstr(template, "%f");
	if(p != NULL){
	  *p = '%';
	  p++;
	  *p = 's';
	}
	sprintf(cmd, template, url);

	launch_external(cmd, FALSE);
	g_free(template);

	LOG(LOG_DEBUG, "OUT : launch_web_browser()");
}


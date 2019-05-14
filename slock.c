/* See LICENSE file for license details. */
#define _XOPEN_SOURCE 500
#if HAVE_SHADOW_H
#include <shadow.h>
#endif

#include <ctype.h>
#include <errno.h>
#include <grp.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <X11/extensions/Xrandr.h>
#include <X11/keysym.h>
#include <X11/XF86keysym.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/XKBlib.h>
#include <Imlib2.h>
#include <security/pam_appl.h>

#include "arg.h"
#include "util.h"

char *argv0;

Imlib_Image image;

static char passwd[512];
pam_handle_t *pamh;

#define SLEEP_TIMEOUT (10*60)
static void
alrm_suspend(int sig)
{
	system("systemctl suspend");
	alarm(SLEEP_TIMEOUT);
}

enum {
	INIT,
	CLEAR,
	INPUT,
	FAILED,
	CAPS,
	PAM,
	BLACK,
	NUMCOLS
};

struct lock {
	int screen;
	Window root, win;
	Pixmap pmap;
	Pixmap bgmap;
	unsigned long colors[NUMCOLS];
};

struct xrandr {
	int active;
	int evbase;
	int errbase;
};

#include "config.h"

static void
die(const char *errstr, ...)
{
	va_list ap;

	va_start(ap, errstr);
	vfprintf(stderr, errstr, ap);
	va_end(ap);
	exit(1);
}

#ifdef __linux__
#include <fcntl.h>
#include <linux/oom.h>

static void
dontkillme(void)
{
	FILE *f;
	const char oomfile[] = "/proc/self/oom_score_adj";

	if (!(f = fopen(oomfile, "w"))) {
		if (errno == ENOENT)
			return;
		die("slock: fopen %s: %s\n", oomfile, strerror(errno));
	}
	fprintf(f, "%d", OOM_SCORE_ADJ_MIN);
	if (fclose(f)) {
		if (errno == EACCES)
			die("slock: unable to disable OOM killer. "
			    "Make sure to suid or sgid slock.\n");
		else
			die("slock: fclose %s: %s\n", oomfile, strerror(errno));
	}
}
#endif

static const char *
getusername(void)
{
	const char *user;
	struct passwd *pw;

	/* Check if the current user has a password entry */
	errno = 0;
	if (!(pw = getpwuid(getuid()))) {
		if (errno)
			die("slock: getpwuid: %s\n", strerror(errno));
		else
			die("slock: cannot retrieve password entry\n");
	}
	user = pw->pw_name;

#if HAVE_SHADOW_H
	if (!strcmp(user, "x")) {
		struct spwd *sp;
		if (!(sp = getspnam(pw->pw_name)))
			die("slock: getspnam: cannot retrieve shadow entry. "
			    "Make sure to suid or sgid slock.\n");
		user = sp->sp_pwdp;
	}
#else
	if (!strcmp(user, "*")) {
#ifdef __OpenBSD__
		if (!(pw = getpwuid_shadow(getuid())))
			die("slock: getpwnam_shadow: cannot retrieve shadow entry. "
			    "Make sure to suid or sgid slock.\n");
		user = pw->pw_user;
#else
		die("slock: getpwuid: cannot retrieve shadow entry. "
		    "Make sure to suid or sgid slock.\n");
#endif /* __OpenBSD__ */
	}
#endif /* HAVE_SHADOW_H */

	return user;
}

/* courtesy of i3lock */
/*
 * Callback function for PAM. We only react on password request callbacks.
 *
 */
static int conv_callback(int num_msg, const struct pam_message **msg,
			 struct pam_response **resp, void *appdata_ptr) {
    if (num_msg == 0)
	return 1;

    /* PAM expects an array of responses, one for each message */
    if ((*resp = calloc(num_msg, sizeof(struct pam_response))) == NULL) {
	perror("calloc");
	return 1;
    }

    for (int c = 0; c < num_msg; c++) {
	if (msg[c]->msg_style != PAM_PROMPT_ECHO_OFF &&
	    msg[c]->msg_style != PAM_PROMPT_ECHO_ON)
	    continue;

	/* return code is currently not used but should be set to zero */
	resp[c]->resp_retcode = 0;
	if ((resp[c]->resp = strdup(passwd)) == NULL) {
	    perror("strdup");
	    return 1;
	}
    }

    return 0;
}

static void
readpw(Display *dpy, struct xrandr *rr, struct lock **locks, int nscreens,
       const char *user)
{
	XRRScreenChangeNotifyEvent *rre;
	char buf[32];
	int num, screen, running, failure, oldc, caps, flash, sleep, black;
	unsigned int len, color, indicators;
	KeySym ksym;
	XEvent ev;
	time_t tim;

	flash = 0;
	caps = 0;
	sleep = 0;
	len = 0;
	running = 1;
	failure = 0;
	black = 0;
	tim = time(NULL);
	oldc = INIT;

	if (!XkbGetIndicatorState(dpy, XkbUseCoreKbd, &indicators))
		caps = indicators & 1;

	alarm(SLEEP_TIMEOUT);

	while (running && !XNextEvent(dpy, &ev)) {
		if (ev.type == KeyPress) {
			explicit_bzero(&buf, sizeof(buf));
			num = XLookupString(&ev.xkey, buf, sizeof(buf), &ksym, 0);
			if(IsKeypadKey(ksym)) {
				if (ksym == XK_KP_Enter)
					ksym = XK_Return;
				else if (ksym >= XK_KP_0 && ksym <= XK_KP_9)
					ksym = (ksym - XK_KP_0) + XK_0;
			}
			if(ev.xkey.state & ControlMask) {
				switch(ksym) {
				case XK_z:
					sleep = 1;
				case XK_u:
				case XK_c:
					ksym = XK_Escape;
					break;
				}
			}
			switch (ksym) {
			case XK_F2:
			case XF86XK_AudioPlay:
			case XF86XK_AudioStop:
			case XK_F1:
			case XF86XK_AudioPrev:
			case XF86XK_AudioNext:
			case XF86XK_AudioLowerVolume:
			case XF86XK_AudioRaiseVolume:
			case XF86XK_AudioMute:
				XSendEvent(dpy, DefaultRootWindow(dpy), True, ButtonPressMask, &ev);
				break;
			case XK_Return:
				passwd[len] = '\0';
				errno = 0;
				color = PAM;
				for (screen = 0; screen < nscreens; screen++) {
					XSetWindowBackground(dpy,
						 locks[screen]->win,
						 locks[screen]->colors[color]);
					XClearWindow(dpy, locks[screen]->win);
					XRaiseWindow(dpy, locks[screen]->win);
				}
				XSync(dpy, False);
				if (pam_authenticate(pamh, 0) == PAM_SUCCESS) {
					pam_setcred(pamh, PAM_REFRESH_CRED);
					pam_end(pamh, PAM_SUCCESS);
					running = 0;
				}
				if (running) {
					XBell(dpy, 100);
					failure = 1;
				}
				explicit_bzero(&passwd, sizeof(passwd));
				len = 0;
				break;
			case XK_Escape:
				explicit_bzero(&passwd, sizeof(passwd));
				len = 0;
				failure = 0;
				flash = !flash;
				if (sleep)
					system("systemctl suspend");
				sleep = 0;
				break;
			case XK_BackSpace:
				if (len)
					passwd[--len] = '\0';
				break;
			case XK_Caps_Lock:
				caps = !caps;
				break;
			case XK_Super_L:
			case XK_Super_R:
				black = !black;
				break;
			default:
				if (IsFunctionKey(ksym) ||
			    		IsKeypadKey(ksym) ||
			   	 	IsMiscFunctionKey(ksym) ||
			    		IsPFKey(ksym) ||
			    		IsPrivateKeypadKey(ksym))
					continue;
				if (num && !iscntrl((int)buf[0]) &&
				    (len + num < sizeof(passwd))) {
					memcpy(passwd + len, buf, num);
					len += num;
				}
				break;
			}
			color = len ? (caps ? CAPS : INPUT) :
				((failure || failonclear) ? FAILED :
				 (black ? BLACK : (caps ? CAPS : INIT)));
			if (running && oldc != color) {
				for (screen = 0; screen < nscreens; screen++) {
					if (color == INIT && locks[screen]->bgmap)
						XSetWindowBackgroundPixmap(dpy,
							locks[screen]->win,
							locks[screen]->bgmap);
					else
						XSetWindowBackground(dpy,
					                     locks[screen]->win,
					                     locks[screen]->colors[color]);
					XClearWindow(dpy, locks[screen]->win);
				}
				oldc = color;
			}
		} else if (rr->active && ev.type == rr->evbase + RRScreenChangeNotify) {
			rre = (XRRScreenChangeNotifyEvent*)&ev;
			for (screen = 0; screen < nscreens; screen++) {
				if (locks[screen]->win == rre->window) {
					if (rre->rotation == RR_Rotate_90 ||
					    rre->rotation == RR_Rotate_270)
						XResizeWindow(dpy, locks[screen]->win,
						              rre->height, rre->width);
					else
						XResizeWindow(dpy, locks[screen]->win,
						              rre->width, rre->height);
					XClearWindow(dpy, locks[screen]->win);
					break;
				}
			}
		} else if (ev.type == MotionNotify) {
			running = !(time(NULL) - tim < 3);
		} else {
			for (screen = 0; screen < nscreens; screen++)
				XRaiseWindow(dpy, locks[screen]->win);
		}
		alarm(SLEEP_TIMEOUT);
	}
}

static struct lock *
lockscreen(Display *dpy, struct xrandr *rr, int screen)
{
	char curs[] = {0, 0, 0, 0, 0, 0, 0, 0};
	int i, ptgrab, kbgrab;
	struct lock *lock;
	XColor color, dummy;
	XSetWindowAttributes wa;
	Cursor invisible;

	if (dpy == NULL || screen < 0 || !(lock = malloc(sizeof(struct lock))))
		return NULL;

	lock->screen = screen;
	lock->root = RootWindow(dpy, lock->screen);


	for (i = 0; i < NUMCOLS; i++) {
		XAllocNamedColor(dpy, DefaultColormap(dpy, lock->screen),
		                 colorname[i], &color, &dummy);
		lock->colors[i] = color.pixel;
	}

	if (image) {
		lock->bgmap = XCreatePixmap(dpy, lock->root,
				DisplayWidth(dpy, lock->screen),
				DisplayHeight(dpy, lock->screen),
				DefaultDepth(dpy, lock->screen));
		imlib_context_set_image(image);
		imlib_context_set_display(dpy);
		imlib_context_set_visual(DefaultVisual(dpy, lock->screen));
		imlib_context_set_colormap(DefaultColormap(dpy, lock->screen));
		imlib_context_set_drawable(lock->bgmap);
		//imlib_render_image_on_drawable(0, 0);
		imlib_render_image_on_drawable_at_size(0, 0,
				DisplayWidth(dpy, lock->screen),
				DisplayHeight(dpy, lock->screen));
		imlib_free_image();
	}

	/* init */
	wa.override_redirect = 1;
	wa.background_pixel = lock->colors[INIT];
	lock->win = XCreateWindow(dpy, lock->root, 0, 0,
	                          DisplayWidth(dpy, lock->screen),
	                          DisplayHeight(dpy, lock->screen),
	                          0, DefaultDepth(dpy, lock->screen),
	                          CopyFromParent,
	                          DefaultVisual(dpy, lock->screen),
	                          CWOverrideRedirect | CWBackPixel, &wa);

	if (lock->bgmap)
		XSetWindowBackgroundPixmap(dpy, lock->win, lock->bgmap);

	lock->pmap = XCreateBitmapFromData(dpy, lock->win, curs, 8, 8);
	invisible = XCreatePixmapCursor(dpy, lock->pmap, lock->pmap,
	                                &color, &color, 0, 0);
	XDefineCursor(dpy, lock->win, invisible);

	/* Try to grab mouse pointer *and* keyboard for 600ms, else fail the lock */
	for (i = 0, ptgrab = kbgrab = -1; i < 6; i++) {
		if (ptgrab != GrabSuccess) {
			ptgrab = XGrabPointer(dpy, lock->root, False,
			                      ButtonPressMask | ButtonReleaseMask |
			                      PointerMotionMask, GrabModeAsync,
			                      GrabModeAsync, None, invisible, CurrentTime);
		}
		if (kbgrab != GrabSuccess) {
			kbgrab = XGrabKeyboard(dpy, lock->root, True,
			                       GrabModeAsync, GrabModeAsync, CurrentTime);
		}

		/* input is grabbed: we can lock the screen */
		if (ptgrab == GrabSuccess && kbgrab == GrabSuccess) {
			XMapRaised(dpy, lock->win);
			if (rr->active)
				XRRSelectInput(dpy, lock->win, RRScreenChangeNotifyMask);

			XSelectInput(dpy, lock->root, SubstructureNotifyMask);
			return lock;
		}

		/* retry on AlreadyGrabbed but fail on other errors */
		if ((ptgrab != AlreadyGrabbed && ptgrab != GrabSuccess) ||
		    (kbgrab != AlreadyGrabbed && kbgrab != GrabSuccess))
			break;

		usleep(100000);
	}

	/* we couldn't grab all input: fail out */
	if (ptgrab != GrabSuccess)
		fprintf(stderr, "slock: unable to grab mouse pointer for screen %d\n",
		        screen);
	if (kbgrab != GrabSuccess)
		fprintf(stderr, "slock: unable to grab keyboard for screen %d\n",
		        screen);
	return NULL;
}

static void
usage(void)
{
	die("usage: slock [-v] [cmd [arg ...]]\n");
}

int
main(int argc, char **argv) {
	struct xrandr rr;
	struct lock **locks;
	Display *dpy;
	int ret, s, nlocks, nscreens;
	const char *user;

	ARGBEGIN {
	case 'v':
		fprintf(stderr, "slock-"VERSION"\n");
		return 0;
	default:
		usage();
	} ARGEND

	image = imlib_load_image("/home/dms/.config/i3/wall");

#ifdef __linux__
	dontkillme();
#endif

	errno = 0;

	struct pam_conv pamc = {conv_callback, NULL};
	user = getusername();

	if ((ret = pam_start("i3lock", user, &pamc, &pamh)) != PAM_SUCCESS)
		die("slock: PAM: %s", pam_strerror(pamh,ret));
	if ((ret = pam_set_item(pamh, PAM_TTY, getenv("DISPLAY"))) != PAM_SUCCESS)
		die("slock: PAM: %s", pam_strerror(pamh,ret));

	if (!(dpy = XOpenDisplay(NULL)))
		die("slock: cannot open display\n");

	/* check for Xrandr support */
	rr.active = XRRQueryExtension(dpy, &rr.evbase, &rr.errbase);

	/* get number of screens in display "dpy" and blank them */
	nscreens = ScreenCount(dpy);
	if (!(locks = calloc(nscreens, sizeof(struct lock *))))
		die("slock: out of memory\n");
	for (nlocks = 0, s = 0; s < nscreens; s++) {
		if ((locks[s] = lockscreen(dpy, &rr, s)) != NULL)
			nlocks++;
		else
			break;
	}
	XSync(dpy, 0);

	/* did we manage to lock everything? */
	if (nlocks != nscreens)
		return 1;

	/* run post-lock command */
	if (argc > 0) {
		switch (fork()) {
		case -1:
			die("slock: fork failed: %s\n", strerror(errno));
		case 0:
			if (close(ConnectionNumber(dpy)) < 0)
				die("slock: close: %s\n", strerror(errno));
			execvp(argv[0], argv);
			fprintf(stderr, "slock: execvp %s: %s\n", argv[0], strerror(errno));
			_exit(1);
		}
	}

	/* setup timeout alarm */
	struct sigaction act;
	act.sa_handler = alrm_suspend;
	act.sa_flags = SA_RESTART;
	sigfillset(&act.sa_mask);
	if (sigaction(SIGALRM, &act, NULL) < 0)
		die("slock: sigaction failed: %s\n", strerror(errno));

	/* everything is now blank. Wait for the correct password */
	readpw(dpy, &rr, locks, nscreens, user);

	return 0;
}

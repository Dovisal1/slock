
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

static const char *colorname[NUMCOLS] = {
	[INIT] = "#222222",
	[CLEAR] =   "#ffb340",     /* after initialization */
	[INPUT] =  "#005577",   /* during input */
	[FAILED] = "#CC3333",   /* wrong password */
	[CAPS] = "#ffff4d",
	[PAM] = "#0080b3",
	[BLACK] = "#222222",
};

/* treat a cleared input like a wrong password (color) */
static const int failonclear = 0;

/* time in seconds before the monitor shuts down */
/* set to zero to disable the feature */
static const int sleeptime = 10*60;

/* time in seconds before the monitor shuts down */
static const int monitortime = 60;

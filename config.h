
static const char *colorname[NUMCOLS] = {
	[INIT] = "#222222",
	[CLEAR] =   "#ffb340",     /* after initialization */
	[INPUT] =  "#005577",   /* during input */
	[FAILED] = "#CC3333",   /* wrong password */
	[CAPS] = "#ffff4d",
	[BLACK] = "#222222",
};

/* treat a cleared input like a wrong password (color) */
static const int failonclear = 0;

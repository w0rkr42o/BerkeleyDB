/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 1997,2005 Oracle.  All rights reserved.
 *
 * $Id: os_sleep.c,v 1.6 2007/02/01 13:15:31 bostic Exp $
 */

#include "db_config.h"

#include "db_int.h"

/*
 * __os_sleep --
 *	Yield the processor for a period of time.
 */
void
__os_sleep(dbenv, secs, usecs)
	DB_ENV *dbenv;
	u_long secs, usecs;		/* Seconds and microseconds. */
{
	COMPQUIET(dbenv, NULL);

#ifdef HAVE_BREW_SDK2
	COMPQUIET(secs, 0);
	COMPQUIET(usecs, 0);
#else
	MSLEEP(secs * MS_PER_SEC + usecs / US_PER_MS);
#endif
}

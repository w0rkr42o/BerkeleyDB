/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2004,2006 Oracle.  All rights reserved.
 *
 * $Id: rep_backup.c,v 12.93 2007/01/29 17:42:57 alanb Exp $
 */

#include "db_config.h"

#include "db_int.h"
#include "dbinc/db_page.h"
#include "dbinc/db_am.h"
#include "dbinc/fop.h"
#include "dbinc/lock.h"
#include "dbinc/log.h"
#include "dbinc/mp.h"
#include "dbinc/qam.h"
#include "dbinc/txn.h"

static int __rep_check_uid __P(( DB_ENV *, u_int8_t *, u_int8_t *, u_int8_t *));
static int __rep_filedone __P((DB_ENV *, int, REP *, __rep_fileinfo_args *,
    u_int32_t));
static int __rep_find_dbs __P((DB_ENV *, u_int8_t **, size_t *,
    size_t *, u_int32_t *));
static int __rep_get_fileinfo __P((DB_ENV *, const char *,
    const char *, __rep_fileinfo_args *, u_int8_t *, u_int32_t *));
static int __rep_get_file_list __P((DB_ENV *, DB_FH *, DBT *));
static int __rep_log_setup __P((DB_ENV *,
    REP *, u_int32_t, u_int32_t, DB_LSN *));
static int __rep_mpf_open __P((DB_ENV *, DB_MPOOLFILE **,
    __rep_fileinfo_args *, u_int32_t));
static int __rep_page_gap __P((DB_ENV *, REP *, __rep_fileinfo_args *,
    u_int32_t));
static int __rep_page_sendpages __P((DB_ENV *, int,
    __rep_fileinfo_args *, DB_MPOOLFILE *, DB *));
static int __rep_queue_filedone __P((DB_ENV *, REP *, __rep_fileinfo_args *));
static int __rep_remove_all __P((DB_ENV *, DBT *));
static int __rep_remove_file __P((DB_ENV *, u_int8_t *, const char *,
    u_int32_t));
static int __rep_remove_logs __P((DB_ENV *));
static int __rep_remove_by_list __P((DB_ENV *, void *, u_int32_t));
static int __rep_remove_by_prefix __P((DB_ENV *, const char *, const char *,
    size_t, APPNAME));
static int __rep_walk_dir __P((DB_ENV *, const char *, u_int8_t **, u_int8_t *,
    size_t *, size_t *, u_int32_t *));
static int __rep_write_page __P((DB_ENV *, REP *, __rep_fileinfo_args *));

/*
 * __rep_update_req -
 *	Process an update_req and send the file information to the client.
 *
 * PUBLIC: int __rep_update_req __P((DB_ENV *, int));
 */
int
__rep_update_req(dbenv, eid)
	DB_ENV *dbenv;
	int eid;
{
	DBT updbt, vdbt;
	DB_LOG *dblp;
	DB_LOGC *logc;
	DB_LSN lsn;
	size_t filelen, filesz, updlen;
	u_int32_t filecnt, version;
	u_int8_t *buf, *fp;
	int ret, t_ret;

	/*
	 * Allocate enough for all currently open files and then some.
	 * Optimize for the common use of having most databases open.
	 * Allocate dbentry_cnt * 2 plus an estimated 60 bytes per
	 * file for the filename/path (or multiplied by 120).
	 *
	 * The data we send looks like this:
	 *	__rep_update_args
	 *	__rep_fileinfo_args
	 *	__rep_fileinfo_args
	 *	...
	 */
	dblp = dbenv->lg_handle;
	logc = NULL;
	filecnt = 0;
	filelen = 0;
	updlen = 0;
	filesz = MEGABYTE;
	if ((ret = __os_calloc(dbenv, 1, filesz, &buf)) != 0)
		return (ret);

	/*
	 * First get our file information.  Get in-memory files first
	 * then get on-disk files.
	 */
	fp = buf + sizeof(__rep_update_args);
	if ((ret = __rep_find_dbs(
	    dbenv, &fp, &filesz, &filelen, &filecnt)) != 0)
		goto err;

	/*
	 * Now get our first LSN.  We send the lsn of the first
	 * non-archivable log file.
	 */
	if ((ret = __log_get_stable_lsn(dbenv, &lsn)) != 0)
		goto err;

	/*
	 * Now get the version number of the log file of that LSN.
	 */
	if ((ret = __log_cursor(dbenv, &logc)) != 0)
		goto err;

	memset(&vdbt, 0, sizeof(vdbt));
	/*
	 * Set our log cursor on the LSN we are sending.
	 */
	if ((ret = __logc_get(logc, &lsn, &vdbt, DB_SET)) != 0)
		goto err;

	if ((ret = __logc_version(logc, &version)) != 0)
		goto err;
	/*
	 * Package up the update information.
	 */
	if ((ret = __rep_update_buf(buf, filesz, &updlen,
	    &lsn, version, filecnt)) != 0)
		goto err;
	/*
	 * We have all the file information now.  Send it to the client.
	 */
	DB_INIT_DBT(updbt, buf, filelen + updlen);

	LOG_SYSTEM_LOCK(dbenv);
	lsn = ((LOG *)dblp->reginfo.primary)->lsn;
	LOG_SYSTEM_UNLOCK(dbenv);
	(void)__rep_send_message(
	    dbenv, eid, REP_UPDATE, &lsn, &updbt, 0, 0);

err:	__os_free(dbenv, buf);
	if (logc != NULL && (t_ret = __logc_close(logc)) != 0 && ret == 0)
		ret = t_ret;
	return (ret);
}

/*
 * __rep_find_dbs -
 *	Walk through all the named files/databases including those in the
 *	environment or data_dirs and those that in named and in-memory.  We
 *	need to	open them, gather the necessary information and then close
 *	them. Then we need to figure out if they're already in the dbentry
 *	array.
 *
 * !!!
 * The pointer *fp is expected to point into a buffer that may be used for an
 * UPDATE message, at an offset equal to the size of __rep_update_args.  This
 * assumption is relied upon if the buffer is found to be too small and must be
 * reallocated.
 */
static int
__rep_find_dbs(dbenv, fp, fileszp, filelenp, filecntp)
	DB_ENV *dbenv;
	u_int8_t **fp;
	size_t *fileszp, *filelenp;
	u_int32_t *filecntp;
{
	int ret;
	char **ddir, *real_dir;
	u_int8_t *origfp;

	ret = 0;
	real_dir = NULL;
	if (dbenv->db_data_dir == NULL) {
		/*
		 * If we don't have a data dir, we have just the
		 * env home dir.
		 */
		ret = __rep_walk_dir(dbenv, dbenv->db_home, fp, NULL,
		    fileszp, filelenp, filecntp);
	} else {
		origfp = *fp;
		for (ddir = dbenv->db_data_dir; *ddir != NULL; ++ddir) {
			if ((ret = __db_appname(dbenv, DB_APP_NONE,
			    *ddir, 0, NULL, &real_dir)) != 0)
				break;
			if ((ret = __rep_walk_dir(dbenv, real_dir, fp, origfp,
			    fileszp, filelenp, filecntp)) != 0)
				break;
			__os_free(dbenv, real_dir);
			real_dir = NULL;
		}
	}

	/* Now, collect any in-memory named databases. */
	if (ret == 0)
		ret = __rep_walk_dir(dbenv, NULL,
		    fp, NULL, fileszp, filelenp, filecntp);

	if (real_dir != NULL)
		__os_free(dbenv, real_dir);
	return (ret);
}

/*
 * __rep_walk_dir --
 *
 * This is the routine that walks a directory and fills in the structures
 * that we use to generate messages to the client telling it what
 * files are available.  If the directory name is NULL, then we should
 * walk the list of in-memory named files.
 */
static int
__rep_walk_dir(dbenv, dir, fp, origfp, fileszp, filelenp, filecntp)
	DB_ENV *dbenv;
	const char *dir;
	u_int8_t **fp, *origfp;
	size_t *fileszp, *filelenp;
	u_int32_t *filecntp;
{
	DBT namedbt, uiddbt;
	__rep_fileinfo_args tmpfp;
	size_t len, offset;
	int cnt, first_file, i, ret;
	u_int8_t *rfp, uid[DB_FILE_ID_LEN];
	char *file, **names, *subdb;

	memset(&namedbt, 0, sizeof(namedbt));
	memset(&uiddbt, 0, sizeof(uiddbt));
	if (dir == NULL) {
		RPRINT(dbenv, (dbenv,
		    "Walk_dir: Getting info for in-memory named files"));
		if ((ret = __memp_inmemlist(dbenv, &names, &cnt)) != 0)
			return (ret);
	} else {
		RPRINT(dbenv, (dbenv,
		    "Walk_dir: Getting info for dir: %s", dir));
		if ((ret = __os_dirlist(dbenv, dir, &names, &cnt)) != 0)
			return (ret);
	}
	rfp = NULL;
	if (fp != NULL)
		rfp = *fp;
	RPRINT(dbenv, (dbenv,
	    "Walk_dir: Dir %s has %d files", dir, cnt));
	first_file = 1;
	for (i = 0; i < cnt; i++) {
		RPRINT(dbenv, (dbenv,
		    "Walk_dir: File %d name: %s", i, names[i]));
		/*
		 * Skip DB-owned files: __db*, DB_CONFIG, log*
		 */
		if (strncmp(names[i], "__db", 4) == 0)
			continue;
		if (strncmp(names[i], "DB_CONFIG", 9) == 0)
			continue;
		if (strncmp(names[i], "log", 3) == 0)
			continue;
		/*
		 * We found a file to process.  Check if we need
		 * to allocate more space.
		 */
		if (dir == NULL) {
			file = NULL;
			subdb = names[i];
		} else {
			file = names[i];
			subdb = NULL;
		}
		if ((ret = __rep_get_fileinfo(dbenv,
		    file, subdb, &tmpfp, uid, filecntp)) != 0) {
			/*
			 * If we find a file that isn't a database, skip it.
			 */
			RPRINT(dbenv, (dbenv,
			    "Walk_dir: File %d %s: returned error %s",
			    i, names[i], db_strerror(ret)));
			ret = 0;
			continue;
		}
		RPRINT(dbenv, (dbenv,
    "Walk_dir: File %d (of %d) %s at 0x%lx: pgsize %lu, max_pgno %lu",
		    tmpfp.filenum, *filecntp, names[i], P_TO_ULONG(rfp),
		    (u_long)tmpfp.pgsize, (u_long)tmpfp.max_pgno));

		/*
		 * Check if we already have info on this file.  Since we're
		 * walking directories, we only need to check the first
		 * file to discover if we have a duplicate data_dir.
		 */
		if (first_file && origfp != NULL) {
			/*
			 * If we have any file info, check if we have this uid.
			 */
			if (rfp != origfp &&
			    (ret = __rep_check_uid(dbenv, origfp,
			    origfp + *filelenp, uid)) != 0) {
				/*
				 * If we have this uid.  Adjust the file
				 * count and stop processing this dir.
				 */
				if (ret == DB_KEYEXIST) {
					ret = 0;
					(*filecntp)--;
				}
				goto err;
			}
			first_file = 0;
		}

		DB_SET_DBT(namedbt, names[i], strlen(names[i]) + 1);
		DB_SET_DBT(uiddbt, uid, DB_FILE_ID_LEN);
retry:		ret = __rep_fileinfo_buf(rfp, *fileszp, &len,
		    tmpfp.pgsize, tmpfp.pgno, tmpfp.max_pgno,
		    tmpfp.filenum, tmpfp.id, tmpfp.type,
		    tmpfp.flags, &uiddbt, &namedbt);
		if (ret == ENOMEM) {
			offset = (size_t)(rfp - *fp);
			*fileszp *= 2;
			/*
			 * Need to account for update info on both sides
			 * of the allocation.
			 */
			*fp -= sizeof(__rep_update_args);
			if ((ret = __os_realloc(dbenv, *fileszp, *fp)) != 0)
				break;
			*fp += sizeof(__rep_update_args);
			rfp = *fp + offset;
			/*
			 * Now that we've reallocated the space, try to
			 * store it again.
			 */
			goto retry;
		}
		rfp += len;
		*fp = rfp;
		*filelenp += len;
	}
err:
	__os_dirfree(dbenv, names, cnt);
	return (ret);
}

/*
 * This function is called when we process the first file of any
 * new directory for internal init.  We walk the list of current
 * files to see if we have already processed these files.  This
 * is to prevent transmitting the same file multiple times if the
 * user calls dbenv->set_data_dir on the same directory more than once.
 */
static int
__rep_check_uid(dbenv, fp, endfp, uid)
	DB_ENV *dbenv;
	u_int8_t *fp, *endfp, *uid;
{
	__rep_fileinfo_args *rfp;
	u_int8_t *fuid;
	int ret;
	void *next;

	ret = 0;
	next = fp;
	rfp = NULL;
	while (next <= (void *)endfp) {
		if ((ret =
		    __rep_fileinfo_read(dbenv, next, &next, &rfp)) != 0) {
			__db_errx(dbenv, "Rep_check_uid: Could not malloc");
			goto err;
		}
		fuid = (u_int8_t *)rfp->uid.data;
		if (memcmp(fuid, uid, DB_FILE_ID_LEN) == 0) {
			RPRINT(dbenv, (dbenv,
			    "Check_uid: Found matching file."));
			ret = DB_KEYEXIST;
			goto err;
		}
		__os_free(dbenv, rfp);
		rfp = NULL;
	}
err:
	if (rfp != NULL)
		__os_free(dbenv, rfp);
	return (ret);

}

static int
__rep_get_fileinfo(dbenv, file, subdb, rfp, uid, filecntp)
	DB_ENV *dbenv;
	const char *file, *subdb;
	__rep_fileinfo_args *rfp;
	u_int8_t *uid;
	u_int32_t *filecntp;
{
	DB *dbp, *entdbp;
	DB_LOCK lk;
	DB_LOG *dblp;
	DB_MPOOLFILE *mpf;
	DBC *dbc;
	DBMETA *dbmeta;
	PAGE *pagep;
	int i, ret, t_ret;

	dbp = NULL;
	dbc = NULL;
	pagep = NULL;
	mpf = NULL;
	LOCK_INIT(lk);

	if ((ret = __db_create_internal(&dbp, dbenv, 0)) != 0)
		goto err;
	if ((ret = __db_open(dbp, NULL, file, subdb, DB_UNKNOWN,
	    DB_RDONLY | (F_ISSET(dbenv, DB_ENV_THREAD) ? DB_THREAD : 0),
	    0, PGNO_BASE_MD)) != 0)
		goto err;

	if ((ret = __db_cursor(dbp, NULL, &dbc, 0)) != 0)
		goto err;
	if ((ret = __db_lget(
	    dbc, 0, dbp->meta_pgno, DB_LOCK_READ, 0, &lk)) != 0)
		goto err;
	if ((ret = __memp_fget(dbp->mpf, &dbp->meta_pgno, dbc->txn,
	    0, &pagep)) != 0)
		goto err;
	/*
	 * We have the meta page.  Set up our information.
	 */
	dbmeta = (DBMETA *)pagep;
	rfp->pgno = 0;
	/*
	 * Queue is a special-case.  We need to set max_pgno to 0 so that
	 * the client can compute the pages from the meta-data.
	 */
	if (dbp->type == DB_QUEUE)
		rfp->max_pgno = 0;
	else
		rfp->max_pgno = dbmeta->last_pgno;
	rfp->pgsize = dbp->pgsize;
	memcpy(uid, dbp->fileid, DB_FILE_ID_LEN);
	rfp->filenum = (*filecntp)++;
	rfp->type = (u_int32_t)dbp->type;
	rfp->flags = dbp->flags;
	rfp->id = DB_LOGFILEID_INVALID;
	ret = __memp_fput(dbp->mpf, pagep, dbc->priority);
	pagep = NULL;
	if ((t_ret = __LPUT(dbc, lk)) != 0 && ret == 0)
		ret = t_ret;
	if (ret != 0)
		goto err;
err:
	if ((t_ret = __LPUT(dbc, lk)) != 0 && ret == 0)
		ret = t_ret;
	if (dbc != NULL && (t_ret = __dbc_close(dbc)) != 0 && ret == 0)
		ret = t_ret;
	if (pagep != NULL && (t_ret =
	    __memp_fput(mpf, pagep, dbc->priority)) != 0 && ret == 0)
		ret = t_ret;
	if (dbp != NULL && (t_ret = __db_close(dbp, NULL, 0)) != 0 && ret == 0)
		ret = t_ret;
	/*
	 * We walk the entry table now, after closing the dbp because
	 * otherwise we find the open from this function and the id
	 * is useless in that case.
	 */
	if (ret == 0) {
		LOG_SYSTEM_LOCK(dbenv);
		/*
		 * Walk entry table looking for this uid.
		 * If we find it, save the id.
		 */
		for (dblp = dbenv->lg_handle,
		    i = 0; i < dblp->dbentry_cnt; i++) {
			entdbp = dblp->dbentry[i].dbp;
			if (entdbp == NULL)
				break;
			DB_ASSERT(dbenv, entdbp->log_filename != NULL);
			if (memcmp(uid,
			    entdbp->log_filename->ufid,
			    DB_FILE_ID_LEN) == 0)
				rfp->id = i;
		}
		LOG_SYSTEM_UNLOCK(dbenv);
	}
	return (ret);
}

/*
 * __rep_page_req
 *	Process a page_req and send the page information to the client.
 *
 * PUBLIC: int __rep_page_req __P((DB_ENV *, int, DBT *));
 */
int
__rep_page_req(dbenv, eid, rec)
	DB_ENV *dbenv;
	int eid;
	DBT *rec;
{
	__rep_fileinfo_args *msgfp;
	DB *dbp;
	DBT msgdbt;
	DB_LOG *dblp;
	DB_MPOOLFILE *mpf;
	DB_REP *db_rep;
	REP *rep;
	int ret, t_ret;
	void *next;

	db_rep = dbenv->rep_handle;
	rep = db_rep->region;
	dblp = dbenv->lg_handle;

	if ((ret = __rep_fileinfo_read(dbenv, rec->data, &next, &msgfp)) != 0)
		return (ret);

	/*
	 * See if we can find it already.  If so we can quickly access its
	 * mpool and process.  Otherwise we have to open the file ourselves.
	 */
	RPRINT(dbenv, (dbenv, "page_req: file %d page %lu to %lu",
	    msgfp->filenum, (u_long)msgfp->pgno, (u_long)msgfp->max_pgno));
	LOG_SYSTEM_LOCK(dbenv);
	if (msgfp->id >= 0 && dblp->dbentry_cnt > msgfp->id) {
		dbp = dblp->dbentry[msgfp->id].dbp;
		if (dbp != NULL) {
			DB_ASSERT(dbenv, dbp->log_filename != NULL);
			if (memcmp(msgfp->uid.data, dbp->log_filename->ufid,
			    DB_FILE_ID_LEN) == 0) {
				LOG_SYSTEM_UNLOCK(dbenv);
				RPRINT(dbenv, (dbenv,
				    "page_req: found %d in dbreg",
				    msgfp->filenum));
				ret = __rep_page_sendpages(dbenv, eid,
				    msgfp, dbp->mpf, dbp);
				goto err;
			}
		}
	}
	LOG_SYSTEM_UNLOCK(dbenv);

	/*
	 * If we get here, we do not have the file open via dbreg.
	 * We need to open the file and then send its pages.
	 * If we cannot open the file, we send REP_FILE_FAIL.
	 */
	RPRINT(dbenv,
	    (dbenv, "page_req: Open %d via mpf_open", msgfp->filenum));
	if ((ret = __rep_mpf_open(dbenv, &mpf, msgfp, 0)) != 0) {
		memset(&msgdbt, 0, sizeof(msgdbt));
		msgdbt.data = msgfp;
		msgdbt.size = sizeof(*msgfp);
		RPRINT(dbenv, (dbenv, "page_req: Open %d failed",
		    msgfp->filenum));
		if (F_ISSET(rep, REP_F_MASTER))
			(void)__rep_send_message(dbenv, eid, REP_FILE_FAIL,
			    NULL, &msgdbt, 0, 0);
		else
			ret = DB_NOTFOUND;
		goto err;
	}

	ret = __rep_page_sendpages(dbenv, eid, msgfp, mpf, NULL);
	t_ret = __memp_fclose(mpf, 0);
	if (ret == 0 && t_ret != 0)
		ret = t_ret;
err:
	__os_free(dbenv, msgfp);
	return (ret);
}

static int
__rep_page_sendpages(dbenv, eid, msgfp, mpf, dbp)
	DB_ENV *dbenv;
	int eid;
	__rep_fileinfo_args *msgfp;
	DB_MPOOLFILE *mpf;
	DB *dbp;
{
	DB *qdbp;
	DBT lockdbt, msgdbt, pgdbt;
	DB_LOCK lock;
	DB_LOCK_ILOCK lock_obj;
	DB_LOG *dblp;
	DB_LSN lsn;
	DB_REP *db_rep;
	PAGE *pagep;
	REP *rep;
	REP_BULK bulk;
	REP_THROTTLE repth;
	db_pgno_t p;
	uintptr_t bulkoff;
	size_t len, msgsz;
	u_int32_t bulkflags, lockid, use_bulk;
	int opened, ret, t_ret;
	u_int8_t *buf;

	db_rep = dbenv->rep_handle;
	rep = db_rep->region;
	lockid = DB_LOCK_INVALIDID;
	opened = 0;
	qdbp = NULL;
	buf = NULL;
	bulk.addr = NULL;
	use_bulk = FLD_ISSET(rep->config, REP_C_BULK);
	if (msgfp->type == (u_int32_t)DB_QUEUE) {
		if (dbp == NULL) {
			if ((ret = __db_create_internal(&qdbp, dbenv, 0)) != 0)
				goto err;
			/*
			 * We need to check whether this is in-memory so that
			 * we pass the name correctly as either the file or
			 * the database name.
			 */
			if ((ret = __db_open(qdbp, NULL,
			    FLD_ISSET(msgfp->flags, DB_AM_INMEM) ?
			    NULL : msgfp->info.data,
			    FLD_ISSET(msgfp->flags, DB_AM_INMEM) ?
			    msgfp->info.data : NULL,
			    DB_UNKNOWN,
			    DB_RDONLY | (F_ISSET(dbenv, DB_ENV_THREAD) ?
			    DB_THREAD : 0), 0, PGNO_BASE_MD)) != 0)
				goto err;
			opened = 1;
		} else
			qdbp = dbp;
	}
	msgsz = sizeof(__rep_fileinfo_args) + DB_FILE_ID_LEN + msgfp->pgsize;
	if ((ret = __os_calloc(dbenv, 1, msgsz, &buf)) != 0)
		goto err;
	memset(&msgdbt, 0, sizeof(msgdbt));
	memset(&pgdbt, 0, sizeof(pgdbt));
	RPRINT(dbenv, (dbenv, "sendpages: file %d page %lu to %lu",
	    msgfp->filenum, (u_long)msgfp->pgno, (u_long)msgfp->max_pgno));
	memset(&repth, 0, sizeof(repth));
	/*
	 * If we're doing bulk transfer, allocate a bulk buffer to put our
	 * pages in.  We still need to initialize the throttle info
	 * because if we encounter a page larger than our entire bulk
	 * buffer, we need to send it as a singleton.
	 *
	 * Use a local var so that we don't need to worry if someone else
	 * turns on/off bulk in the middle of our call here.
	 */
	if (use_bulk && (ret = __rep_bulk_alloc(dbenv, &bulk, eid,
	    &bulkoff, &bulkflags, REP_BULK_PAGE)) != 0)
		goto err;
	REP_SYSTEM_LOCK(dbenv);
	repth.gbytes = rep->gbytes;
	repth.bytes = rep->bytes;
	repth.type = REP_PAGE;
	repth.data_dbt = &msgdbt;
	REP_SYSTEM_UNLOCK(dbenv);

	/*
	 * Set up locking.
	 */
	LOCK_INIT(lock);
	memset(&lock_obj, 0, sizeof(lock_obj));
	if ((ret = __lock_id(dbenv, &lockid, NULL)) != 0)
		goto err;
	memcpy(lock_obj.fileid, mpf->fileid, DB_FILE_ID_LEN);
	lock_obj.type = DB_PAGE_LOCK;

	memset(&lockdbt, 0, sizeof(lockdbt));
	lockdbt.data = &lock_obj;
	lockdbt.size = sizeof(lock_obj);

	for (p = msgfp->pgno; p <= msgfp->max_pgno; p++) {
		/*
		 * We're not waiting for the lock, if we cannot get
		 * the lock for this page, skip it.  The gap
		 * code will rerequest it.
		 */
		lock_obj.pgno = p;
		if ((ret = __lock_get(dbenv, lockid, DB_LOCK_NOWAIT, &lockdbt,
		    DB_LOCK_READ, &lock)) != 0) {
			/*
			 * Continue if we couldn't get the lock.
			 */
			if (ret == DB_LOCK_NOTGRANTED)
				continue;
			/*
			 * Otherwise we have an error.
			 */
			goto err;
		}
		if (msgfp->type == (u_int32_t)DB_QUEUE && p != 0)
#ifdef HAVE_QUEUE
			ret = __qam_fget(qdbp, &p, NULL,
			    DB_MPOOL_CREATE, &pagep);
#else
			ret = DB_PAGE_NOTFOUND;
#endif
		else
			ret = __memp_fget(mpf, &p, NULL,
			    DB_MPOOL_CREATE, &pagep);
		if (ret == DB_PAGE_NOTFOUND) {
			memset(&pgdbt, 0, sizeof(pgdbt));
			ZERO_LSN(lsn);
			msgfp->pgno = p;
			if (F_ISSET(rep, REP_F_MASTER)) {
				ret = 0;
				RPRINT(dbenv, (dbenv,
				    "sendpages: PAGE_FAIL on page %lu",
				    (u_long)p));
				(void)__rep_send_message(dbenv, eid,
				    REP_PAGE_FAIL, &lsn, &msgdbt, 0, 0);
			} else
				ret = DB_NOTFOUND;
			goto lockerr;
		} else if (ret != 0)
			goto lockerr;
		else
			DB_SET_DBT(pgdbt, pagep, msgfp->pgsize);
		len = 0;
		RPRINT(dbenv, (dbenv,
		    "sendpages: %lu, page lsn [%lu][%lu]", (u_long)p,
		    (u_long)pagep->lsn.file, (u_long)pagep->lsn.offset));
		ret = __rep_fileinfo_buf(buf, msgsz, &len,
		    msgfp->pgsize, p, msgfp->max_pgno,
		    msgfp->filenum, msgfp->id, msgfp->type,
		    msgfp->flags, &msgfp->uid, &pgdbt);
		if (msgfp->type != (u_int32_t)DB_QUEUE || p == 0)
			t_ret = __memp_fput(mpf, pagep, DB_PRIORITY_UNCHANGED);
#ifdef HAVE_QUEUE
		else
			/*
			 * We don't need an #else for HAVE_QUEUE here because if
			 * we're not compiled with queue, then we're guaranteed
			 * to have set REP_PAGE_FAIL above.
			 */
			t_ret = __qam_fput(qdbp, p, pagep, qdbp->priority);
#endif
		if ((t_ret = __ENV_LPUT(dbenv, lock)) != 0 && ret == 0)
			ret = t_ret;
		if (ret != 0)
			goto err;

		DB_ASSERT(dbenv, len <= msgsz);
		DB_SET_DBT(msgdbt, buf, len);

		dblp = dbenv->lg_handle;
		LOG_SYSTEM_LOCK(dbenv);
		repth.lsn = ((LOG *)dblp->reginfo.primary)->lsn;
		LOG_SYSTEM_UNLOCK(dbenv);
		/*
		 * If we are configured for bulk, try to send this as a bulk
		 * request.  If not configured, or it is too big for bulk
		 * then just send normally.
		 */
		if (use_bulk)
			ret = __rep_bulk_message(dbenv, &bulk, &repth,
			    &repth.lsn, &msgdbt, 0);
		if (!use_bulk || ret == DB_REP_BULKOVF)
			ret = __rep_send_throttle(dbenv, eid, &repth, 0);
		RPRINT(dbenv, (dbenv,
		    "sendpages: %lu, lsn [%lu][%lu]", (u_long)p,
		    (u_long)repth.lsn.file, (u_long)repth.lsn.offset));
		/*
		 * If we have REP_PAGE_MORE
		 * we need to break this loop after giving the page back
		 * to mpool.  Otherwise, with REP_PAGE, we keep going.
		 */
		if (ret == 0)
			ret = t_ret;
		if (repth.type == REP_PAGE_MORE || ret != 0)
			break;
	}

	if (0) {
lockerr:	if ((t_ret = __ENV_LPUT(dbenv, lock)) != 0 && ret == 0)
			ret = t_ret;
	}
err:
	/*
	 * We're done, force out whatever remains in the bulk buffer and
	 * free it.
	 */
	if (use_bulk && bulk.addr != NULL &&
	    (t_ret = __rep_bulk_free(dbenv, &bulk, 0)) != 0 && ret == 0)
		ret = t_ret;
	if (opened && (t_ret = __db_close(qdbp, NULL, DB_NOSYNC)) != 0 &&
	    ret == 0)
		ret = t_ret;
	if (buf != NULL)
		__os_free(dbenv, buf);
	if (lockid != DB_LOCK_INVALIDID && (t_ret = __lock_id_free(dbenv,
	    lockid)) != 0 && ret == 0)
		ret = t_ret;
	return (ret);
}

/*
 * __rep_update_setup
 *	Process and setup with this file information.
 *
 * PUBLIC: int __rep_update_setup __P((DB_ENV *, int, REP_CONTROL *, DBT *));
 */
int
__rep_update_setup(dbenv, eid, rp, rec)
	DB_ENV *dbenv;
	int eid;
	REP_CONTROL *rp;
	DBT *rec;
{
	DB_LOG *dblp;
	DB_REP *db_rep;
	DBT pagereq_dbt;
	LOG *lp;
	REGENV *renv;
	REGINFO *infop;
	REP *rep;
	__rep_update_args *rup;
	int ret;
	u_int32_t count, infolen;
	void *next;

	db_rep = dbenv->rep_handle;
	rep = db_rep->region;
	dblp = dbenv->lg_handle;
	lp = dblp->reginfo.primary;
	ret = 0;

	REP_SYSTEM_LOCK(dbenv);
	if (!F_ISSET(rep, REP_F_RECOVER_UPDATE) || IN_ELECTION(rep)) {
		REP_SYSTEM_UNLOCK(dbenv);
		return (0);
	}
	F_CLR(rep, REP_F_RECOVER_UPDATE);
	/*
	 * We know we're the first to come in here due to the
	 * REP_F_RECOVER_UPDATE flag.
	 */
	F_SET(rep, REP_F_RECOVER_PAGE);
	/*
	 * We do not clear REP_F_READY_* in this code.
	 * We'll eventually call the normal __rep_verify_match recovery
	 * code and that will clear all the flags and allow others to
	 * proceed.  We only need to lockout the API here.  We do not
	 * need to lockout other message threads.
	 */
	if ((ret = __rep_lockout_api(dbenv, rep)) != 0)
		goto err;
	/*
	 * We need to update the timestamp and kill any open handles
	 * on this client.  The files are changing completely.
	 */
	infop = dbenv->reginfo;
	renv = infop->primary;
	(void)time(&renv->rep_timestamp);

	REP_SYSTEM_UNLOCK(dbenv);
	MUTEX_LOCK(dbenv, rep->mtx_clientdb);
	lp->wait_recs = rep->request_gap;
	lp->rcvd_recs = 0;
	ZERO_LSN(lp->ready_lsn);
	ZERO_LSN(lp->verify_lsn);
	ZERO_LSN(lp->waiting_lsn);
	ZERO_LSN(lp->max_wait_lsn);
	ZERO_LSN(lp->max_perm_lsn);
	MUTEX_UNLOCK(dbenv, rep->mtx_clientdb);
	if ((ret = __rep_update_read(dbenv, rec->data, &next, &rup)) != 0)
		goto err_nolock;

	/*
	 * We need to empty out any old log records that might be in the
	 * temp database.
	 */
	if ((ret = __db_truncate(db_rep->rep_db, NULL, &count)) != 0)
		goto err_nolock;

	/*
	 * We will remove all logs we have so we need to request
	 * from the master's beginning.
	 */
	REP_SYSTEM_LOCK(dbenv);
	rep->first_lsn = rup->first_lsn;
	rep->first_vers = rup->first_vers;
	rep->last_lsn = rp->lsn;
	/*
	 * We need to take the larger of the LSNs for the sync_lsn
	 * for the STARTUPDONE event.  We don't want to trigger
	 * STARTUPDONE in the middle of internal init because we
	 * aren't done starting up.
	 */
	if (LOG_COMPARE(&rp->lsn, &rep->sync_lsn) > 0)
		rep->sync_lsn = rp->lsn;
	rep->nfiles = rup->num_files;
	rep->curfile = 0;
	rep->ready_pg = 0;
	rep->npages = 0;
	rep->waiting_pg = PGNO_INVALID;
	rep->max_wait_pg = PGNO_INVALID;

	__os_free(dbenv, rup);

	RPRINT(dbenv, (dbenv,
	    "Update setup for %d files.", rep->nfiles));
	RPRINT(dbenv, (dbenv, "Update setup:  First LSN [%lu][%lu].",
	    (u_long)rep->first_lsn.file, (u_long)rep->first_lsn.offset));
	RPRINT(dbenv, (dbenv, "Update setup:  Last LSN [%lu][%lu]",
	    (u_long)rep->last_lsn.file, (u_long)rep->last_lsn.offset));

	infolen = rec->size - sizeof(__rep_update_args);
	if ((ret = __os_calloc(dbenv, 1, infolen, &rep->originfo)) != 0)
		goto err;
	memcpy(rep->originfo, next, infolen);
	rep->finfo = rep->originfo;
	if ((ret = __rep_fileinfo_read(dbenv,
	    rep->finfo, &next, &rep->curinfo)) != 0) {
		RPRINT(dbenv, (dbenv,
		    "Update setup: Fileinfo read: %s", db_strerror(ret)));
		goto errmem1;
	}
	rep->nextinfo = next;

#ifdef DIAGNOSTIC
	{
	__rep_fileinfo_args *msgfp;
	msgfp = rep->curinfo;
	DB_ASSERT(dbenv, msgfp->pgno == 0);
	}
#endif

	/*
	 * We need to remove all logs and databases the client has prior to
	 * getting pages for current databases on the master.
	 */
	if ((ret = __rep_remove_all(dbenv, rec)) != 0)
		goto errmem;

	/*
	 * We want to create/open our dbp to the database
	 * where we'll keep our page information.
	 */
	if ((ret = __rep_client_dbinit(dbenv, 1, REP_PG)) != 0) {
		RPRINT(dbenv, (dbenv,
		    "Update setup: Client_dbinit %s", db_strerror(ret)));
		goto errmem;
	}

	/*
	 * We should get file info 'ready to go' to avoid data copies.
	 */
	memset(&pagereq_dbt, 0, sizeof(pagereq_dbt));
	pagereq_dbt.data = rep->finfo;
	pagereq_dbt.size =
	    (u_int32_t)((u_int8_t *)rep->nextinfo - (u_int8_t *)rep->finfo);

	RPRINT(dbenv, (dbenv,
	    "Update PAGE_REQ file 0: pgsize %lu, maxpg %lu",
	    (u_long)rep->curinfo->pgsize,
	    (u_long)rep->curinfo->max_pgno));
	/*
	 * We set up pagereq_dbt as we went along.  Send it now.
	 */
	(void)__rep_send_message(dbenv, eid, REP_PAGE_REQ,
	    NULL, &pagereq_dbt, 0, DB_REP_ANYWHERE);
	if (0) {
errmem:		__os_free(dbenv, rep->curinfo);
errmem1:	__os_free(dbenv, rep->originfo);
		rep->finfo = NULL;
		rep->curinfo = NULL;
		rep->originfo = NULL;
	}

	if (0) {
err_nolock:	REP_SYSTEM_LOCK(dbenv);
	}

err:	/*
	 * If we get an error, we cannot leave ourselves in the RECOVER_PAGE
	 * state because we have no file information.  That also means undo'ing
	 * the rep_lockout.  We need to move back to the RECOVER_UPDATE stage.
	 */
	if (ret != 0) {
		RPRINT(dbenv, (dbenv,
		    "Update_setup: Error: Clear PAGE, set UPDATE again. %s",
		    db_strerror(ret)));
		F_CLR(rep, REP_F_RECOVER_PAGE | REP_F_READY_API |
		    REP_F_READY_OP);
		F_SET(rep, REP_F_RECOVER_UPDATE);
	}
	REP_SYSTEM_UNLOCK(dbenv);
	return (ret);
}

/*
 * Removes all existing logs and databases, at the start of internal init.  But
 * before we do, write a list of the databases onto the init file, so that in
 * case we crash in the middle, we'll know how to resume when we restart.
 * Finally, also write into the init file the UPDATE message from the master (in
 * the "rec" DBT), which includes the (new) list of databases we intend to
 * request copies of (again, so that we know what to do if we crash in the
 * middle).
 *
 * For the sake of simplicity, these database lists are in the form of an UPDATE
 * message (since we already have the mechanisms in place), even though strictly
 * speaking that contains more information than we really need to store.
 */
static int
__rep_remove_all(dbenv, rec)
	DB_ENV *dbenv;
	DBT *rec;
{
	__rep_fileinfo_args *finfo;
	DB_FH *fhp;
	DB_LSN unused;
	size_t cnt, filelen, filesz, updlen;
	u_int32_t bufsz, filecnt;
	char *fname;
	int ret, t_ret;
	u_int8_t *buf, *fp, *origfp;

	ZERO_LSN(unused);
	finfo = NULL;
	fname = NULL;
	fhp = NULL;

	/*
	 * 1. Get list of databases currently present at this client, which we
	 *    intend to remove.
	 */
	filelen = 0;
	filecnt = 0;
	filesz = MEGABYTE;
	if ((ret = __os_calloc(dbenv, 1, filesz, &buf)) != 0)
		return (ret);
	origfp = fp = buf + sizeof(__rep_update_args);
	if ((ret = __rep_find_dbs(
	    dbenv, &fp, &filesz, &filelen, &filecnt)) != 0)
		goto out;
	if ((ret = __rep_update_buf(buf, filesz, &updlen,
	    &unused, 0, filecnt)) != 0)
		goto out;

	/*
	 * 2. Before removing anything, safe-store the database list, so that in
	 *    case we crash before we've removed them all, when we restart we
	 *    can clean up what we were doing.
	 */
	if ((ret = __db_appname(
	    dbenv, DB_APP_NONE, REP_INITNAME, 0, NULL, &fname)) != 0)
		goto out;
	bufsz = updlen + filelen;
	/* (Short writes aren't possible, so we don't have to verify 'cnt'.) */
	if ((ret = __os_open(dbenv, fname, 0,
	    DB_OSO_CREATE | DB_OSO_TRUNC, __db_omode(OWNER_RW), &fhp)) != 0 ||
	    (ret = __os_write(dbenv, fhp, &bufsz, sizeof(bufsz), &cnt)) != 0 ||
	    (ret = __os_write(dbenv, fhp, buf, bufsz, &cnt)) != 0 ||
	    (ret = __os_fsync(dbenv, fhp)) != 0) {
		__db_err(dbenv, ret, "%s", fname);
		goto out;
	}

	/*
	 * 3. Go ahead and remove logs and databases.  The databases get removed
	 *    according to the list we just finished safe-storing.
	 */
	if ((ret = __rep_remove_logs(dbenv)) != 0)
		goto out;
	if ((ret = __rep_closefiles(dbenv)) != 0)
		goto out;
	fp = origfp;
	while (filecnt-- > 0) {
		if ((ret =__rep_fileinfo_read(dbenv,
		    fp, (void*)&fp, &finfo)) != 0)
			goto out;
		if ((ret = __rep_remove_file(dbenv,
		    finfo->uid.data, finfo->info.data, finfo->type)) != 0)
			goto out;
		__os_free(dbenv, finfo);
		finfo = NULL;
	}

	/*
	 * 4. Safe-store the (new) list of database files we intend to copy from
	 *    the master (again, so that in case we crash before we're finished
	 *    doing so, we'll have enough information to clean up and start over
	 *    again).
	 */
	if ((ret = __os_write(dbenv, fhp,
	    &rec->size, sizeof(rec->size), &cnt)) != 0 ||
	    (ret = __os_write(dbenv, fhp, rec->data, rec->size, &cnt)) != 0 ||
	    (ret = __os_fsync(dbenv, fhp)) != 0) {
		__db_err(dbenv, ret, "%s", fname);
		goto out;
	}

out:
	if (fhp != NULL && (t_ret = __os_closehandle(dbenv, fhp)) && ret == 0)
		ret = t_ret;
	if (fname != NULL)
		__os_free(dbenv, fname);
	if (finfo != NULL)
		__os_free(dbenv, finfo);
	__os_free(dbenv, buf);
	return (ret);
}



/*
 * __rep_remove_logs -
 *	Remove our logs to prepare for internal init.
 */
static int
__rep_remove_logs(dbenv)
	DB_ENV *dbenv;
{
	DB_LOG *dblp;
	DB_LSN lsn;
	LOG *lp;
	u_int32_t fnum, lastfile;
	int ret;
	char *name;

	dblp = dbenv->lg_handle;
	lp = dblp->reginfo.primary;
	ret = 0;

	/*
	 * Call memp_sync to flush out any logs that might
	 * be in the log buffers and not on disk before
	 * we remove files on disk.
	 */
	if ((ret = __memp_sync(dbenv, NULL)) != 0)
		return (ret);
	/*
	 * Forcibly remove existing log files or reset
	 * the in-memory log space.
	 */
	if (lp->db_log_inmemory) {
		INIT_LSN(lsn);
		if ((ret = __log_zero(dbenv, &lsn)) != 0)
			return (ret);
	} else {
		lastfile = lp->lsn.file;
		for (fnum = 1; fnum <= lastfile; fnum++) {
			if ((ret = __log_name(dblp, fnum, &name, NULL, 0)) != 0)
				return (ret);
			(void)time(&lp->timestamp);
			(void)__os_unlink(dbenv, name);
			__os_free(dbenv, name);
		}
	}
	return (0);
}

/*
 * Removes a file during internal init.  Assumes underlying subsystems are
 * active; therefore, this can't be used for internal init crash recovery.
 */
static int
__rep_remove_file(dbenv, uid, name, type)
	DB_ENV *dbenv;
	u_int8_t *uid;
	const char *name;
	u_int32_t type;
{
	DB *dbp;
	char *real_name;
	int ret;

	real_name = NULL;

	/*
	 * Calling __fop_remove will both purge any matching
	 * fileid from mpool and unlink it on disk.
	 */
#ifdef HAVE_QUEUE
	/*
	 * Handle queue separately.  __fop_remove will not
	 * remove extent files.  Use __qam_remove to remove
	 * extent files that might exist under this name.
	 */
	if (type == (u_int32_t)DB_QUEUE) {
		if ((ret = __db_create_internal(&dbp, dbenv, 0)) != 0)
			goto out;

		if ((ret = __db_appname(dbenv,
		    DB_APP_DATA, name, 0, NULL, &real_name)) != 0)
			return (ret);
		
		if ((ret = __fop_remove_setup(dbp, NULL, real_name, 0)) != 0)
			goto out;
		
		RPRINT(dbenv, (dbenv, "QAM: Unlink %s via __qam_remove", name));
		if ((ret = __qam_remove(dbp, NULL, name, NULL)) != 0) {
			RPRINT(dbenv, (dbenv, "qam_remove returned %d", ret));
			(void)__db_close(dbp, NULL, DB_NOSYNC);
			goto out;
		}
		if ((ret = __db_close(dbp, NULL, DB_NOSYNC)) != 0)
			goto out;
	}
#endif
	/*
	 * We call fop_remove even if we've called qam_remove.
	 * That will only have removed extent files.  Now
	 * we need to deal with the actual file itself.
	 */
	ret = __fop_remove(dbenv, NULL, uid, name, DB_APP_DATA, 0);

out:
	if (real_name != NULL)
		__os_free(dbenv, real_name);
	return (ret);
}	

/*
 * __rep_bulk_page
 *	Process a bulk page message.
 *
 * PUBLIC: int __rep_bulk_page __P((DB_ENV *, int, REP_CONTROL *, DBT *));
 */
int
__rep_bulk_page(dbenv, eid, rp, rec)
	DB_ENV *dbenv;
	int eid;
	REP_CONTROL *rp;
	DBT *rec;
{
	DBT pgrec;
	REP_CONTROL tmprp;
	u_int32_t len;
	int ret;
	u_int8_t *p, *ep;

	memset(&pgrec, 0, sizeof(pgrec));
	/*
	 * We're going to be modifying the rp LSN contents so make
	 * our own private copy to play with.  We need to set the
	 * rectype to REP_PAGE because we're calling through __rep_page
	 * to process each page, and lower functions make decisions
	 * based on the rectypes (for throttling/gap processing)
	 */
	memcpy(&tmprp, rp, sizeof(tmprp));
	tmprp.rectype = REP_PAGE;
	ret = 0;
	for (ep = (u_int8_t *)rec->data + rec->size, p = (u_int8_t *)rec->data;
	    p < ep; p += len) {
		/*
		 * First thing in the buffer is the length.  Then the LSN
		 * of this page, then the page info itself.
		 */
		memcpy(&len, p, sizeof(len));
		p += sizeof(len);
		memcpy(&tmprp.lsn, p, sizeof(DB_LSN));
		p += sizeof(DB_LSN);
		pgrec.data = p;
		pgrec.size = len;
		RPRINT(dbenv, (dbenv,
		    "rep_bulk_page: Processing LSN [%lu][%lu]",
		    (u_long)tmprp.lsn.file, (u_long)tmprp.lsn.offset));
		RPRINT(dbenv, (dbenv,
    "rep_bulk_page: p %#lx ep %#lx pgrec data %#lx, size %lu (%#lx)",
		    P_TO_ULONG(p), P_TO_ULONG(ep), P_TO_ULONG(pgrec.data),
		    (u_long)pgrec.size, (u_long)pgrec.size));
		/*
		 * Now send the page info DBT to the page processing function.
		 */
		ret = __rep_page(dbenv, eid, &tmprp, &pgrec);
		RPRINT(dbenv, (dbenv,
		    "rep_bulk_page: rep_page ret %d", ret));

		/*
		 * If this set of pages is already done just return.
		 */
		if (ret != 0) {
			if (ret == DB_REP_PAGEDONE)
				ret = 0;
			break;
		}
	}
	return (ret);
}

/*
 * __rep_page
 *	Process a page message.
 *
 * PUBLIC: int __rep_page __P((DB_ENV *, int, REP_CONTROL *, DBT *));
 */
int
__rep_page(dbenv, eid, rp, rec)
	DB_ENV *dbenv;
	int eid;
	REP_CONTROL *rp;
	DBT *rec;
{

	DB_REP *db_rep;
	DBT key, data;
	REP *rep;
	__rep_fileinfo_args *msgfp;
	db_recno_t recno;
	int ret;
	void *next;

	ret = 0;
	db_rep = dbenv->rep_handle;
	rep = db_rep->region;

	if (!F_ISSET(rep, REP_F_RECOVER_PAGE))
		return (DB_REP_PAGEDONE);
	if ((ret = __rep_fileinfo_read(dbenv, rec->data, &next, &msgfp)) != 0)
		return (ret);
	MUTEX_LOCK(dbenv, rep->mtx_clientdb);
	REP_SYSTEM_LOCK(dbenv);
	RPRINT(dbenv, (dbenv,
	    "PAGE: Received page %lu from file %d",
	    (u_long)msgfp->pgno, msgfp->filenum));
	/*
	 * Check if this page is from the file we're expecting.
	 * This may be an old or delayed page message.
	 */
	/*
	 * !!!
	 * If we allow dbrename/dbremove on the master while a client
	 * is updating, then we'd have to verify the file's uid here too.
	 */
	if (msgfp->filenum != rep->curfile) {
		RPRINT(dbenv, (dbenv, "Msg file %d != curfile %d",
		    msgfp->filenum, rep->curfile));
		ret = DB_REP_PAGEDONE;
		goto err;
	}
	/*
	 * We want to create/open our dbp to the database
	 * where we'll keep our page information.
	 */
	if ((ret = __rep_client_dbinit(dbenv, 1, REP_PG)) != 0)
		goto err;

	memset(&key, 0, sizeof(key));
	memset(&data, 0, sizeof(data));
	recno = (db_recno_t)(msgfp->pgno + 1);
	key.data = &recno;
	key.ulen = key.size = sizeof(db_recno_t);
	key.flags = DB_DBT_USERMEM;

	/*
	 * If we already have this page, then we don't want to bother
	 * rewriting it into the file.  Otherwise, any other error
	 * we want to return.
	 */
	ret = __db_put(rep->file_dbp, NULL, &key, &data, DB_NOOVERWRITE);
	if (ret == DB_KEYEXIST) {
		RPRINT(dbenv, (dbenv,
		    "PAGE: Received duplicate page %lu from file %d",
		    (u_long)msgfp->pgno, msgfp->filenum));
		rep->stat.st_pg_duplicated++;
		ret = 0;
		goto err;
	}
	if (ret != 0)
		goto err;

	RPRINT(dbenv, (dbenv,
	    "PAGE: Write page %lu into mpool", (u_long)msgfp->pgno));
	/*
	 * We put the page in the database file itself.
	 */
	ret = __rep_write_page(dbenv, rep, msgfp);
	if (ret != 0) {
		/*
		 * We got an error storing the page, therefore, we need
		 * remove this page marker from the page database too.
		 * !!!
		 * I'm ignoring errors from the delete because we want to
		 * return the original error.  If we cannot write the page
		 * and we cannot delete the item we just put, what should
		 * we do?  Panic the env and return DB_RUNRECOVERY?
		 */
		(void)__db_del(rep->file_dbp, NULL, &key, 0);
		goto err;
	}
	rep->stat.st_pg_records++;
	rep->npages++;

	/*
	 * Now check the LSN on the page and save it if it is later
	 * than the one we have.
	 *
	 * We need to take the larger of the LSNs for the sync_lsn
	 * for the STARTUPDONE event.  We don't want to trigger
	 * STARTUPDONE in the middle of internal init because we
	 * aren't done starting up.
	 */
	if (LOG_COMPARE(&rp->lsn, &rep->last_lsn) > 0) {
		rep->last_lsn = rp->lsn;
		if (LOG_COMPARE(&rep->last_lsn, &rep->sync_lsn) > 0)
			rep->sync_lsn = rep->last_lsn;
	}

	/*
	 * We've successfully written the page.  Now we need to see if
	 * we're done with this file.  __rep_filedone will check if we
	 * have all the pages expected and if so, set up for the next
	 * file and send out a page request for the next file's pages.
	 */
	ret = __rep_filedone(dbenv, eid, rep, msgfp, rp->rectype);

err:	REP_SYSTEM_UNLOCK(dbenv);
	MUTEX_UNLOCK(dbenv, rep->mtx_clientdb);

	__os_free(dbenv, msgfp);
	return (ret);
}

/*
 * __rep_page_fail
 *	Process a page fail message.
 *
 * PUBLIC: int __rep_page_fail __P((DB_ENV *, int, DBT *));
 */
int
__rep_page_fail(dbenv, eid, rec)
	DB_ENV *dbenv;
	int eid;
	DBT *rec;
{

	DB_REP *db_rep;
	REP *rep;
	__rep_fileinfo_args *msgfp, *rfp;
	int ret;
	void *next;

	ret = 0;
	db_rep = dbenv->rep_handle;
	rep = db_rep->region;

	if (!F_ISSET(rep, REP_F_RECOVER_PAGE))
		return (0);
	if ((ret = __rep_fileinfo_read(dbenv, rec->data, &next, &msgfp)) != 0)
		return (ret);
	/*
	 * Check if this page is from the file we're expecting.
	 * This may be an old or delayed page message.
	 */
	/*
	 * !!!
	 * If we allow dbrename/dbremove on the master while a client
	 * is updating, then we'd have to verify the file's uid here too.
	 */
	MUTEX_LOCK(dbenv, rep->mtx_clientdb);
	REP_SYSTEM_LOCK(dbenv);
	if (msgfp->filenum != rep->curfile) {
		RPRINT(dbenv, (dbenv, "Msg file %d != curfile %d",
		    msgfp->filenum, rep->curfile));
		goto out;
	}
	rfp = rep->curinfo;
	if (rfp->type != (u_int32_t)DB_QUEUE)
		--rfp->max_pgno;
	else {
		/*
		 * Queue is special.  Pages at the beginning of the queue
		 * may disappear, as well as at the end.  Use msgfp->pgno
		 * to adjust accordingly.
		 */
		RPRINT(dbenv, (dbenv,
	    "page_fail: BEFORE page %lu failed. ready %lu, max %lu, npages %d",
		    (u_long)msgfp->pgno, (u_long)rep->ready_pg,
		    (u_long)rfp->max_pgno, rep->npages));
		if (msgfp->pgno == rfp->max_pgno)
			--rfp->max_pgno;
		if (msgfp->pgno >= rep->ready_pg) {
			rep->ready_pg = msgfp->pgno + 1;
			rep->npages = rep->ready_pg;
		}
		RPRINT(dbenv, (dbenv,
	    "page_fail: AFTER page %lu failed. ready %lu, max %lu, npages %d",
		    (u_long)msgfp->pgno, (u_long)rep->ready_pg,
		    (u_long)rfp->max_pgno, rep->npages));
	}

	/*
	 * We've lowered the number of pages expected.  It is possible that
	 * this was the last page we were expecting.  Now we need to see if
	 * we're done with this file.  __rep_filedone will check if we have
	 * all the pages expected and if so, set up for the next file and
	 * send out a page request for the next file's pages.
	 */
	ret = __rep_filedone(dbenv, eid, rep, msgfp, REP_PAGE_FAIL);
out:
	REP_SYSTEM_UNLOCK(dbenv);
	MUTEX_UNLOCK(dbenv, rep->mtx_clientdb);
	__os_free(dbenv, msgfp);
	return (ret);
}

/*
 * __rep_write_page -
 *	Write this page into a database.
 */
static int
__rep_write_page(dbenv, rep, msgfp)
	DB_ENV *dbenv;
	REP *rep;
	__rep_fileinfo_args *msgfp;
{
	__rep_fileinfo_args *rfp;
	int ret;
	void *dst;

	rfp = NULL;

	/*
	 * If this is the first page we're putting in this database, we need
	 * to create the mpool file.  Otherwise call memp_fget to create the
	 * page in mpool.  Then copy the data to the page, and memp_fput the
	 * page to give it back to mpool.
	 *
	 * We need to create the file, removing any existing file and associate
	 * the correct file ID with the new one.
	 */
	rfp = rep->curinfo;
	if (rep->file_mpf == NULL) {
		if (!F_ISSET(rfp, DB_AM_INMEM)) {
			/*
			 * Recreate the file on disk.  We'll be putting
			 * the data into the file via mpool.
			 */
			RPRINT(dbenv, (dbenv,
			    "rep_write_page: Calling fop_create for %s",
			    (char *)rfp->info.data));
			if ((ret = __fop_create(dbenv, NULL, NULL,
			    rfp->info.data, DB_APP_DATA,
			    dbenv->db_mode, 0)) != 0)
				goto err;
		}

		if ((ret =
		    __rep_mpf_open(dbenv, &rep->file_mpf, rep->curinfo,
		    F_ISSET(rfp, DB_AM_INMEM) ? DB_CREATE : 0)) != 0)
			goto err;
	}
	/*
	 * Handle queue specially.  If we're a QUEUE database, we need to
	 * use the __qam_fget/put calls.  We need to use rep->queue_dbp for
	 * that.  That dbp is opened after getting the metapage for the
	 * queue database.  Since the meta-page is always in the queue file,
	 * we'll use the normal path for that first page.  After that we
	 * can assume the dbp is opened.
	 */
	if (msgfp->type == (u_int32_t)DB_QUEUE && msgfp->pgno != 0) {
#ifdef HAVE_QUEUE
		ret = __qam_fget(rep->queue_dbp, &msgfp->pgno, NULL,
		    DB_MPOOL_CREATE | DB_MPOOL_DIRTY, &dst);
#else
		/*
		 * This always returns an error.
		 */
		ret = __db_no_queue_am(dbenv);
#endif
	} else
		ret = __memp_fget(rep->file_mpf, &msgfp->pgno, NULL,
		    DB_MPOOL_CREATE | DB_MPOOL_DIRTY, &dst);

	if (ret != 0)
		goto err;

	memcpy(dst, msgfp->info.data, msgfp->pgsize);
#ifdef HAVE_QUEUE
	if (msgfp->type == (u_int32_t)DB_QUEUE && msgfp->pgno != 0)
		ret = __qam_fput(rep->queue_dbp,
		     msgfp->pgno, dst, rep->queue_dbp->priority);
	else
#endif
		ret = __memp_fput(rep->file_mpf, dst, rep->file_dbp->priority);

err:	return (ret);
}

/*
 * __rep_page_gap -
 *	After we've put the page into the database, we need to check if
 *	we have a page gap and whether we need to request pages.
 */
static int
__rep_page_gap(dbenv, rep, msgfp, type)
	DB_ENV *dbenv;
	REP *rep;
	__rep_fileinfo_args *msgfp;
	u_int32_t type;
{
	DB_LOG *dblp;
	DBC *dbc;
	DBT data, key;
	LOG *lp;
	__rep_fileinfo_args *rfp;
	db_recno_t recno;
	int ret, t_ret;

	dblp = dbenv->lg_handle;
	lp = dblp->reginfo.primary;
	ret = 0;
	dbc = NULL;

	/*
	 * We've successfully put this page into our file.
	 * Now we need to account for it and re-request new pages
	 * if necessary.
	 */
	/*
	 * We already hold both the db mutex and rep mutex.
	 */
	rfp = rep->curinfo;

	/*
	 * Make sure we're still talking about the same file.
	 * If not, we're done here.
	 */
	if (rfp->filenum != msgfp->filenum) {
		ret = DB_REP_PAGEDONE;
		goto err;
	}

	/*
	 * We have 3 possible states:
	 * 1.  We receive a page we already have accounted for.
	 *	msg pgno < ready pgno
	 * 2.  We receive a page that is beyond a gap.
	 *	msg pgno > ready pgno
	 * 3.  We receive the page we're expecting next.
	 *	msg pgno == ready pgno
	 */
	/*
	 * State 1.  This can happen once we put our page record into the
	 * database, but by the time we acquire the mutex other
	 * threads have already accounted for this page and moved on.
	 * We just want to return.
	 */
	if (msgfp->pgno < rep->ready_pg) {
		RPRINT(dbenv, (dbenv,
		    "PAGE_GAP: pgno %lu < ready %lu, waiting %lu",
		    (u_long)msgfp->pgno, (u_long)rep->ready_pg,
		    (u_long)rep->waiting_pg));
		goto err;
	}

	/*
	 * State 2.  This page is beyond the page we're expecting.
	 * We need to update waiting_pg if this page is less than
	 * (earlier) the current waiting_pg.  There is nothing
	 * to do but see if we need to request.
	 */
	RPRINT(dbenv, (dbenv,
    "PAGE_GAP: pgno %lu, max_pg %lu ready %lu, waiting %lu max_wait %lu",
	    (u_long)msgfp->pgno, (u_long)rfp->max_pgno, (u_long)rep->ready_pg,
	    (u_long)rep->waiting_pg, (u_long)rep->max_wait_pg));
	if (msgfp->pgno > rep->ready_pg) {
		if (rep->waiting_pg == PGNO_INVALID ||
		    msgfp->pgno < rep->waiting_pg)
			rep->waiting_pg = msgfp->pgno;
	} else {
		/*
		 * We received the page we're expecting.
		 */
		rep->ready_pg++;
		lp->rcvd_recs = 0;
		if (rep->ready_pg == rep->waiting_pg) {
			/*
			 * If we get here we know we just filled a gap.
			 * Move the cursor to that place and then walk
			 * forward looking for the next gap, if it exists.
			 */
			lp->wait_recs = 0;
			lp->rcvd_recs = 0;
			rep->max_wait_pg = PGNO_INVALID;
			/*
			 * We need to walk the recno database looking for the
			 * next page we need or expect.
			 */
			memset(&key, 0, sizeof(key));
			memset(&data, 0, sizeof(data));
			if ((ret = __db_cursor(rep->file_dbp, NULL,
			    &dbc, 0)) != 0)
				goto err;
			/*
			 * Set cursor to the first waiting page.
			 * Page numbers/record numbers are offset by 1.
			 */
			recno = (db_recno_t)rep->waiting_pg + 1;
			key.data = &recno;
			key.ulen = key.size = sizeof(db_recno_t);
			key.flags = DB_DBT_USERMEM;
			/*
			 * We know that page is there, this should
			 * find the record.
			 */
			ret = __dbc_get(dbc, &key, &data, DB_SET);
			if (ret != 0)
				goto err;
			RPRINT(dbenv, (dbenv,
			    "PAGE_GAP: Set cursor for ready %lu, waiting %lu",
			    (u_long)rep->ready_pg, (u_long)rep->waiting_pg));
		}
		while (ret == 0 && rep->ready_pg == rep->waiting_pg) {
			rep->ready_pg++;
			ret = __dbc_get(dbc, &key, &data, DB_NEXT);
			/*
			 * If we get to the end of the list, there are no
			 * more gaps.  Reset waiting_pg.
			 */
			if (ret == DB_NOTFOUND || ret == DB_KEYEMPTY) {
				rep->waiting_pg = PGNO_INVALID;
				RPRINT(dbenv, (dbenv,
	    "PAGE_GAP: Next cursor No next - ready %lu, waiting %lu",
				    (u_long)rep->ready_pg,
				    (u_long)rep->waiting_pg));
				break;
			}
			/*
			 * Subtract 1 from waiting_pg because record numbers
			 * are 1-based and pages are 0-based and we added 1
			 * into the page number when we put it into the db.
			 */
			rep->waiting_pg = *(db_pgno_t *)key.data;
			rep->waiting_pg--;
			RPRINT(dbenv, (dbenv,
	    "PAGE_GAP: Next cursor ready %lu, waiting %lu",
			    (u_long)rep->ready_pg, (u_long)rep->waiting_pg));
		}
	}

	/*
	 * If we filled a gap and now have the entire file, there's
	 * nothing to do.  We're done when ready_pg is > max_pgno
	 * because ready_pg is larger than the last page we received.
	 */
	if (rep->ready_pg > rfp->max_pgno)
		goto err;

	/*
	 * Check if we need to ask for more pages.
	 */
	if ((rep->waiting_pg != PGNO_INVALID &&
	    rep->ready_pg != rep->waiting_pg) || type == REP_PAGE_MORE) {
		/*
		 * We got a page but we may still be waiting for more.
		 */
		if (lp->wait_recs == 0) {
			/*
			 * This is a new gap. Initialize the number of
			 * records that we should wait before requesting
			 * that it be resent.  We grab the limits out of
			 * the rep without the mutex.
			 */
			lp->wait_recs = rep->request_gap;
			lp->rcvd_recs = 0;
			rep->max_wait_pg = PGNO_INVALID;
		}
		/*
		 * If we got REP_PAGE_MORE we always want to ask for more.
		 * We need to set rfp->pgno to the current page number
		 * we will use to ask for more pages.
		 */
		if (type == REP_PAGE_MORE)
			rfp->pgno = msgfp->pgno;
		if ((__rep_check_doreq(dbenv, rep) || type == REP_PAGE_MORE) &&
		    ((ret = __rep_pggap_req(dbenv, rep, rfp,
		    (type == REP_PAGE_MORE) ? REP_GAP_FORCE : 0)) != 0))
			goto err;
	} else {
		lp->wait_recs = 0;
		rep->max_wait_pg = PGNO_INVALID;
	}

err:
	if (dbc != NULL && (t_ret = __dbc_close(dbc)) != 0 && ret == 0)
		ret = t_ret;

	return (ret);
}

/*
 * __rep_init_cleanup -
 *	Clean up internal initialization pieces.
 *
 * PUBLIC: int __rep_init_cleanup __P((DB_ENV *, REP *, int));
 */
int
__rep_init_cleanup(dbenv, rep, force)
	DB_ENV *dbenv;
	REP *rep;
	int force;
{
	DB_LOG *dblp;
	LOG *lp;
	int cleanup_failure, ret, t_ret;

	ret = 0;
	/*
	 * 1.  Close up the file data pointer we used.
	 * 2.  Close/reset the page database.
	 * 3.  Close/reset the queue database if we're forcing a cleanup.
	 * 4.  Free current file info.
	 * 5.  If we have all files or need to force, free original file info.
	 */
	if (rep->file_mpf != NULL) {
		ret = __memp_fclose(rep->file_mpf, 0);
		rep->file_mpf = NULL;
	}
	if (rep->file_dbp != NULL) {
		t_ret = __db_close(rep->file_dbp, NULL, DB_NOSYNC);
		rep->file_dbp = NULL;
		if (t_ret != 0 && ret == 0)
			ret = t_ret;
	}
	if (force && rep->queue_dbp != NULL) {
		t_ret = __db_close(rep->queue_dbp, NULL, DB_NOSYNC);
		rep->queue_dbp = NULL;
		if (t_ret != 0 && ret == 0)
			ret = t_ret;
	}
	if (rep->curinfo != NULL) {
		__os_free(dbenv, rep->curinfo);
		rep->curinfo = NULL;
	}
	if (rep->originfo != NULL && force) {
		/*
		 * Clean up files involved in an interrupted internal init.
		 *
		 * 1. logs
		 *   a) remove old log files
		 *   b) set up initial log file #1
		 * 2. database files
		 * 3. the "init file"
		 *
		 * Steps 1 and 2 can be attempted independently.  Step 1b is
		 * dependent on successful completion of 1a.  Step 3 must not be
		 * done if anything fails along the way, because the init file's
		 * raison d'etre is to show that some files remain to be cleaned
		 * up.
		 */
		RPRINT(dbenv, (dbenv, "clean up interrupted internal init"));
		cleanup_failure = 0;

		if ((t_ret = __rep_remove_logs(dbenv)) == 0) {
			/*
			 * Since we have no logs, recover by making it look like
			 * the case when a new client first starts up, namely we
			 * have nothing but a fresh log file #1.  This is a
			 * little wasteful, since we may soon remove this log
			 * file again.  But that's OK, because this is the
			 * unusual case of NEWMASTER during internal init, and
			 * the rest of internal init doubtless dwarfs this.
			 */
			dblp = dbenv->lg_handle;
			lp = dblp->reginfo.primary;
			
			if ((t_ret = __rep_log_setup(dbenv,
			    rep, 1, DB_LOGVERSION, &lp->ready_lsn)) != 0) {
				cleanup_failure = 1;
				if (ret == 0)
					ret = t_ret;
			}
		} else {
			cleanup_failure = 1;
			if (ret == 0)
				ret = t_ret;
		}

		if ((t_ret = __rep_remove_by_list(dbenv,
		    rep->originfo, rep->nfiles)) != 0) {
			cleanup_failure = 1;
			if (ret == 0)
				ret = t_ret;
		}

		if (!cleanup_failure &&
		    (t_ret = __rep_remove_init_file(dbenv)) != 0) {
			if (ret == 0)
				ret = t_ret;
		}

		__os_free(dbenv, rep->originfo);
		rep->originfo = NULL;
	}

	return (ret);
}

/*
 * __rep_filedone -
 *	We need to check if we're done with the current file after
 *	processing the current page.  Stat the database to see if
 *	we have all the pages.  If so, we need to clean up/close
 *	this one, set up for the next one, and ask for its pages,
 *	or if this is the last file, request the log records and
 *	move to the REP_RECOVER_LOG state.
 */
static int
__rep_filedone(dbenv, eid, rep, msgfp, type)
	DB_ENV *dbenv;
	int eid;
	REP *rep;
	__rep_fileinfo_args *msgfp;
	u_int32_t type;
{
	DBT dbt;
	__rep_fileinfo_args *rfp;
	int ret;

	/*
	 * We've put our page, now we need to do any gap processing
	 * that might be needed to re-request pages.
	 */
	ret = __rep_page_gap(dbenv, rep, msgfp, type);
	/*
	 * The world changed while we were doing gap processing.
	 * We're done here.
	 */
	if (ret == DB_REP_PAGEDONE)
		return (0);

	rfp = rep->curinfo;
	/*
	 * max_pgno is 0-based and npages is 1-based, so we don't have
	 * all the pages until npages is > max_pgno.
	 */
	RPRINT(dbenv, (dbenv, "FILEDONE: have %lu pages. Need %lu.",
	    (u_long)rep->npages, (u_long)rfp->max_pgno + 1));
	if (rep->npages <= rfp->max_pgno)
		return (0);

	/*
	 * If we're queue and we think we have all the pages for this file,
	 * we need to do special queue processing.  Queue is handled in
	 * several stages.
	 */
	if (rfp->type == (u_int32_t)DB_QUEUE &&
	    ((ret = __rep_queue_filedone(dbenv, rep, rfp)) !=
	    DB_REP_PAGEDONE))
		return (ret);
	/*
	 * We have all the pages for this file.  Clean up.
	 */
	if ((ret = __rep_init_cleanup(dbenv, rep, 0)) != 0)
		goto err;
	if (++rep->curfile == rep->nfiles) {
		RPRINT(dbenv, (dbenv,
		    "FILEDONE: have %d files.  RECOVER_LOG now", rep->nfiles));
		/*
		 * Move to REP_RECOVER_LOG state.
		 * Request logs.
		 */
		/*
		 * We need to do a sync here so that any later opens
		 * can find the file and file id.  We need to do it
		 * before we clear REP_F_RECOVER_PAGE so that we do not
		 * try to flush the log.
		 */
		if ((ret = __memp_sync(dbenv, NULL)) != 0)
			goto err;
		F_CLR(rep, REP_F_RECOVER_PAGE);
		F_SET(rep, REP_F_RECOVER_LOG);
		memset(&dbt, 0, sizeof(dbt));
		dbt.data = &rep->last_lsn;
		dbt.size = sizeof(rep->last_lsn);
		REP_SYSTEM_UNLOCK(dbenv);
		if ((ret = __rep_log_setup(dbenv, rep,
		    rep->first_lsn.file, rep->first_vers, NULL)) != 0)
			goto err;
		RPRINT(dbenv, (dbenv,
		    "FILEDONE: LOG_REQ from LSN [%lu][%lu] to [%lu][%lu]",
		    (u_long)rep->first_lsn.file, (u_long)rep->first_lsn.offset,
		    (u_long)rep->last_lsn.file, (u_long)rep->last_lsn.offset));
		(void)__rep_send_message(dbenv, eid,
		    REP_LOG_REQ, &rep->first_lsn, &dbt, 
		    REPCTL_INIT, DB_REP_ANYWHERE);
		REP_SYSTEM_LOCK(dbenv);
		return (0);
	}

	/*
	 * 4.  If not, set curinfo to next file and request its pages.
	 */
	rep->finfo = rep->nextinfo;
	if ((ret = __rep_fileinfo_read(dbenv, rep->finfo, &rep->nextinfo,
	    &rep->curinfo)) != 0)
		goto err;
	DB_ASSERT(dbenv, rep->curinfo->pgno == 0);
	rep->ready_pg = 0;
	rep->npages = 0;
	rep->waiting_pg = PGNO_INVALID;
	rep->max_wait_pg = PGNO_INVALID;
	memset(&dbt, 0, sizeof(dbt));
	RPRINT(dbenv, (dbenv,
	    "FILEDONE: Next file %d.  Request pages 0 to %lu",
	    rep->curinfo->filenum, (u_long)rep->curinfo->max_pgno));
	dbt.data = rep->finfo;
	dbt.size =
	    (u_int32_t)((u_int8_t *)rep->nextinfo - (u_int8_t *)rep->finfo);
	(void)__rep_send_message(dbenv, eid, REP_PAGE_REQ,
	    NULL, &dbt, 0, DB_REP_ANYWHERE);
err:
	return (ret);
}

/*
 * __rep_mpf_open -
 *	Create and open the mpool file for a database.
 *	Used by both master and client to bring files into mpool.
 */
static int
__rep_mpf_open(dbenv, mpfp, rfp, flags)
	DB_ENV *dbenv;
	DB_MPOOLFILE **mpfp;
	__rep_fileinfo_args *rfp;
	u_int32_t flags;
{
	DB db;
	int ret;

	if ((ret = __memp_fcreate(dbenv, mpfp)) != 0)
		return (ret);

	/*
	 * We need a dbp to pass into to __db_env_mpool.  Set up
	 * only the parts that it needs.
	 */
	db.dbenv = dbenv;
	db.type = (DBTYPE)rfp->type;
	db.pgsize = rfp->pgsize;
	memcpy(db.fileid, rfp->uid.data, DB_FILE_ID_LEN);
	db.flags = rfp->flags;
	/* We need to make sure the dbp isn't marked open. */
	F_CLR(&db, DB_AM_OPEN_CALLED);
	db.mpf = *mpfp;
	if (F_ISSET(&db, DB_AM_INMEM))
		(void)__memp_set_flags(db.mpf, DB_MPOOL_NOFILE, 1);
	if ((ret = __db_env_mpool(&db, rfp->info.data, flags)) != 0) {
		(void)__memp_fclose(*mpfp, 0);
		*mpfp = NULL;
	}
	return (ret);
}

/*
 * __rep_pggap_req -
 *	Request a page gap.  Assumes the caller holds the rep_mutex.
 *
 * PUBLIC: int __rep_pggap_req __P((DB_ENV *, REP *, __rep_fileinfo_args *,
 * PUBLIC:    u_int32_t));
 */
int
__rep_pggap_req(dbenv, rep, reqfp, gapflags)
	DB_ENV *dbenv;
	REP *rep;
	__rep_fileinfo_args *reqfp;
	u_int32_t gapflags;
{
	DBT max_pg_dbt;
	__rep_fileinfo_args *tmpfp, t;
	size_t len;
	u_int32_t flags;
	int alloc, ret;

	ret = 0;
	alloc = 0;
	/*
	 * There is a window where we have to set REP_RECOVER_PAGE when
	 * we receive the update information to transition from getting
	 * file information to getting page information.  However, that
	 * thread does release and then reacquire mutexes.  So, we might
	 * try re-requesting before the original thread can get curinfo
	 * setup.  If curinfo isn't set up there is nothing to do.
	 */
	if (rep->curinfo == NULL)
		return (0);
	if (reqfp == NULL) {
		if ((ret = __rep_finfo_alloc(dbenv, rep->curinfo, &tmpfp)) != 0)
			return (ret);
		alloc = 1;
	} else {
		t = *reqfp;
		tmpfp = &t;
	}

	/*
	 * If we've never requested this page, then
	 * request everything between it and the first
	 * page we have.  If we have requested this page
	 * then only request this record, not the entire gap.
	 */
	flags = 0;
	memset(&max_pg_dbt, 0, sizeof(max_pg_dbt));
	/*
	 * If this is a PAGE_MORE and we're forcing then we want to
	 * force the request to ask for the next page after this one.
	 */
	if (FLD_ISSET(gapflags, REP_GAP_FORCE))
		tmpfp->pgno++;
	else
		tmpfp->pgno = rep->ready_pg;
	max_pg_dbt.data = rep->finfo;
	max_pg_dbt.size =
	    (u_int32_t)((u_int8_t *)rep->nextinfo - (u_int8_t *)rep->finfo);
	if (rep->max_wait_pg == PGNO_INVALID ||
	    FLD_ISSET(gapflags, REP_GAP_FORCE | REP_GAP_REREQUEST)) {
		/*
		 * Request the gap - set max to waiting_pg - 1 or if
		 * there is no waiting_pg, just ask for one.
		 */
		if (rep->waiting_pg == PGNO_INVALID) {
			if (FLD_ISSET(gapflags,
			    REP_GAP_FORCE | REP_GAP_REREQUEST))
				rep->max_wait_pg = rep->curinfo->max_pgno;
			else
				rep->max_wait_pg = rep->ready_pg;
		} else {
			/*
			 * If we're forcing, and waiting_pg is less than
			 * the page we want to start this request at, then
			 * we set max_wait_pg to the max pgno in the file.
			 */
			if (FLD_ISSET(gapflags, REP_GAP_FORCE) &&
			  rep->waiting_pg < tmpfp->pgno)
				rep->max_wait_pg = rep->curinfo->max_pgno;
			else
				rep->max_wait_pg = rep->waiting_pg - 1;
		}
		tmpfp->max_pgno = rep->max_wait_pg;
		/*
		 * Gap requests are "new" and can go anywhere.
		 */
		if (FLD_ISSET(gapflags, REP_GAP_REREQUEST))
			flags = DB_REP_REREQUEST;
		else
			flags = DB_REP_ANYWHERE;
	} else {
		/*
		 * Request 1 page - set max to ready_pg.
		 */
		rep->max_wait_pg = rep->ready_pg;
		tmpfp->max_pgno = rep->ready_pg;
		/*
		 * If we're dropping to singletons, this is a rerequest.
		 */
		flags = DB_REP_REREQUEST;
	}
	if (rep->master_id != DB_EID_INVALID) {
		rep->stat.st_pg_requested++;
		/*
		 * We need to request the pages, but we need to get the
		 * new info into rep->finfo.  Assert that the sizes never
		 * change.  The only thing this should do is change
		 * the pgno field.  Everything else remains the same.
		 */
		ret = __rep_fileinfo_buf(rep->finfo, max_pg_dbt.size, &len,
		    tmpfp->pgsize, tmpfp->pgno, tmpfp->max_pgno,
		    tmpfp->filenum, tmpfp->id, tmpfp->type,
		    tmpfp->flags, &tmpfp->uid, &tmpfp->info);
		DB_ASSERT(dbenv, len == max_pg_dbt.size);
		(void)__rep_send_message(dbenv, rep->master_id,
		    REP_PAGE_REQ, NULL, &max_pg_dbt, 0, flags);
	} else
		(void)__rep_send_message(dbenv, DB_EID_BROADCAST,
		    REP_MASTER_REQ, NULL, NULL, 0, 0);

	if (alloc)
		__os_free(dbenv, tmpfp);
	return (ret);
}

/*
 * __rep_finfo_alloc -
 *	Allocate and initialize a fileinfo structure.
 *
 * PUBLIC: int __rep_finfo_alloc __P((DB_ENV *, __rep_fileinfo_args *,
 * PUBLIC:    __rep_fileinfo_args **));
 */
int
__rep_finfo_alloc(dbenv, rfpsrc, rfpp)
	DB_ENV *dbenv;
	__rep_fileinfo_args *rfpsrc, **rfpp;
{
	__rep_fileinfo_args *rfp;
	size_t size;
	int ret;
	void *uidp, *infop;

	/*
	 * Allocate enough for the structure and the two DBT data areas.
	 */
	size = sizeof(__rep_fileinfo_args) + rfpsrc->uid.size +
	    rfpsrc->info.size;
	if ((ret = __os_malloc(dbenv, size, &rfp)) != 0)
		return (ret);

	/*
	 * Copy the structure itself, and then set the DBT data pointers
	 * to their space and copy the data itself as well.
	 */
	memcpy(rfp, rfpsrc, sizeof(__rep_fileinfo_args));
	uidp = (u_int8_t *)rfp + sizeof(__rep_fileinfo_args);
	rfp->uid.data = uidp;
	memcpy(uidp, rfpsrc->uid.data, rfpsrc->uid.size);

	infop = (u_int8_t *)uidp + rfpsrc->uid.size;
	rfp->info.data = infop;
	memcpy(infop, rfpsrc->info.data, rfpsrc->info.size);
	*rfpp = rfp;
	return (ret);
}

/*
 * __rep_log_setup -
 *	We know our first LSN and need to reset the log subsystem
 *	to get our logs set up for the proper file.
 */
static int
__rep_log_setup(dbenv, rep, file, version, lsnp)
	DB_ENV *dbenv;
	REP *rep;
	u_int32_t file;
	u_int32_t version;
	DB_LSN *lsnp;
{
	DB_LOG *dblp;
	DB_LSN lsn;
	DB_TXNMGR *mgr;
	DB_TXNREGION *region;
	LOG *lp;
	int ret;

	dblp = dbenv->lg_handle;
	lp = dblp->reginfo.primary;
	mgr = dbenv->tx_handle;
	region = mgr->reginfo.primary;

	/*
	 * Set up the log starting at the file number of the first LSN we
	 * need to get from the master.
	 */
	if ((ret = __log_newfile(dblp, &lsn, file, version)) == 0 &&
	    lsnp != NULL)
		*lsnp = lsn;

	/*
	 * We reset first_lsn to the lp->lsn.  We were given the LSN of
	 * the checkpoint and we now need the LSN for the beginning of
	 * the file, which __log_newfile conveniently set up for us
	 * in lp->lsn.
	 */
	rep->first_lsn = lp->lsn;
	TXN_SYSTEM_LOCK(dbenv);
	ZERO_LSN(region->last_ckp);
	TXN_SYSTEM_UNLOCK(dbenv);
	return (ret);
}

/*
 * __rep_queue_filedone -
 *	Determine if we're really done getting the pages for a queue file.
 *	Queue is handled in several steps.
 *	1.  First we get the meta page only.
 *	2.  We use the meta-page information to figure out first and last
 *	    page numbers (and if queue wraps, first can be > last.
 *	3.  If first < last, we do a REP_PAGE_REQ for all pages.
 *	4.  If first > last, we REP_PAGE_REQ from first -> max page number.
 *	    Then we'll ask for page 1 -> last.
 *
 * This function can return several things:
 *	DB_REP_PAGEDONE - if we're done with this file.
 *	0 - if we're not doen with this file.
 *	error - if we get an error doing some operations.
 *
 * This function will open a dbp handle to the queue file.  This is needed
 * by most of the QAM macros.  We'll open it on the first pass through
 * here and we'll close it whenever we decide we're done.
 */
static int
__rep_queue_filedone(dbenv, rep, rfp)
	DB_ENV *dbenv;
	REP *rep;
	__rep_fileinfo_args *rfp;
{
#ifndef HAVE_QUEUE
	COMPQUIET(rep, NULL);
	COMPQUIET(rfp, NULL);
	return (__db_no_queue_am(dbenv));
#else
	db_pgno_t first, last;
	u_int32_t flags;
	int empty, ret, t_ret;

	ret = 0;
	if (rep->queue_dbp == NULL) {
		/*
		 * We need to do a sync here so that the open
		 * can find the file and file id.
		 */
		if ((ret = __memp_sync(dbenv, NULL)) != 0)
			goto out;
		if ((ret = __db_create_internal(
		    &rep->queue_dbp, dbenv, 0)) != 0)
			goto out;
		flags = DB_NO_AUTO_COMMIT |
		    (F_ISSET(dbenv, DB_ENV_THREAD) ? DB_THREAD : 0);
		/*
		 * We need to check whether this is in-memory so that we pass
		 * the name correctly as either the file or the database name.
		 */
		if ((ret = __db_open(rep->queue_dbp, NULL,
		    FLD_ISSET(rfp->flags, DB_AM_INMEM) ? NULL : rfp->info.data,
		    FLD_ISSET(rfp->flags, DB_AM_INMEM) ? rfp->info.data : NULL,
		    DB_QUEUE, flags, 0, PGNO_BASE_MD)) != 0)
			goto out;
	}
	if ((ret = __queue_pageinfo(rep->queue_dbp,
	    &first, &last, &empty, 0, 0)) != 0)
		goto out;
	RPRINT(dbenv, (dbenv,
	    "Queue fileinfo: first %lu, last %lu, empty %d",
	    (u_long)first, (u_long)last, empty));
	/*
	 * We can be at the end of 3 possible states.
	 * 1.  We have received the meta-page and now need to get the
	 *     rest of the pages in the database.
	 * 2.  We have received from first -> max_pgno.  We might be done,
	 *     or we might need to ask for wrapped pages.
	 * 3.  We have received all pages in the file.  We're done.
	 */
	if (rfp->max_pgno == 0) {
		/*
		 * We have just received the meta page.  Set up the next
		 * pages to ask for and check if the file is empty.
		 */
		if (empty)
			goto out;
		if (first > last) {
			rfp->max_pgno =
			    QAM_RECNO_PAGE(rep->queue_dbp, UINT32_MAX);
		} else
			rfp->max_pgno = last;
		RPRINT(dbenv, (dbenv,
		    "Queue fileinfo: First req: first %lu, last %lu",
		    (u_long)first, (u_long)rfp->max_pgno));
		goto req;
	} else if (rfp->max_pgno != last) {
		/*
		 * If max_pgno != last that means we're dealing with a
		 * wrapped situation.  Request next batch of pages.
		 * Set npages to 1 because we already have page 0, the
		 * meta-page, now we need pages 1-max_pgno.
		 */
		first = 1;
		rfp->max_pgno = last;
		RPRINT(dbenv, (dbenv,
		    "Queue fileinfo: Wrap req: first %lu, last %lu",
		    (u_long)first, (u_long)last));
req:
		/*
		 * Since we're simulating a "gap" to resend new PAGE_REQ
		 * for this file, we need to set waiting page to last + 1
		 * so that we'll ask for all from ready_pg -> last.
		 */
		rep->npages = first;
		rep->ready_pg = first;
		rep->waiting_pg = rfp->max_pgno + 1;
		rep->max_wait_pg = PGNO_INVALID;
		ret = __rep_pggap_req(dbenv, rep, rfp, 0);
		return (ret);
	}
	/*
	 * max_pgno == last
	 * If we get here, we have all the pages we need.
	 * Close the dbp and return.
	 */
out:
	if (rep->queue_dbp != NULL &&
	    (t_ret = __db_close(rep->queue_dbp, NULL, DB_NOSYNC)) != 0 &&
	    ret == 0)
		ret = t_ret;
	rep->queue_dbp = NULL;
	if (ret == 0)
		ret = DB_REP_PAGEDONE;
	return (ret);
#endif
}

/*
 * PUBLIC: int __rep_remove_init_file __P((DB_ENV *));
 */
int
__rep_remove_init_file(dbenv)
	DB_ENV *dbenv;
{
	int ret;
	char *name;

	if ((ret = __db_appname(
	    dbenv, DB_APP_NONE, REP_INITNAME, 0, NULL, &name)) != 0)
		return (ret);
	(void)__os_unlink(dbenv, name);	
	__os_free(dbenv, name);
	return (0);
}	

/*
 * Checks for the existence of the internal init flag file.  If it exists, we
 * remove all logs and databases, and then remove the flag file.  This is
 * intended to force the internal init to start over again, and thus affords
 * protection against a client crashing during internal init.  This function
 * must be called before normal recovery in order to be properly effective.
 * 
 * !!!
 * This function should only be called during initial set-up of the environment,
 * before various subsystems are initialized.  It doesn't rely on the
 * subsystems' code having been initialized, and it summarily deletes files "out
 * from under" them, which might disturb the subsystems if they were up.
 * 
 * PUBLIC: int __rep_reset_init __P((DB_ENV *));
 */
int
__rep_reset_init(dbenv)
	DB_ENV *dbenv;
{
	DB_FH *fhp;
	__rep_update_args *rup;
	DBT dbt;
	char *allocated_dir, *dir, *init_name;
	void *next;
	int ret, t_ret;

	allocated_dir = NULL;
	rup = NULL;
	dbt.data = NULL;

	if ((ret = __db_appname(
	    dbenv, DB_APP_NONE, REP_INITNAME, 0, NULL, &init_name)) != 0)
		return (ret);
	
	if ((ret = __os_open(dbenv, init_name, 0, DB_OSO_RDONLY,
	    __db_omode(OWNER_RW), &fhp)) != 0) {
		if (ret == ENOENT)
			ret = 0;
		goto out;
	}
	
	RPRINT(dbenv, (dbenv, "Cleaning up interrupted internal init"));

	/* There are a few possibilities:
	 *   1. no init file, or less than 1 full file list
	 *   2. exactly one full file list
	 *   3. more than one, less then a second full file list
	 *   4. second file list in full
	 *
	 * In cases 2 or 4, we need to remove all logs, and then remove files
	 * according to the (most recent) file list.  (In case 1 or 3, we don't
	 * have to do anything.)
	 *
	 * The __rep_get_file_list function takes care of folding these cases
	 * into two simple outcomes:
	 */
	ret = __rep_get_file_list(dbenv, fhp, &dbt);
	if ((t_ret = __os_closehandle(dbenv, fhp)) != 0 || ret != 0) {
		if (ret == 0)
			ret = t_ret;
		goto out;
	}
	if (dbt.data == NULL) {
		/*
		 * The init file did not end with an intact file list.  Since we
		 * never start log/db removal without an intact file list
		 * sync'ed to the init file, this must mean we don't have any
		 * partial set of files to clean up.  So all we need to do is
		 * remove the init file.
		 */
		goto rm;
	}
		
	/* Remove all log files. */
	if (dbenv->db_log_dir == NULL)
		dir = dbenv->db_home;
	else {
		if ((ret = __db_appname(dbenv, DB_APP_NONE,
		    dbenv->db_log_dir, 0, NULL, &dir)) != 0)
			goto out;
		allocated_dir = dir;
	}

	if ((ret = __rep_remove_by_prefix(dbenv,
	    dir, LFPREFIX, sizeof(LFPREFIX)-1, DB_APP_LOG)) != 0)
		goto out;

	/*
	 * Remove databases according to the list, and queue extent files by
	 * searching them out on a walk through the data_dir's.
	 */
	if ((ret = __rep_update_read(dbenv, dbt.data, &next, &rup)) != 0)
		goto out;
	if ((ret = __rep_remove_by_list(dbenv, next, rup->num_files)) != 0)
		goto out;


	/* Here, we've established that the file exists. */
rm:	(void)__os_unlink(dbenv, init_name);
out:	if (rup != NULL)
		__os_free(dbenv, rup);
	if (allocated_dir != NULL)
		__os_free(dbenv, allocated_dir);
	if (dbt.data != NULL)
		free(dbt.data);	/* whatever that might mean */
	
	__os_free(dbenv, init_name);
	return (ret);
}

/*
 * Reads the last fully intact file list from the init file.  If the file ends
 * with a partial list (or is empty), we're not interested in it.  Lack of a
 * full file list is indicated by a NULL dbt->data.  On success, the list is
 * returned in allocated space, which becomes the responsibility of the caller.
 *
 * The file format is a u_int32_t buffer length, in native format, followed by
 * the file list itself, in the same format as in an UPDATE message (though many
 * parts of it in this case are meaningless).
 */
static int
__rep_get_file_list(dbenv, fhp, dbt)
	DB_ENV *dbenv;
	DB_FH *fhp;
	DBT *dbt;
{
	u_int32_t length;
	size_t cnt;
	int i, ret;

	/* At most 2 file lists: old and new. */
	dbt->data = NULL;
	for (i = 1; i <= 2; i++) {
		if ((ret = __os_read(dbenv,
		    fhp, &length, sizeof(length), &cnt)) != 0)
			goto err;

		/*
		 * Reaching the end here is fine, if we've been through at least
		 * once already.
		 */
		if (cnt == 0 && dbt->data != NULL)
			break;
		if (cnt != sizeof(length)) 
			goto err;

		if ((ret = __os_realloc(dbenv,
		    (size_t)length, &dbt->data)) != 0)
			goto err;

		if ((ret = __os_read(dbenv, fhp, dbt->data, length, &cnt)) != 0
		    || cnt != (size_t)length)
			goto err;
	}

	dbt->size = length;
	return (0);

err:
	/*
	 * Note that it's OK to get here with a zero value in 'ret': it means we
	 * read less than we expected, and dbt->data == NULL indicates to the
	 * caller that we don't have an intact list.
	 */
	if (dbt->data != NULL)
		__os_free(dbenv, dbt->data);
	dbt->data = NULL;
	return (ret);
}

/*
 * Removes every file in a given directory that matches a given prefix.  Notice
 * how similar this is to __rep_walk_dir.
 */
static int
__rep_remove_by_prefix(dbenv, dir, prefix, pref_len, appname)
	DB_ENV *dbenv;
	const char *dir;
	const char *prefix;
	size_t pref_len;
	APPNAME appname;	/* What kind of name. */
{
	char *namep, **names;
	int cnt, i, ret;

	if ((ret = __os_dirlist(dbenv, dir, &names, &cnt)) != 0)
		return (ret);
	for (i = 0; i < cnt; i++) {
		if (strncmp(names[i], prefix, pref_len) == 0) {
			if ((ret = __db_appname(dbenv,
			    appname, names[i], 0, NULL, &namep)) != 0)
				goto out;
			(void)__os_unlink(dbenv, namep);
			__os_free(dbenv, namep);
		}
	}
out:	__os_dirfree(dbenv, names, cnt);
	return (ret);
}

/*
 * Removes database files according to the contents of a list.
 *
 * This function must support removal either during environment creation, or
 * when an internal init is reset in the middle.  This means it must work
 * regardless of whether underlying subsystems are initialized.  However, it may
 * assume that databases are not open.
 */
static int
__rep_remove_by_list(dbenv, filelist, count)
	DB_ENV *dbenv;
	void *filelist;
	u_int32_t count;
{
	__rep_fileinfo_args *file_argsp;
	char **ddir, *dir, *namep;
	int ret;

	ret = 0;
	file_argsp = NULL;
	while (count-- > 0) {
		if ((ret = __rep_fileinfo_read(dbenv,
		    filelist, &filelist, &file_argsp)) != 0)
			goto out;
		if ((ret = __db_appname(dbenv,
		    DB_APP_DATA, file_argsp->info.data, 0, NULL, &namep)) != 0)
			goto out;
		(void)__os_unlink(dbenv, namep);
		__os_free(dbenv, namep);
		__os_free(dbenv, file_argsp);
		file_argsp = NULL;
	}


	/* Notice how similar this code is to __rep_find_dbs. */
	if (dbenv->db_data_dir == NULL)
		ret = __rep_remove_by_prefix(dbenv, dbenv->db_home,
		    QUEUE_EXTENT_PREFIX, sizeof(QUEUE_EXTENT_PREFIX) - 1,
		    DB_APP_DATA);
	else {
		for (ddir = dbenv->db_data_dir; *ddir != NULL; ++ddir) {
			if ((ret = __db_appname(dbenv, DB_APP_NONE,
			    *ddir, 0, NULL, &dir)) != 0)
				break;
			ret = __rep_remove_by_prefix(dbenv, dir,
			    QUEUE_EXTENT_PREFIX, sizeof(QUEUE_EXTENT_PREFIX)-1,
			    DB_APP_DATA);
			__os_free(dbenv, dir);
			if (ret != 0)
				break;
		}
	}


out:
	if (file_argsp != NULL)
		__os_free(dbenv, file_argsp);
	return (ret);
}

/*******************************************************************************
 *
 *                              Delta Chat Core
 *                      Copyright (C) 2017 Björn Petersen
 *                   Contact: r10s@b44t.com, http://b44t.com
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see http://www.gnu.org/licenses/ .
 *
 ******************************************************************************/


#include "dc_context.h"
#include "dc_apeerstate.h"


/* This class wraps around SQLite.  Some hints to the underlying database:

- `PRAGMA cache_size` and `PRAGMA page_size`: As we save BLOBs in external
  files, caching is not that important; we rely on the system defaults here
  (normally 2 MB cache, 1 KB page size on sqlite < 3.12.0, 4 KB for newer
  versions)

- We use `sqlite3_last_insert_rowid()` to find out created records - for this
  purpose, the primary ID has to be marked using `INTEGER PRIMARY KEY`, see
  https://www.sqlite.org/c3ref/last_insert_rowid.html

- Some words to the "param" fields:  These fields contains a string with
  additonal, named parameters which must not be accessed by a search and/or
  are very seldomly used. Moreover, this allows smart minor database updates. */


/*******************************************************************************
 * Tools
 ******************************************************************************/


void dc_sqlite3_log_error(dc_sqlite3_t* ths, const char* msg_format, ...)
{
	char*       msg;
	const char* notSetUp = "SQLite object not set up.";
	va_list     va;

	va_start(va, msg_format);
		msg = sqlite3_vmprintf(msg_format, va); if( msg == NULL ) { dc_log_error(ths->m_context, 0, "Bad log format string \"%s\".", msg_format); }
			dc_log_error(ths->m_context, 0, "%s SQLite says: %s", msg, ths->m_cobj? sqlite3_errmsg(ths->m_cobj) : notSetUp);
		sqlite3_free(msg);
	va_end(va);
}


sqlite3_stmt* dc_sqlite3_prepare_v2_(dc_sqlite3_t* ths, const char* querystr)
{
	sqlite3_stmt* retStmt = NULL;

	if( ths == NULL || querystr == NULL || ths->m_cobj == NULL ) {
		return NULL;
	}

	if( sqlite3_prepare_v2(ths->m_cobj,
	         querystr, -1 /*read `sql` up to the first null-byte*/,
	         &retStmt,
	         NULL /*tail not interesting, we use only single statements*/) != SQLITE_OK )
	{
		dc_sqlite3_log_error(ths, "Query failed: %s", querystr);
		return NULL;
	}

	/* success - the result must be freed using sqlite3_finalize() */
	return retStmt;
}


int dc_sqlite3_execute__(dc_sqlite3_t* ths, const char* querystr)
{
	int           success = 0;
	sqlite3_stmt* stmt = NULL;
	int           sqlState;

	stmt = dc_sqlite3_prepare_v2_(ths, querystr);
	if( stmt == NULL ) {
		goto cleanup;
	}

	sqlState = sqlite3_step(stmt);
	if( sqlState != SQLITE_DONE && sqlState != SQLITE_ROW )  {
		dc_sqlite3_log_error(ths, "Cannot excecute \"%s\".", querystr);
		goto cleanup;
	}

	success = 1;

cleanup:
	if( stmt ) {
		sqlite3_finalize(stmt);
	}
	return success;
}


/*******************************************************************************
 * Main interface
 ******************************************************************************/


dc_sqlite3_t* dc_sqlite3_new(dc_context_t* mailbox)
{
	dc_sqlite3_t* ths = NULL;
	int          i;

	if( (ths=calloc(1, sizeof(dc_sqlite3_t)))==NULL ) {
		exit(24); /* cannot allocate little memory, unrecoverable error */
	}

	ths->m_context          = mailbox;

	for( i = 0; i < PREDEFINED_CNT; i++ ) {
		ths->m_pd[i] = NULL;
	}

	pthread_mutex_init(&ths->m_critical_, NULL);

	return ths;
}


void dc_sqlite3_unref(dc_sqlite3_t* ths)
{
	if( ths == NULL ) {
		return;
	}

	if( ths->m_cobj ) {
		pthread_mutex_lock(&ths->m_critical_); /* as a very exeception, we do the locking inside the mrsqlite3-class - normally, this should be done by the caller! */
			dc_sqlite3_close__(ths);
		pthread_mutex_unlock(&ths->m_critical_);
	}

	pthread_mutex_destroy(&ths->m_critical_);
	free(ths);
}


int dc_sqlite3_open__(dc_sqlite3_t* ths, const char* dbfile, int flags)
{
	if( ths == NULL || dbfile == NULL ) {
		goto cleanup;
	}

	if( sqlite3_threadsafe() == 0 ) {
		dc_log_error(ths->m_context, 0, "Sqlite3 compiled thread-unsafe; this is not supported.");
		goto cleanup;
	}

	if( ths->m_cobj ) {
		dc_log_error(ths->m_context, 0, "Cannot open, database \"%s\" already opened.", dbfile);
		goto cleanup;
	}

	// Force serialized mode (SQLITE_OPEN_FULLMUTEX) explicitly.
	// So, most of the explicit lock/unlocks on dc_sqlite3_t object are no longer needed.
	// However, locking is _also_ used for dc_context_t which _is_ still needed, so, we
	// should remove locks only if we're really sure.
	if( sqlite3_open_v2(dbfile, &ths->m_cobj,
			SQLITE_OPEN_FULLMUTEX | ((flags&MR_OPEN_READONLY)? SQLITE_OPEN_READONLY : (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE)),
			NULL) != SQLITE_OK ) {
		dc_sqlite3_log_error(ths, "Cannot open database \"%s\".", dbfile); /* ususally, even for errors, the pointer is set up (if not, this is also checked by dc_sqlite3_log_error()) */
		goto cleanup;
	}

	// Only one process can make changes to the database at one time.
	// busy_timeout defines, that if a seconds process wants write access, this second process will wait some milliseconds
	// and try over until it gets write access or the given timeout is elapsed.
	// If the second process does not get write access within the given timeout, sqlite3_step() will return the error SQLITE_BUSY.
	// (without a busy_timeout, sqlite3_step() would return SQLITE_BUSY at once)
	sqlite3_busy_timeout(ths->m_cobj, 10*1000);

	if( !(flags&MR_OPEN_READONLY) )
	{
		int dbversion_before_update = 0;

		/* Init tables to dbversion=0 */
		if( !dc_sqlite3_table_exists__(ths, "config") )
		{
			dc_log_info(ths->m_context, 0, "First time init: creating tables in \"%s\".", dbfile);

			dc_sqlite3_execute__(ths, "CREATE TABLE config (id INTEGER PRIMARY KEY, keyname TEXT, value TEXT);");
			dc_sqlite3_execute__(ths, "CREATE INDEX config_index1 ON config (keyname);");

			dc_sqlite3_execute__(ths, "CREATE TABLE contacts (id INTEGER PRIMARY KEY,"
						" name TEXT DEFAULT '',"
						" addr TEXT DEFAULT '' COLLATE NOCASE,"
						" origin INTEGER DEFAULT 0,"
						" blocked INTEGER DEFAULT 0,"
						" last_seen INTEGER DEFAULT 0,"   /* last_seen is for future use */
						" param TEXT DEFAULT '');");      /* param is for future use, eg. for the status */
			dc_sqlite3_execute__(ths, "CREATE INDEX contacts_index1 ON contacts (name COLLATE NOCASE);"); /* needed for query contacts */
			dc_sqlite3_execute__(ths, "CREATE INDEX contacts_index2 ON contacts (addr COLLATE NOCASE);"); /* needed for query and on receiving mails */
			dc_sqlite3_execute__(ths, "INSERT INTO contacts (id,name,origin) VALUES (1,'self',262144), (2,'device',262144), (3,'rsvd',262144), (4,'rsvd',262144), (5,'rsvd',262144), (6,'rsvd',262144), (7,'rsvd',262144), (8,'rsvd',262144), (9,'rsvd',262144);");
			#if !defined(MR_ORIGIN_INTERNAL) || MR_ORIGIN_INTERNAL!=262144
				#error
			#endif

			dc_sqlite3_execute__(ths, "CREATE TABLE chats (id INTEGER PRIMARY KEY, "
						" type INTEGER DEFAULT 0,"
						" name TEXT DEFAULT '',"
						" draft_timestamp INTEGER DEFAULT 0,"
						" draft_txt TEXT DEFAULT '',"
						" blocked INTEGER DEFAULT 0,"
						" grpid TEXT DEFAULT '',"          /* contacts-global unique group-ID, see mrchat.c for details */
						" param TEXT DEFAULT '');");
			dc_sqlite3_execute__(ths, "CREATE INDEX chats_index1 ON chats (grpid);");
			dc_sqlite3_execute__(ths, "CREATE TABLE chats_contacts (chat_id INTEGER, contact_id INTEGER);");
			dc_sqlite3_execute__(ths, "CREATE INDEX chats_contacts_index1 ON chats_contacts (chat_id);");
			dc_sqlite3_execute__(ths, "INSERT INTO chats (id,type,name) VALUES (1,120,'deaddrop'), (2,120,'rsvd'), (3,120,'trash'), (4,120,'msgs_in_creation'), (5,120,'starred'), (6,120,'archivedlink'), (7,100,'rsvd'), (8,100,'rsvd'), (9,100,'rsvd');");
			#if !defined(MR_CHAT_TYPE_SINGLE) || MR_CHAT_TYPE_SINGLE!=100 || MR_CHAT_TYPE_GROUP!=120 || \
			 MR_CHAT_ID_DEADDROP!=1 || MR_CHAT_ID_TRASH!=3 || \
			 MR_CHAT_ID_MSGS_IN_CREATION!=4 || MR_CHAT_ID_STARRED!=5 || MR_CHAT_ID_ARCHIVED_LINK!=6 || \
			 MR_CHAT_NOT_BLOCKED!=0  || MR_CHAT_MANUALLY_BLOCKED!=1 || MR_CHAT_DEADDROP_BLOCKED!=2
				#error
			#endif

			dc_sqlite3_execute__(ths, "CREATE TABLE msgs (id INTEGER PRIMARY KEY,"
						" rfc724_mid TEXT DEFAULT '',"     /* forever-global-unique Message-ID-string, unfortunately, this cannot be easily used to communicate via IMAP */
						" server_folder TEXT DEFAULT '',"  /* folder as used on the server, the folder will change when messages are moved around. */
						" server_uid INTEGER DEFAULT 0,"   /* UID as used on the server, the UID will change when messages are moved around, unique together with validity, see RFC 3501; the validity may differ from folder to folder.  We use the server_uid for "markseen" and to delete messages as we check against the message-id, we ignore the validity for these commands. */
						" chat_id INTEGER DEFAULT 0,"
						" from_id INTEGER DEFAULT 0,"
						" to_id INTEGER DEFAULT 0,"        /* to_id is needed to allow moving messages eg. from "deaddrop" to a normal chat, may be unset */
						" timestamp INTEGER DEFAULT 0,"
						" type INTEGER DEFAULT 0,"
						" state INTEGER DEFAULT 0,"
						" msgrmsg INTEGER DEFAULT 1,"      /* does the message come from a messenger? (0=no, 1=yes, 2=no, but the message is a reply to a messenger message) */
						" bytes INTEGER DEFAULT 0,"        /* not used, added in ~ v0.1.12 */
						" txt TEXT DEFAULT '',"            /* as this is also used for (fulltext) searching, nothing but normal, plain text should go here */
						" txt_raw TEXT DEFAULT '',"
						" param TEXT DEFAULT '');");
			dc_sqlite3_execute__(ths, "CREATE INDEX msgs_index1 ON msgs (rfc724_mid);");     /* in our database, one email may be split up to several messages (eg. one per image), so the email-Message-ID may be used for several records; id is always unique */
			dc_sqlite3_execute__(ths, "CREATE INDEX msgs_index2 ON msgs (chat_id);");
			dc_sqlite3_execute__(ths, "CREATE INDEX msgs_index3 ON msgs (timestamp);");      /* for sorting */
			dc_sqlite3_execute__(ths, "CREATE INDEX msgs_index4 ON msgs (state);");          /* for selecting the count of fresh messages (as there are normally only few unread messages, an index over the chat_id is not required for _this_ purpose */
			dc_sqlite3_execute__(ths, "INSERT INTO msgs (id,msgrmsg,txt) VALUES (1,0,'marker1'), (2,0,'rsvd'), (3,0,'rsvd'), (4,0,'rsvd'), (5,0,'rsvd'), (6,0,'rsvd'), (7,0,'rsvd'), (8,0,'rsvd'), (9,0,'daymarker');"); /* make sure, the reserved IDs are not used */

			dc_sqlite3_execute__(ths, "CREATE TABLE jobs (id INTEGER PRIMARY KEY,"
						" added_timestamp INTEGER,"
						" desired_timestamp INTEGER DEFAULT 0,"
						" action INTEGER,"
						" foreign_id INTEGER,"
						" param TEXT DEFAULT '');");
			dc_sqlite3_execute__(ths, "CREATE INDEX jobs_index1 ON jobs (desired_timestamp);");

			if( !dc_sqlite3_table_exists__(ths, "config") || !dc_sqlite3_table_exists__(ths, "contacts")
			 || !dc_sqlite3_table_exists__(ths, "chats") || !dc_sqlite3_table_exists__(ths, "chats_contacts")
			 || !dc_sqlite3_table_exists__(ths, "msgs") || !dc_sqlite3_table_exists__(ths, "jobs") )
			{
				dc_sqlite3_log_error(ths, "Cannot create tables in new database \"%s\".", dbfile);
				goto cleanup; /* cannot create the tables - maybe we cannot write? */
			}

			dc_sqlite3_set_config_int__(ths, "dbversion", 0);
		}
		else
		{
			dbversion_before_update = dc_sqlite3_get_config_int__(ths, "dbversion", 0);
		}

		// (1) update low-level database structure.
		// this should be done before updates that use high-level objects that rely themselves on the low-level structure.
		int dbversion = dbversion_before_update;
		int recalc_fingerprints = 0;

		#define NEW_DB_VERSION 1
			if( dbversion < NEW_DB_VERSION )
			{
				dc_sqlite3_execute__(ths, "CREATE TABLE leftgrps ("
							" id INTEGER PRIMARY KEY,"
							" grpid TEXT DEFAULT '');");
				dc_sqlite3_execute__(ths, "CREATE INDEX leftgrps_index1 ON leftgrps (grpid);");

				dbversion = NEW_DB_VERSION;
				dc_sqlite3_set_config_int__(ths, "dbversion", NEW_DB_VERSION);
			}
		#undef NEW_DB_VERSION

		#define NEW_DB_VERSION 2
			if( dbversion < NEW_DB_VERSION )
			{
				dc_sqlite3_execute__(ths, "ALTER TABLE contacts ADD COLUMN authname TEXT DEFAULT '';");

				dbversion = NEW_DB_VERSION;
				dc_sqlite3_set_config_int__(ths, "dbversion", NEW_DB_VERSION);
			}
		#undef NEW_DB_VERSION

		#define NEW_DB_VERSION 7
			if( dbversion < NEW_DB_VERSION )
			{
				dc_sqlite3_execute__(ths, "CREATE TABLE keypairs ("
							" id INTEGER PRIMARY KEY,"
							" addr TEXT DEFAULT '' COLLATE NOCASE,"
							" is_default INTEGER DEFAULT 0,"
							" private_key,"
							" public_key,"
							" created INTEGER DEFAULT 0);");

				dbversion = NEW_DB_VERSION;
				dc_sqlite3_set_config_int__(ths, "dbversion", NEW_DB_VERSION);
			}
		#undef NEW_DB_VERSION

		#define NEW_DB_VERSION 10
			if( dbversion < NEW_DB_VERSION )
			{
				dc_sqlite3_execute__(ths, "CREATE TABLE acpeerstates ("
							" id INTEGER PRIMARY KEY,"
							" addr TEXT DEFAULT '' COLLATE NOCASE,"    /* no UNIQUE here, Autocrypt: requires the index above mail+type (type however, is not used at the moment, but to be future-proof, we do not use an index. instead we just check ourself if there is a record or not)*/
							" last_seen INTEGER DEFAULT 0,"
							" last_seen_autocrypt INTEGER DEFAULT 0,"
							" public_key,"
							" prefer_encrypted INTEGER DEFAULT 0);");
				dc_sqlite3_execute__(ths, "CREATE INDEX acpeerstates_index1 ON acpeerstates (addr);");

				dbversion = NEW_DB_VERSION;
				dc_sqlite3_set_config_int__(ths, "dbversion", NEW_DB_VERSION);
			}
		#undef NEW_DB_VERSION

		#define NEW_DB_VERSION 12
			if( dbversion < NEW_DB_VERSION )
			{
				dc_sqlite3_execute__(ths, "CREATE TABLE msgs_mdns ("
							" msg_id INTEGER, "
							" contact_id INTEGER);");
				dc_sqlite3_execute__(ths, "CREATE INDEX msgs_mdns_index1 ON msgs_mdns (msg_id);");

				dbversion = NEW_DB_VERSION;
				dc_sqlite3_set_config_int__(ths, "dbversion", NEW_DB_VERSION);
			}
		#undef NEW_DB_VERSION

		#define NEW_DB_VERSION 17
			if( dbversion < NEW_DB_VERSION )
			{
				dc_sqlite3_execute__(ths, "ALTER TABLE chats ADD COLUMN archived INTEGER DEFAULT 0;");
				dc_sqlite3_execute__(ths, "CREATE INDEX chats_index2 ON chats (archived);");
				dc_sqlite3_execute__(ths, "ALTER TABLE msgs ADD COLUMN starred INTEGER DEFAULT 0;");
				dc_sqlite3_execute__(ths, "CREATE INDEX msgs_index5 ON msgs (starred);");

				dbversion = NEW_DB_VERSION;
				dc_sqlite3_set_config_int__(ths, "dbversion", NEW_DB_VERSION);
			}
		#undef NEW_DB_VERSION

		#define NEW_DB_VERSION 18
			if( dbversion < NEW_DB_VERSION )
			{
				dc_sqlite3_execute__(ths, "ALTER TABLE acpeerstates ADD COLUMN gossip_timestamp INTEGER DEFAULT 0;");
				dc_sqlite3_execute__(ths, "ALTER TABLE acpeerstates ADD COLUMN gossip_key;");

				dbversion = NEW_DB_VERSION;
				dc_sqlite3_set_config_int__(ths, "dbversion", NEW_DB_VERSION);
			}
		#undef NEW_DB_VERSION

		#define NEW_DB_VERSION 27
			if( dbversion < NEW_DB_VERSION )
			{
				dc_sqlite3_execute__(ths, "DELETE FROM msgs WHERE chat_id=1 OR chat_id=2;"); /* chat.id=1 and chat.id=2 are the old deaddrops, the current ones are defined by chats.blocked=2 */
				dc_sqlite3_execute__(ths, "CREATE INDEX chats_contacts_index2 ON chats_contacts (contact_id);"); /* needed to find chat by contact list */
				dc_sqlite3_execute__(ths, "ALTER TABLE msgs ADD COLUMN timestamp_sent INTEGER DEFAULT 0;");
				dc_sqlite3_execute__(ths, "ALTER TABLE msgs ADD COLUMN timestamp_rcvd INTEGER DEFAULT 0;");

				dbversion = NEW_DB_VERSION;
				dc_sqlite3_set_config_int__(ths, "dbversion", NEW_DB_VERSION);
			}
		#undef NEW_DB_VERSION

		#define NEW_DB_VERSION 34
			if( dbversion < NEW_DB_VERSION )
			{
				dc_sqlite3_execute__(ths, "ALTER TABLE msgs ADD COLUMN hidden INTEGER DEFAULT 0;");
				dc_sqlite3_execute__(ths, "ALTER TABLE msgs_mdns ADD COLUMN timestamp_sent INTEGER DEFAULT 0;");
				dc_sqlite3_execute__(ths, "ALTER TABLE acpeerstates ADD COLUMN public_key_fingerprint TEXT DEFAULT '';"); /* do not add `COLLATE NOCASE` case-insensivity is not needed as we force uppercase on store - otoh case-sensivity may be neeed for other/upcoming fingerprint formats */
				dc_sqlite3_execute__(ths, "ALTER TABLE acpeerstates ADD COLUMN gossip_key_fingerprint TEXT DEFAULT '';"); /* do not add `COLLATE NOCASE` case-insensivity is not needed as we force uppercase on store - otoh case-sensivity may be neeed for other/upcoming fingerprint formats */
				dc_sqlite3_execute__(ths, "CREATE INDEX acpeerstates_index3 ON acpeerstates (public_key_fingerprint);");
				dc_sqlite3_execute__(ths, "CREATE INDEX acpeerstates_index4 ON acpeerstates (gossip_key_fingerprint);");
				recalc_fingerprints = 1;

				dbversion = NEW_DB_VERSION;
				dc_sqlite3_set_config_int__(ths, "dbversion", NEW_DB_VERSION);
			}
		#undef NEW_DB_VERSION

		#define NEW_DB_VERSION 39
			if( dbversion < NEW_DB_VERSION )
			{
				dc_sqlite3_execute__(ths, "CREATE TABLE tokens ("
							" id INTEGER PRIMARY KEY,"
							" namespc INTEGER DEFAULT 0,"
							" foreign_id INTEGER DEFAULT 0,"
							" token TEXT DEFAULT '',"
							" timestamp INTEGER DEFAULT 0);");
				dc_sqlite3_execute__(ths, "ALTER TABLE acpeerstates ADD COLUMN verified_key;");
				dc_sqlite3_execute__(ths, "ALTER TABLE acpeerstates ADD COLUMN verified_key_fingerprint TEXT DEFAULT '';"); /* do not add `COLLATE NOCASE` case-insensivity is not needed as we force uppercase on store - otoh case-sensivity may be neeed for other/upcoming fingerprint formats */
				dc_sqlite3_execute__(ths, "CREATE INDEX acpeerstates_index5 ON acpeerstates (verified_key_fingerprint);");

				if( dbversion_before_update == 34 )
				{
					// migrate database from the use of verified-flags to verified_key,
					// _only_ version 34 (0.17.0) has the fields public_key_verified and gossip_key_verified
					// this block can be deleted in half a year or so (created 5/2018)
					dc_sqlite3_execute__(ths, "UPDATE acpeerstates SET verified_key=gossip_key, verified_key_fingerprint=gossip_key_fingerprint WHERE gossip_key_verified=2;");
					dc_sqlite3_execute__(ths, "UPDATE acpeerstates SET verified_key=public_key, verified_key_fingerprint=public_key_fingerprint WHERE public_key_verified=2;");
				}

				dbversion = NEW_DB_VERSION;
				dc_sqlite3_set_config_int__(ths, "dbversion", NEW_DB_VERSION);
			}
		#undef NEW_DB_VERSION

		#define NEW_DB_VERSION 40
			if( dbversion < NEW_DB_VERSION )
			{
				dc_sqlite3_execute__(ths, "ALTER TABLE jobs ADD COLUMN thread INTEGER DEFAULT 0;");

				dbversion = NEW_DB_VERSION;
				dc_sqlite3_set_config_int__(ths, "dbversion", NEW_DB_VERSION);
			}
		#undef NEW_DB_VERSION

		// (2) updates that require high-level objects (the structure is complete now and all objects are usable)
		if( recalc_fingerprints )
		{
			sqlite3_stmt* stmt = dc_sqlite3_prepare_v2_(ths, "SELECT addr FROM acpeerstates;");
				while( sqlite3_step(stmt) == SQLITE_ROW ) {
					dc_apeerstate_t* peerstate = dc_apeerstate_new(ths->m_context);
						if( dc_apeerstate_load_by_addr__(peerstate, ths, (const char*)sqlite3_column_text(stmt, 0))
						 && dc_apeerstate_recalc_fingerprint(peerstate) ) {
							dc_apeerstate_save_to_db__(peerstate, ths, 0/*don't create*/);
						}
					dc_apeerstate_unref(peerstate);
				}
			sqlite3_finalize(stmt);
		}
	}

	dc_log_info(ths->m_context, 0, "Opened \"%s\" successfully.", dbfile);
	return 1;

cleanup:
	dc_sqlite3_close__(ths);
	return 0;
}


void dc_sqlite3_close__(dc_sqlite3_t* ths)
{
	int i;

	if( ths == NULL ) {
		return;
	}

	if( ths->m_cobj )
	{
		for( i = 0; i < PREDEFINED_CNT; i++ ) {
			if( ths->m_pd[i] ) {
				sqlite3_finalize(ths->m_pd[i]);
				ths->m_pd[i] = NULL;
			}
		}

		sqlite3_close(ths->m_cobj);
		ths->m_cobj = NULL;
	}

	dc_log_info(ths->m_context, 0, "Database closed."); /* We log the information even if not real closing took place; this is to detect logic errors. */
}


int dc_sqlite3_is_open(const dc_sqlite3_t* ths)
{
	if( ths == NULL || ths->m_cobj == NULL ) {
		return 0;
	}
	return 1;
}


sqlite3_stmt* dc_sqlite3_predefine__(dc_sqlite3_t* ths, size_t idx, const char* querystr)
{
	/* Predefines a statement or resets and reuses a statement.

	The same idx MUST NOT be used at the same time from different threads and
	you MUST NOT call this function with different strings for the same index. */

	if( ths == NULL || ths->m_cobj == NULL || idx >= PREDEFINED_CNT ) {
		return NULL;
	}

	if( ths->m_pd[idx] ) {
		sqlite3_reset(ths->m_pd[idx]);
		return ths->m_pd[idx]; /* fine, already prepared before */
	}

	/*prepare for the first time - this requires the querystring*/
	if( querystr == NULL ) {
		return NULL;
	}

	if( sqlite3_prepare_v2(ths->m_cobj,
	         querystr, -1 /*read `sql` up to the first null-byte*/,
	         &ths->m_pd[idx],
	         NULL /*tail not interesing, we use only single statements*/) != SQLITE_OK )
	{
		dc_sqlite3_log_error(ths, "Preparing statement \"%s\" failed.", querystr);
		return NULL;
	}

	return ths->m_pd[idx];
}


void dc_sqlite3_reset_all_predefinitions(dc_sqlite3_t* ths)
{
	int i;
	for( i = 0; i < PREDEFINED_CNT; i++ ) {
		if( ths->m_pd[i] ) {
			sqlite3_reset(ths->m_pd[i]);
		}
	}
}


int dc_sqlite3_table_exists__(dc_sqlite3_t* ths, const char* name)
{
	int           ret = 0;
	char*         querystr = NULL;
	sqlite3_stmt* stmt = NULL;
	int           sqlState;

	if( (querystr=sqlite3_mprintf("PRAGMA table_info(%s)", name)) == NULL ) { /* this statement cannot be used with binded variables */
		dc_log_error(ths->m_context, 0, "dc_sqlite3_table_exists_(): Out of memory.");
		goto cleanup;
	}

	if( (stmt=dc_sqlite3_prepare_v2_(ths, querystr)) == NULL ) {
		goto cleanup;
	}

	sqlState = sqlite3_step(stmt);
	if( sqlState == SQLITE_ROW ) {
		ret = 1; /* the table exists. Other states are SQLITE_DONE or SQLITE_ERROR in both cases we return 0. */
	}

	/* success - fall through to free allocated objects */
	;

	/* error/cleanup */
cleanup:
	if( stmt ) {
		sqlite3_finalize(stmt);
	}

	if( querystr ) {
		sqlite3_free(querystr);
	}

	return ret;
}


/*******************************************************************************
 * Handle configuration
 ******************************************************************************/


int dc_sqlite3_set_config__(dc_sqlite3_t* ths, const char* key, const char* value)
{
	int           state;
	sqlite3_stmt* stmt;

	if( key == NULL ) {
		dc_log_error(ths->m_context, 0, "dc_sqlite3_set_config(): Bad parameter.");
		return 0;
	}

	if( !dc_sqlite3_is_open(ths) ) {
		dc_log_error(ths->m_context, 0, "dc_sqlite3_set_config(): Database not ready.");
		return 0;
	}

	if( value )
	{
		/* insert/update key=value */
		#define SELECT_v_FROM_config_k_STATEMENT "SELECT value FROM config WHERE keyname=?;"
		stmt = dc_sqlite3_predefine__(ths, SELECT_v_FROM_config_k, SELECT_v_FROM_config_k_STATEMENT);
		sqlite3_bind_text (stmt, 1, key, -1, SQLITE_STATIC);
		state=sqlite3_step(stmt);
		if( state == SQLITE_DONE ) {
			stmt = dc_sqlite3_predefine__(ths, INSERT_INTO_config_kv, "INSERT INTO config (keyname, value) VALUES (?, ?);");
			sqlite3_bind_text (stmt, 1, key,   -1, SQLITE_STATIC);
			sqlite3_bind_text (stmt, 2, value, -1, SQLITE_STATIC);
			state=sqlite3_step(stmt);

		}
		else if( state == SQLITE_ROW ) {
			stmt = dc_sqlite3_predefine__(ths, UPDATE_config_vk, "UPDATE config SET value=? WHERE keyname=?;");
			sqlite3_bind_text (stmt, 1, value, -1, SQLITE_STATIC);
			sqlite3_bind_text (stmt, 2, key,   -1, SQLITE_STATIC);
			state=sqlite3_step(stmt);
		}
		else {
			dc_log_error(ths->m_context, 0, "dc_sqlite3_set_config(): Cannot read value.");
			return 0;
		}
	}
	else
	{
		/* delete key */
		stmt = dc_sqlite3_predefine__(ths, DELETE_FROM_config_k, "DELETE FROM config WHERE keyname=?;");
		sqlite3_bind_text (stmt, 1, key,   -1, SQLITE_STATIC);
		state=sqlite3_step(stmt);
	}

	if( state != SQLITE_DONE )  {
		dc_log_error(ths->m_context, 0, "dc_sqlite3_set_config(): Cannot change value.");
		return 0;
	}

	return 1;
}


char* dc_sqlite3_get_config__(dc_sqlite3_t* ths, const char* key, const char* def) /* the returned string must be free()'d, NULL is only returned if def is NULL */
{
	sqlite3_stmt* stmt;

	if( !dc_sqlite3_is_open(ths) || key == NULL ) {
		return strdup_keep_null(def);
	}

	stmt = dc_sqlite3_predefine__(ths, SELECT_v_FROM_config_k, SELECT_v_FROM_config_k_STATEMENT);
	sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
	if( sqlite3_step(stmt) == SQLITE_ROW )
	{
		const unsigned char* ptr = sqlite3_column_text(stmt, 0); /* Do not pass the pointers returned from sqlite3_column_text(), etc. into sqlite3_free(). */
		if( ptr )
		{
			/* success, fall through below to free objects */
			return safe_strdup((const char*)ptr);
		}
	}

	/* return the default value */
	return strdup_keep_null(def);
}


int32_t dc_sqlite3_get_config_int__(dc_sqlite3_t* ths, const char* key, int32_t def)
{
    char* str = dc_sqlite3_get_config__(ths, key, NULL);
    if( str == NULL ) {
		return def;
    }
    int32_t ret = atol(str);
    free(str);
    return ret;
}


int dc_sqlite3_set_config_int__(dc_sqlite3_t* ths, const char* key, int32_t value)
{
    char* value_str = dc_mprintf("%i", (int)value);
    if( value_str == NULL ) {
		return 0;
    }
    int ret = dc_sqlite3_set_config__(ths, key, value_str);
    free(value_str);
    return ret;
}


/*******************************************************************************
 * Locking
 ******************************************************************************/


#ifdef MR_USE_LOCK_DEBUG
void dc_sqlite3_lockNdebug(dc_sqlite3_t* ths, const char* filename, int linenum) /* wait and lock */
#else
void dc_sqlite3_lock(dc_sqlite3_t* ths) /* wait and lock */
#endif
{
	#ifdef MR_USE_LOCK_DEBUG
		clock_t start = clock();
		dc_log_info(ths->m_context, 0, "    waiting for lock at %s#L%i", filename, linenum);
	#endif

	pthread_mutex_lock(&ths->m_critical_);

	#ifdef MR_USE_LOCK_DEBUG
		dc_log_info(ths->m_context, 0, "{{{ LOCK AT %s#L%i after %.3f ms", filename, linenum, (double)(clock()-start)*1000.0/CLOCKS_PER_SEC);
	#endif
}


#ifdef MR_USE_LOCK_DEBUG
void dc_sqlite3_unlockNdebug(dc_sqlite3_t* ths, const char* filename, int linenum)
#else
void dc_sqlite3_unlock(dc_sqlite3_t* ths)
#endif
{
	#ifdef MR_USE_LOCK_DEBUG
		dc_log_info(ths->m_context, 0, "    UNLOCK AT %s#L%i }}}", filename, linenum);
	#endif

	pthread_mutex_unlock(&ths->m_critical_);
}


/*******************************************************************************
 * Transactions
 ******************************************************************************/


void dc_sqlite3_begin_transaction__(dc_sqlite3_t* ths)
{
	sqlite3_stmt* stmt;

	ths->m_transactionCount++; /* this is safe, as the database should be locked when using a transaction */

	if( ths->m_transactionCount == 1 )
	{
		stmt = dc_sqlite3_predefine__(ths, BEGIN_transaction, "BEGIN;");
		if( sqlite3_step(stmt) != SQLITE_DONE ) {
			dc_sqlite3_log_error(ths, "Cannot begin transaction.");
		}
	}
}


void dc_sqlite3_rollback__(dc_sqlite3_t* ths)
{
	sqlite3_stmt* stmt;

	if( ths->m_transactionCount >= 1 )
	{
		if( ths->m_transactionCount == 1 )
		{
			stmt = dc_sqlite3_predefine__(ths, ROLLBACK_transaction, "ROLLBACK;");
			if( sqlite3_step(stmt) != SQLITE_DONE ) {
				dc_sqlite3_log_error(ths, "Cannot rollback transaction.");
			}
		}

		ths->m_transactionCount--;
	}
}


void dc_sqlite3_commit__(dc_sqlite3_t* ths)
{
	sqlite3_stmt* stmt;

	if( ths->m_transactionCount >= 1 )
	{
		if( ths->m_transactionCount == 1 )
		{
			stmt = dc_sqlite3_predefine__(ths, COMMIT_transaction, "COMMIT;");
			if( sqlite3_step(stmt) != SQLITE_DONE ) {
				dc_sqlite3_log_error(ths, "Cannot commit transaction.");
			}
		}

		ths->m_transactionCount--;
	}
}
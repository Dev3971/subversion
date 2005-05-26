/* intl.c: Internationalization and localization for Subversion.
 *
 * ====================================================================
 * Copyright (c) 2005 CollabNet.  All rights reserved.
 *
 * This software is licensed as described in the file COPYING, which
 * you should have received as part of this distribution.  The terms
 * are also available at http://subversion.tigris.org/license-1.html.
 * If newer versions of this license are posted there, you may use a
 * newer version instead, at your option.
 *
 * This software consists of voluntary contributions made by many
 * individuals.  For exact contribution history, see the revision
 * history and logs, available at http://subversion.tigris.org/.
 * ====================================================================
 */

#include <apr_errno.h>
#include <apr_mmap.h>
#include <apr_file_io.h>
#include <apr_strings.h>
#include <assert.h>
#include <apr_hash.h>
#include <ctype.h>
#include <stdlib.h>

#include "svn_intl.h"
#include "svn_private_config.h" /* for SVN_LOCALE_DIR */



static apr_hash_t *cache;
static apr_pool_t *private_pool;
#if APR_HAS_THREADS
static apr_thread_mutex_t *cache_lock;
#endif

#if !APR_HAS_MMAP
#error This needs mmap support
#endif

typedef struct
{
  char domain[32];
  char locale[16];
} message_table_key_t;

apr_status_t
svn_intl_initialize (apr_pool_t *parent_pool)
{
  apr_status_t st;

  st = apr_pool_create (&private_pool, parent_pool);
  if (st)
    return st;
  cache = apr_hash_make (private_pool);
#if APR_HAS_THREADS
  st =
    apr_thread_mutex_create (&cache_lock, APR_THREAD_MUTEX_DEFAULT,
			     private_pool);
  if (st)
    {
      apr_pool_destroy (private_pool);
      return st;
    }
#endif
  return 0;
}

/* ### Is there a way to register svn_gettext_terminate as a clean
   ### hook which is called on SVN or APR termination? */

apr_status_t
svn_intl_terminate (void)
{
  apr_pool_destroy (private_pool);
  return 0;
}

typedef struct
{
  apr_uint32_t len;
  apr_uint32_t offset;
} message_entry_t;

typedef struct
{
  apr_mmap_t *map;
  apr_uint32_t num_strings;
  message_entry_t *original;
  message_entry_t *translated;
} message_table_t;

static apr_status_t
message_table_open (message_table_t **mt, const char *domain,
                    const char *locale, apr_pool_t * pool)
{
  char fn[200];
  const char *header;
  apr_finfo_t finfo;
  apr_file_t *file;
  const void *mo;
  apr_uint32_t *mem;
  apr_status_t st;

  /* Follows macro usage from libsvn_subr/cmdline.c:svn_cmdline_init() */
  apr_snprintf (fn, sizeof (fn),
                SVN_LOCALE_DIR "/%s/LC_MESSAGES/%s.mo",
                locale, domain);
  *mt = (message_table_t *) apr_palloc (pool, sizeof (**mt));
#if 0
  printf("Looking for localization bundle '%s'...\n", fn);
#endif
  st = apr_stat (&finfo, fn, APR_FINFO_SIZE, pool);
  if (st)
    return st;
  st =
    apr_file_open (&file, fn, APR_READ | APR_BINARY | APR_SHARELOCK,
		   APR_OS_DEFAULT, pool);
  if (st)
    return st;
  st = apr_file_lock (file, APR_FLOCK_SHARED);
  if (st)
    return st;
  st =
    apr_mmap_create (&((*mt)->map), file, 0, finfo.size, APR_MMAP_READ, pool);
  if (st)
    return st;
  apr_mmap_offset ((void **) &mo, (*mt)->map, 0);
  mem = (apr_uint32_t *) mo;
  /* ### What does this big numeric value represent? */
  assert (mem[0] == 0x950412de && mem[1] == 0);
  (*mt)->num_strings = mem[2];

  apr_mmap_offset ((void **) &((*mt)->original), (*mt)->map, mem[3]);
  apr_mmap_offset ((void **) &((*mt)->translated), (*mt)->map, mem[4]);

  header = (char *) (mem + (*mt)->translated[0].offset);

#if 0
  printf ("%d strings\n", (*mt)->num_strings);

  for (i = 0; i < (*mt)->num_strings; i++)
    {
      printf ("%.*s -> %.*s\n-----------------\n", (*mt)->original[i].len,
	      (char *) (mo + (*mt)->original[i].offset),
	      (*mt)->translated[i].len,
	      (char *) (mo + (*mt)->translated[i].offset));
    }
#endif
  return 0;
}

static const char *
message_table_gettext (message_table_t *mt, const char *msgid)
{
  apr_uint32_t a = 0;
  apr_uint32_t b = mt->num_strings - 1;

  while (1)
    {
      void *addr;
      const char *str;
      int i;
      apr_uint32_t m;

      m = (a + b) / 2;
      apr_mmap_offset (&addr, mt->map, mt->original[m].offset);
      str = addr;
      i = apr_strnatcmp (msgid, str);
      if (i == 0)
        {
          apr_mmap_offset (&addr, mt->map, mt->translated[m].offset);
          return addr;
        }
      if (a == b)
        return msgid;

      if (i < 0)
        b = m - 1;
      else
        a = m + 1;
    }
}

/* ### Perhaps use a thread-local to avoid the need for specifying the
   ### domain and locale, giving us a gettext(char *) API?
   ### http://apr.apache.org/docs/apr/group__apr__thread__proc.html#ga23 */


const char *
svn_intl_dlgettext (const char *domain, const char *locale, const char *msgid)
{
  message_table_t *msg_tbl;
  message_table_key_t key;
  apr_status_t st;

  memset (&key, 0, sizeof (key));
  apr_cpystrn (key.domain, domain, sizeof (key.domain));
  apr_cpystrn (key.locale, locale, sizeof (key.locale));
#if APR_HAS_THREADS
  st = apr_thread_mutex_lock (cache_lock);
  if (st != APR_SUCCESS)
    return msgid;
#endif
  msg_tbl = (message_table_t *) apr_hash_get (cache, &key, sizeof (key));
  if (!msg_tbl)
    {
      st = message_table_open (&msg_tbl, domain, locale, private_pool);
      if (st == APR_SUCCESS)
        {
          apr_hash_set (cache, apr_pmemdup (private_pool, &key, sizeof (key)),
                        sizeof (key), msg_tbl);
        }
      else
        msg_tbl = NULL;
    }
#if APR_HAS_THREADS
  apr_thread_mutex_unlock (cache_lock);
#endif
  if (!msg_tbl)
    return msgid;
  return message_table_gettext (msg_tbl, msgid);
}

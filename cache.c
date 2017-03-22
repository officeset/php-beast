/*
  +----------------------------------------------------------------------+
  | PHP Version 5                                                        |
  +----------------------------------------------------------------------+
  | Copyright (c) 1997-2007 The PHP Group                                |
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.01 of the PHP license,      |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.php.net/license/3_01.txt                                  |
  | If you did not receive a copy of the PHP license and are unable to   |
  | obtain it through the world-wide-web, please send a note to          |
  | license@php.net so we can mail you a copy immediately.               |
  +----------------------------------------------------------------------+
  | Author: Liexusong <280259971@qq.com>                                 |
  +----------------------------------------------------------------------+
*/

#include <stdlib.h>
#include <sys/types.h>
#ifndef PHP_WIN32
#include <sys/mman.h>
#endif

#include "beast_mm.h"
#include "spinlock.h"
#include "php.h"
#include "cache.h"
#include "beast_log.h"

#define BUCKETS_DEFAULT_SIZE 1021

static int beast_cache_initialization = 0;
static cache_item_t **beast_cache_buckets = NULL;
static beast_atomic_t *cache_lock;
static int cache_pid = -1;


void beast_cache_lock()
{
    if (cache_pid == -1) {
        cache_pid = (int)getpid();
    }
    beast_spinlock(cache_lock, cache_pid);
}


void beast_cache_unlock()
{
    if (cache_pid == -1) {
        cache_pid = (int)getpid();
    }
    beast_spinunlock(cache_lock, cache_pid);
}


static inline unsigned int
beast_cache_hash(cache_key_t *key)
{
    unsigned int retval;

    retval = (unsigned int)key->device * 3
           + (unsigned int)key->inode * 7;

    return retval;
}


int beast_cache_init(int size)
{
    int index, bucket_size;
#ifdef PHP_WIN32
	HANDLE hLockMapFile, hBucketsMapFile;
#endif

    if (beast_cache_initialization) {
        return 0;
    }

    if (beast_mm_init(size) == -1) {
        return -1;
    }

    /* init cache lock */
#ifdef PHP_WIN32
	cache_lock = NULL;
	hLockMapFile = CreateFileMapping(INVALID_HANDLE_VALUE,
		NULL, PAGE_READWRITE, 0, sizeof(int), NULL);
	if (hLockMapFile) {
		cache_lock = MapViewOfFile(
			hLockMapFile,
			FILE_MAP_ALL_ACCESS,
			0,
			0,
			sizeof(int)
		);
		CloseHandle(hLockMapFile);
	}
#else
    cache_lock = (int *)mmap(NULL,
                             sizeof(int),
                             PROT_READ|PROT_WRITE,
                             MAP_SHARED|MAP_ANON,
                             -1,
                             0);
#endif
    if (!cache_lock) {
        beast_write_log(beast_log_error,
                        "Unable alloc share memory for cache lock");
        beast_mm_destroy();
        return -1;
    }

    *cache_lock = 0;

    /* init cache buckets's memory */
    bucket_size = sizeof(cache_item_t *) * BUCKETS_DEFAULT_SIZE;

#ifdef PHP_WIN32
	hBucketsMapFile = CreateFileMapping(INVALID_HANDLE_VALUE,
		NULL, PAGE_READWRITE, 0, bucket_size, NULL);
	if (hBucketsMapFile) {
		beast_cache_buckets = (cache_item_t **)MapViewOfFile(
			hBucketsMapFile, 
			FILE_MAP_ALL_ACCESS,
			0,
			0,
			bucket_size
		);
		CloseHandle(hBucketsMapFile);
	}
#else
    beast_cache_buckets = (cache_item_t **)mmap(NULL,
                                                bucket_size,
                                                PROT_READ|PROT_WRITE,
                                                MAP_SHARED|MAP_ANON,
                                                -1,
                                                0);
#endif

    if (!beast_cache_buckets) {
        beast_write_log(beast_log_error,
                        "Unable alloc share memory for cache buckets");
#ifdef PHP_WIN32
		UnmapViewOfFile(cache_lock);
#else
		munmap((void *)cache_lock, sizeof(int));
#endif
        beast_mm_destroy();
        return -1;
    }

    for (index = 0; index < BUCKETS_DEFAULT_SIZE; index++) {
        beast_cache_buckets[index] = NULL;
    }

    beast_cache_initialization = 1;

    return 0;
}


cache_item_t *beast_cache_find(cache_key_t *key)
{
    unsigned int hashval = beast_cache_hash(key);
    unsigned int index = hashval % BUCKETS_DEFAULT_SIZE;
    cache_item_t *item, *temp;

    beast_cache_lock();

    item = beast_cache_buckets[index];
    while (item) {
        if (item->key.device == key->device &&
             item->key.inode == key->inode)
        {
            break;
        }
        item = item->next;
    }

    if (item && item->key.mtime < key->mtime) /* cache exprie */
    {
        temp = beast_cache_buckets[index];
        if (temp == item) { /* the header node */
            beast_cache_buckets[index] = item->next;
        } else {
            while (temp->next != item) { /* find prev node */
                temp = temp->next;
            }
            temp->next = item->next;
        }

        beast_mm_free(item);

        item = NULL;
    }

    beast_cache_unlock();

    return item;
}


cache_item_t *beast_cache_create(cache_key_t *key)
{
    cache_item_t *item, *next;
    int i, msize, bsize;

    msize = sizeof(*item) + key->fsize;
    bsize = sizeof(cache_item_t *) * BUCKETS_DEFAULT_SIZE;

    if ((msize + bsize) > beast_mm_realspace()) {
        beast_write_log(beast_log_error,
                        "Cache item size too big");
        return NULL;
    }

    item = beast_mm_malloc(msize);

    if (!item) {
        beast_write_log(beast_log_notice,
                        "Not enough memory for alloc cache");
        return NULL;
    }

    item->key.device = key->device;
    item->key.inode = key->inode;
    item->key.fsize = key->fsize;
    item->key.mtime = key->mtime;

    item->next = NULL;

    return item;
}


/*
 * Push cache item into cache manager,
 * this function return a cache item,
 * may be return value not equals push item,
 * so we must use return value.
 */
cache_item_t *beast_cache_push(cache_item_t *item)
{
    unsigned int hashval = beast_cache_hash(&item->key);
    unsigned int index = hashval % BUCKETS_DEFAULT_SIZE;
    cache_item_t **this, *self;

    beast_cache_lock();

    item->next = beast_cache_buckets[index];
    beast_cache_buckets[index] = item;

    beast_cache_unlock();

    return item;
}


int beast_cache_destroy()
{
    int index;
    cache_item_t *item, *next;

    if (!beast_cache_initialization) {
        return 0;
    }

    beast_mm_destroy(); /* destroy memory manager */

    /* free cache buckets's mmap memory */
#ifdef PHP_WIN32
	UnmapViewOfFile((void *)cache_lock);
	UnmapViewOfFile((void *)beast_cache_buckets);
#else
    munmap((void *)beast_cache_buckets,
           sizeof(cache_item_t *) * BUCKETS_DEFAULT_SIZE);

    munmap((void *)cache_lock, sizeof(int));
#endif
    beast_cache_initialization = 0;

    return 0;
}


void beast_cache_info(zval *retval)
{
    char key[128];
    int i;
    cache_item_t *item;

    beast_cache_lock();

    for (i = 0; i < BUCKETS_DEFAULT_SIZE; i++) {
        item = beast_cache_buckets[i];
        while (item) {
            sprintf(key, "{device(%d)#inode(%d)}",
                  item->key.device, item->key.inode);
            add_assoc_long(retval, key, item->key.fsize);
            item = item->next;
        }
    }

    beast_cache_unlock();
}

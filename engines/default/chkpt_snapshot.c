/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 * arcus-memcached - Arcus memory cache server
 * Copyright 2019 JaM2in Co., Ltd.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include <assert.h>

#include "default_engine.h"
#ifdef ENABLE_PERSISTENCE
#include "cmdlogmgr.h"

#define SNAPSHOT_BUFFER_SIZE (10 * 1024 * 1024)
#define SCAN_ITEM_ARRAY_SIZE 16
//#define SCAN_ITEM_ARRAY_SIZE 64

/* snapshot file structure */
struct snapshot_file {
    char   path[MAX_FILEPATH_LENGTH];
    int    fd;
    size_t size;
};

/* snapshot buffer structure */
struct snapshot_buffer {
    char       *memory;
    uint32_t    maxlen;
    uint32_t    curlen;
};

/* snapshot main structure */
typedef struct _snapshot_st {
   pthread_mutex_t lock;
   void    *engine;
   bool     running;    /* Is it running, now ? */
   bool     success;    /* snapshot final status: success or fail */
   bool     reqstop;    /* request to stop snapshot */
   enum chkpt_snapshot_mode mode;
   uint64_t snapped;    /* # of cache item snapped */
   time_t   started;    /* snapshot start time */
   time_t   stopped;    /* snapshot stop time */
   char    *prefix;     /* prefix name */
   int      nprefix;    /* prefix name length */
   struct snapshot_file   file;
   struct snapshot_buffer buffer;
   CB_SNAPSHOT_DONE cb_snapshot_done;
   volatile bool initialized;
} snapshot_st;

/* global data */
static struct default_engine *engine = NULL;
static EXTENSION_LOGGER_DESCRIPTOR *logger = NULL;
static snapshot_st snapshot_anch;

static const char *snapshot_mode_string[] = {
    "KEY", "DATA", "CHKPT"
};

static const char *item_type_string[] = {
    "K", "L", "S", "M", "B"
};

/*
 * snapshot functions
 */
typedef struct _snapshot_func {
    /* The dump function called for the given item array.
     */
    int (*dump)(snapshot_st *ss, void **item_array, int item_count, void *args);
    /* The done function called after dumping items.
     * It's usually called for recording the summary.
     */
    int (*done)(snapshot_st *ss);
} SNAPSHOT_FUNC;

static int do_snapshot_key_dump(snapshot_st *ss, void **item_array, int item_count, void *args);
static int do_snapshot_key_done(snapshot_st *ss);
static int do_snapshot_data_dump(snapshot_st *ss, void **item_array, int item_count, void *args);
static int do_snapshot_data_done(snapshot_st *ss);

/* snapshot function array for each snapshot mode */
SNAPSHOT_FUNC snapshot_func[CHKPT_SNAPSHOT_MODE_MAX] = {
    { do_snapshot_key_dump,  do_snapshot_key_done },
    { do_snapshot_data_dump, do_snapshot_data_done },
    { do_snapshot_data_dump, do_snapshot_data_done }
};

/*
 * snapshot buffer functions
 */
static int do_snapshot_buffer_check_space(snapshot_st *ss, int needsize)
{
    struct snapshot_buffer *ssb = &ss->buffer;

    if ((ssb->curlen + needsize) > ssb->maxlen) {
        int nwritten = write(ss->file.fd, ssb->memory, ssb->curlen);
        if (nwritten != ssb->curlen) {
            logger->log(EXTENSION_LOG_WARNING, NULL,
                        "Failed to write the snapshot: nwritten(%d) != request(%d)\n",
                        nwritten, ssb->curlen);
            return -1;
        }
        ssb->curlen = 0;
    }
    return 0;
}

static int do_snapshot_buffer_flush(snapshot_st *ss)
{
    struct snapshot_buffer *ssb = &ss->buffer;

    if (ssb->curlen > 0) {
        int nwritten = write(ss->file.fd, ssb->memory, ssb->curlen);
        if (nwritten != ssb->curlen) {
            logger->log(EXTENSION_LOG_WARNING, NULL,
                        "Failed to write the snapshot: nwritten(%d) != request(%d)\n",
                        nwritten, ssb->curlen);
            return -1;
        }
        ssb->curlen = 0;
    }
    if (1) { /* Assume that some data are written */
        (void)fsync(ss->file.fd);
    }
    return 0;
}

static void do_snapshot_buffer_reset(snapshot_st *ss)
{
    ss->buffer.curlen = 0;
}

/* mode == CHKPT_SNAPSHOT_MODE_KEY
 * dump: do_snapshot_key_dump()
 * done: do_snapshot_key_done()
 */
static int do_snapshot_key_dump(snapshot_st *ss, void **item_array, int item_count, void *args)
{
    struct default_engine *engine = ss->engine;
    hash_item *it;
    struct snapshot_buffer *ssb = &ss->buffer;
    char *bufptr;
    int length;
    int needsize;
    int type;
    rel_time_t curtime = engine->server.core->get_current_time();
    int i, ret = 0;

    /* format: "<type> <key> <exptime>\n"
     * - <type>: "K', "L", "S", "M", "B"
     * - <exptime> : max 20 characters
     */
    needsize = 24; /* except key string */

    for (i = 0; i < item_count; i++) {
        it = (hash_item*)item_array[i];

        if (do_snapshot_buffer_check_space(ss, (needsize + it->nkey)) < 0) {
            ret = -1; break;
        }

        /* record item key info in the snapshot buffer */
        bufptr = &ssb->memory[ssb->curlen];
        /* 1) <type> */
        type = GET_ITEM_TYPE(it);
        memcpy(bufptr, item_type_string[type], 1);
        memcpy(bufptr+1, " ", 1);
        bufptr += 2;
        /* 2) <key> */
        memcpy(bufptr, item_get_key(it), it->nkey);
        bufptr += it->nkey;
        /* 3) <exptime> */
        if (it->exptime == 0) {
            memcpy(bufptr, " 0\n", 3);
            length = 3;
#ifdef ENABLE_STICKY_ITEM
        } else if (it->exptime == (rel_time_t)-1) {
            memcpy(bufptr, " -1\n", 4);
            length = 4;
#endif
        } else {
            uint32_t diff_time = it->exptime > curtime
                               ? it->exptime - curtime : 1;
            length = sprintf(bufptr, " %u\n", diff_time);
        }
        bufptr += length;
        ssb->curlen += (length + it->nkey + 2);
        ss->snapped++;
    }
    return ret;
}

static int do_snapshot_key_done(snapshot_st *ss)
{
    struct snapshot_buffer *ssb = &ss->buffer;
    char *bufptr;
    int   length;
    int needsize;

    needsize = 256; /* just, enough memory space size */
    if (do_snapshot_buffer_check_space(ss, needsize) < 0) {
        return -1;
    }

    /* record snapshot summary in the snapshot buffer */
    bufptr = &ssb->memory[ssb->curlen];
    length = snprintf(bufptr, needsize,
                      "SNAPSHOT SUMMARY: { prefix=%s, count=%"PRIu64", elapsed=%"PRIu64" }\n",
                      ss->nprefix > 0 ? ss->prefix : (ss->nprefix == 0 ? "<null>" : "<all>"),
                      ss->snapped, (uint64_t)(time(NULL) - ss->started));
    ssb->curlen += length;

    if (do_snapshot_buffer_flush(ss) < 0) {
        return -1;
    }
    return 0;
}

/* mode == CHKPT_SNAPSHOT_MODE_DATA || mode == CHKPT_SNAPSHOT_MODE_CHKPT
 * dump: do_snapshot_data_dump()
 * done: do_snapshot_data_done()
 */
static int do_snapshot_data_dump(snapshot_st *ss, void **item_array, int item_count, void *args)
{
    struct snapshot_buffer *ssb = &ss->buffer;
    elems_result_t *erst_array = (elems_result_t *)args;
    hash_item *it;
    char *bufptr;
    int logsize;
    int i, j, ret = 0;

    for (i = 0; i < item_count; i++) {
        it = (hash_item*)item_array[i];

        ITLinkLog log;
        logsize = lrec_construct_link_item((LogRec*)&log, it);
        if (do_snapshot_buffer_check_space(ss, logsize) < 0) {
            ret = -1; break;
        }

        bufptr = &ssb->memory[ssb->curlen];
        lrec_write_to_buffer((LogRec*)&log, bufptr);
        ssb->curlen += logsize;

        if (erst_array != NULL && IS_COLL_ITEM(it)) {
            elems_result_t *eresult = &erst_array[i];
            for (j = 0; j < eresult->elem_count; j++) {
                SnapshotElemLog elog;
                logsize = lrec_construct_snapshot_elem((LogRec*)&elog, it,
                                                       eresult->elem_array[j]);
                if (do_snapshot_buffer_check_space(ss, logsize) < 0) {
                    ret = -1; break;
                }

                bufptr = &ssb->memory[ssb->curlen];
                lrec_write_to_buffer((LogRec*)&elog, bufptr);
                ssb->curlen += logsize;
            }
            if (ret == -1) break;
        }
        ss->snapped++;
    }
    return ret;
}

static int do_snapshot_data_done(snapshot_st *ss)
{
    struct snapshot_buffer *ssb = &ss->buffer;
    char *bufptr;
    int logsize;

    SnapshotDoneLog log;
    logsize = lrec_construct_snapshot_done((LogRec*)&log);
    if (do_snapshot_buffer_check_space(ss, logsize) < 0) {
        return -1;
    }

    /* record snapshot done mark in the end of file. */
    bufptr = &ssb->memory[ssb->curlen];
    lrec_write_to_buffer((LogRec*)&log, bufptr);
    ssb->curlen += logsize;
    if (do_snapshot_buffer_flush(ss) < 0) {
        return -1;
    }
    return 0;
}

static ENGINE_ERROR_CODE do_snapshot_argcheck(enum chkpt_snapshot_mode mode)
{
    /* check snapshot mode */
    if (mode >= CHKPT_SNAPSHOT_MODE_MAX) {
        logger->log(EXTENSION_LOG_WARNING, NULL,
                    "Failed to start snapshot. Given mode(%d) is invalid.\n", (int)mode);
        return ENGINE_EBADVALUE;
    }
    return ENGINE_SUCCESS;
}

static void do_snapshot_prepare(snapshot_st *ss,
                                enum chkpt_snapshot_mode mode,
                                const char *prefix, const int nprefix,
                                const char *filepath,
                                CB_SNAPSHOT_DONE callback)
{
    /* prepare snapshot anchor */
    ss->success = false;
    ss->reqstop = false;
    ss->mode = mode;
    ss->snapped = 0;
    ss->started = time(NULL);
    ss->stopped = 0;
    ss->prefix = (char*)prefix;
    ss->nprefix = nprefix;
    ss->cb_snapshot_done = callback;

    /* prepare snapshot file */
    snprintf(ss->file.path, MAX_FILEPATH_LENGTH, "%s",
             (filepath != NULL ? filepath : "chkpt_snapshot"));
    ss->file.fd = -1;
    ss->file.size = 0;

    /* reset snapshot buffer */
    do_snapshot_buffer_reset(ss);
}

static bool do_snapshot_action(snapshot_st *ss)
{
    CB_SCAN_OPEN    cb_scan_open = NULL;
    CB_SCAN_CLOSE   cb_scan_close = NULL;
    item_scan       scan;
    elems_result_t  eresults[SCAN_ITEM_ARRAY_SIZE];
    elems_result_t *erst_array = NULL;
    void           *item_array[SCAN_ITEM_ARRAY_SIZE];
    int             item_arrsz = SCAN_ITEM_ARRAY_SIZE;
    int             item_count = 0;
    bool            snapshot_done = false;

    ss->file.fd = open(ss->file.path, O_WRONLY | O_CREAT | O_TRUNC,
                                       S_IRUSR | S_IWUSR | S_IRGRP);
    if (ss->file.fd < 0) {
        logger->log(EXTENSION_LOG_WARNING, NULL,
                    "Failed to open the snapshot file. path=%s err=%s\n",
                    ss->file.path, strerror(errno));
        goto done;
    }

    if (ss->mode == CHKPT_SNAPSHOT_MODE_DATA || ss->mode == CHKPT_SNAPSHOT_MODE_CHKPT) {
        for (int i = 0; i < SCAN_ITEM_ARRAY_SIZE; i++) {
            (void)coll_elem_result_init(&eresults[i], 0);
        }
        erst_array = eresults;
    }

    if (ss->mode == CHKPT_SNAPSHOT_MODE_CHKPT) {
        cb_scan_open = &cmdlog_set_chkpt_scan;
        cb_scan_close = &cmdlog_reset_chkpt_scan;
    }
    item_scan_open(&scan, ss->prefix, ss->nprefix, cb_scan_open);
    while (1) {
        if (ss->reqstop) {
            logger->log(EXTENSION_LOG_INFO, NULL, "Ongoing snapshot recognized stop request.\n");
            break;
        }
        item_count = item_scan_getnext(&scan, item_array, erst_array, item_arrsz);
        if (item_count == -2) { /* FIXME: rethink itscan_getnext() interface */
            /* OUT OF MEMORY */
            logger->log(EXTENSION_LOG_WARNING, NULL,
                        "The item scan function has failed by out of memory.\n");
            break;
        }
        if (item_count < 0) { /* reached to the end */
            if (snapshot_func[ss->mode].done(ss) < 0) {
                logger->log(EXTENSION_LOG_WARNING, NULL,
                            "The snapshot done function has failed.\n");
            } else {
                snapshot_done = true;
            }
            break;
        }
        if (item_count > 0) {
            int ret = snapshot_func[ss->mode].dump(ss, item_array, item_count, erst_array);
            item_scan_release(&scan, item_array, erst_array, item_count);
            if (ret < 0) {
                logger->log(EXTENSION_LOG_WARNING, NULL,
                            "The snapshot dump function has failed.\n");
                break;
            }
        }
        /* item_count == 0: No valid items found.
         * We continue the scan.
         */
    }
    item_scan_close(&scan, cb_scan_close, snapshot_done);

done:
    if (erst_array != NULL) {
        for (int i = 0; i < SCAN_ITEM_ARRAY_SIZE; i++) {
            (void)coll_elem_result_free(&erst_array[i]);
        }
        erst_array = NULL;
    }
    if (ss->file.fd > 0) {
        ss->file.size = lseek(ss->file.fd, 0, SEEK_END);
        close(ss->file.fd);
        ss->file.fd = -1;
    }
    ss->success = snapshot_done;
    ss->stopped = time(NULL);
    return snapshot_done;
}

static ENGINE_ERROR_CODE do_snapshot_direct(snapshot_st *ss,
                                            enum chkpt_snapshot_mode mode,
                                            const char *prefix, const int nprefix,
                                            const char *filepath)
{
    ENGINE_ERROR_CODE ret;

    if (ss->running) {
        logger->log(EXTENSION_LOG_INFO, NULL,
                    "Failed to start snapshot. Already started.\n");
        return ENGINE_FAILED;
    }

    do_snapshot_prepare(ss, mode, prefix, nprefix, filepath, NULL);

    ss->running = true;
    pthread_mutex_unlock(&ss->lock);

    if (do_snapshot_action(ss) == true) {
        logger->log(EXTENSION_LOG_INFO, NULL, "Done the snapshot action.\n");
        ret = ENGINE_SUCCESS;
    } else {
        logger->log(EXTENSION_LOG_INFO, NULL, "Failed to do snapshot action\n");
        ret = ENGINE_FAILED;
    }

    pthread_mutex_lock(&ss->lock);
    ss->running = false;

    return ret;
}

static void *do_snapshot_thread_main(void *arg)
{
    snapshot_st *ss = (snapshot_st *)arg;
    assert(ss->running == true);

    if (do_snapshot_action(ss) == true) {
        logger->log(EXTENSION_LOG_INFO, NULL,
                    "The snapshot thread has done the snapshot action.\n");
    } else {
        logger->log(EXTENSION_LOG_INFO, NULL,
                    "The snapshot thread has failed to do snapshot action.\n");
    }

    pthread_mutex_lock(&ss->lock);
    ss->running = false;
    pthread_mutex_unlock(&ss->lock);
    if (ss->cb_snapshot_done) {
        /* perform the registered callback function */
        ss->cb_snapshot_done(ss->engine);
    }
    return NULL;
}

static ENGINE_ERROR_CODE do_snapshot_start(snapshot_st *ss,
                                           enum chkpt_snapshot_mode mode,
                                           const char *prefix, const int nprefix,
                                           const char *filepath,
                                           CB_SNAPSHOT_DONE callback)
{
    pthread_t tid;
    pthread_attr_t attr;

    if (ss->running) {
        logger->log(EXTENSION_LOG_INFO, NULL,
                    "Failed to start snapshot. Already started.\n");
        return ENGINE_FAILED;
    }

    do_snapshot_prepare(ss, mode, prefix, nprefix, filepath, callback);

    /* start the snapshot thread */
    ss->running = true;

    if (pthread_attr_init(&attr) != 0 ||
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED) != 0 ||
        pthread_create(&tid, &attr, do_snapshot_thread_main, ss) != 0)
    {
        logger->log(EXTENSION_LOG_WARNING, NULL,
                    "Failed to create the snapshot thread. err=%s\n",
                    strerror(errno));
        ss->running = false;
        return ENGINE_FAILED;
    }

    return ENGINE_SUCCESS;
}

static void do_snapshot_stop(snapshot_st *ss, bool wait_stop)
{
    if (!ss->running || ss->mode == CHKPT_SNAPSHOT_MODE_CHKPT) {
        return;
    }

    while (ss->running) {
        ss->reqstop = true; /* request to stop the snapshot */

        if (wait_stop) {
            pthread_mutex_unlock(&ss->lock);
            usleep(1000); /* sleep 1 ms */
            pthread_mutex_lock(&ss->lock);
        } else {
            break;
        }
    }
    logger->log(EXTENSION_LOG_INFO, NULL, "Snapshot thread stopped.\n");
}

static void do_snapshot_stats(snapshot_st *ss, ADD_STAT add_stat, const void *cookie)
{
    char val[256];
    int  len;

    if (ss->running) {
        add_stat("snapshot:status", 15, "running", 7, cookie);
    } else {
        add_stat("snapshot:status", 15, "stopped", 7, cookie);

        len = sprintf(val, "%s", (ss->success ? "true" : "false"));
        add_stat("snapshot:success", 16, val, len, cookie);
    }

    if (ss->started != 0) {
        const char *modestr = snapshot_mode_string[ss->mode];
        add_stat("snapshot:mode", 13, modestr, strlen(modestr), cookie);
        if (ss->stopped != 0) {
            time_t diff = ss->stopped - ss->started;
            len = sprintf(val, "%"PRIu64, (uint64_t)diff);
            add_stat("snapshot:last_run", 17, val, len, cookie);
        }
        len = sprintf(val, "%"PRIu64, ss->snapped);
        add_stat("snapshot:snapped", 16, val, len, cookie);
        len = sprintf(val, "%s", (ss->nprefix > 0 ? ss->prefix :
                                  (ss->nprefix == 0 ? "<null>" : "<all>")));
        add_stat("snapshot:prefix", 15, val, len, cookie);
        if (strlen(ss->file.path) > 0) {
            len = sprintf(val, "%s", ss->file.path);
            add_stat("snapshot:filepath", 17, val, len, cookie);
        }
    }
}

/*
 * External Functions
 */
ENGINE_ERROR_CODE chkpt_snapshot_init(void *engine_ptr)
{
    snapshot_st *ss = &snapshot_anch;

    engine = (struct default_engine *)engine_ptr;
    logger = engine->server.log->get_logger();

    pthread_mutex_init(&ss->lock, NULL);
    ss->engine = (void*)engine;
    ss->running = false;
    ss->success = false;
    ss->reqstop = false;
    ss->mode = CHKPT_SNAPSHOT_MODE_MAX;
    ss->snapped = 0;
    ss->started = 0;
    ss->stopped = 0;

    /* snapshot prefix */
    ss->prefix = NULL;
    ss->nprefix = -1;

    /* snapshot file */
    ss->file.path[0] = '\0';
    ss->file.fd = -1;

    /* snapshot buffer */
    ss->buffer.memory = (char*)malloc(SNAPSHOT_BUFFER_SIZE);
    if (ss->buffer.memory == NULL) {
        logger->log(EXTENSION_LOG_INFO, NULL,
                    "Failed to allocate snapshot buffer.\n");
        return ENGINE_FAILED;
    }
    ss->buffer.maxlen = SNAPSHOT_BUFFER_SIZE;
    ss->buffer.curlen = 0;

    ss->initialized = true;
    logger->log(EXTENSION_LOG_INFO, NULL, "SNAPSHOT module initialized.\n");

    return ENGINE_SUCCESS;
}

void chkpt_snapshot_final(void)
{
    snapshot_st *ss = &snapshot_anch;

    if (ss->initialized == false) {
        return;
    }

    /* stop the current snapshot */
    chkpt_snapshot_stop();

    if (ss->file.fd != -1) {
        close(ss->file.fd);
        ss->file.fd = -1;
    }
    if (ss->buffer.memory != NULL) {
        free(ss->buffer.memory);
        ss->buffer.memory = NULL;
    }
    pthread_mutex_destroy(&ss->lock);

    ss->initialized = false;
    logger->log(EXTENSION_LOG_INFO, NULL, "SNAPSHOT module destroyed.\n");
}

ENGINE_ERROR_CODE chkpt_snapshot_direct(enum chkpt_snapshot_mode mode,
                                        const char *prefix, const int nprefix,
                                        const char *filepath, size_t *filesize)
{
    ENGINE_ERROR_CODE ret;

    ret = do_snapshot_argcheck(mode);
    if (ret != ENGINE_SUCCESS) {
        return ret;
    }

    pthread_mutex_lock(&snapshot_anch.lock);
    ret = do_snapshot_direct(&snapshot_anch, mode, prefix, nprefix, filepath);
    if (ret == ENGINE_SUCCESS && filesize != NULL) {
        *filesize = snapshot_anch.file.size;
    }
    pthread_mutex_unlock(&snapshot_anch.lock);
    return ret;
}

ENGINE_ERROR_CODE chkpt_snapshot_start(enum chkpt_snapshot_mode mode,
                                       const char *prefix, const int nprefix,
                                       const char *filepath,
                                       CB_SNAPSHOT_DONE callback)
{
    ENGINE_ERROR_CODE ret;

    ret = do_snapshot_argcheck(mode);
    if (ret != ENGINE_SUCCESS) {
        return ret;
    }

    pthread_mutex_lock(&snapshot_anch.lock);
    ret = do_snapshot_start(&snapshot_anch, mode, prefix, nprefix,
                            filepath, callback);
    pthread_mutex_unlock(&snapshot_anch.lock);
    return ret;
}

void chkpt_snapshot_stop(void)
{
    pthread_mutex_lock(&snapshot_anch.lock);
    do_snapshot_stop(&snapshot_anch, true);
    pthread_mutex_unlock(&snapshot_anch.lock);
}

void chkpt_snapshot_stats(ADD_STAT add_stat, const void *cookie)
{
    pthread_mutex_lock(&snapshot_anch.lock);
    do_snapshot_stats(&snapshot_anch, add_stat, cookie);
    pthread_mutex_unlock(&snapshot_anch.lock);
}

/* Check snapshot file validity by inspecting SnapshotDone log record. */
int chkpt_snapshot_check_file_validity(const int fd, size_t *filesize)
{
    SnapshotDoneLog log;
    off_t offset;
    ssize_t nread;

    assert(fd > 0);

    offset = lseek(fd, -sizeof(log), SEEK_END);
    if (offset < 0) {
        return -1;
    }
    nread = read(fd, &log, sizeof(log));
    if (nread != sizeof(log)) {
        return -1;
    }
    lseek(fd, 0, SEEK_SET);

    *filesize = offset + sizeof(log);
    return lrec_check_snapshot_done(&log);
}

int chkpt_snapshot_file_apply(const char *filepath)
{
    logger->log(EXTENSION_LOG_INFO, NULL,
                "[RECOVERY - SNAPSHOT] applying snapshot file. path=%s\n", filepath);

    int fd = open(filepath, O_RDONLY);
    if (fd < 0) {
        logger->log(EXTENSION_LOG_WARNING, NULL,
                    "[RECOVERY - SNAPSHOT] failed : file open. "
                    "path=%s, error=%s\n", filepath, strerror(errno));
        return -1;
    }

    struct default_engine *engine = (struct default_engine*)snapshot_anch.engine;
    int ret = 0;
    ENGINE_ERROR_CODE err;
    hash_item *last_coll_it = NULL;
    char buf[MAX_LOG_RECORD_SIZE];
    LogRec *logrec = (LogRec*)buf;
    LogHdr *loghdr = &logrec->header;

    while (engine->initialized) {
        ssize_t nread = read(fd, loghdr, sizeof(LogHdr));
        if (nread != sizeof(LogHdr)) {
            logger->log(EXTENSION_LOG_WARNING, NULL,
                        "[RECOVERY - SNAPSHOT] failed : read header data "
                        "nread(%zd) != header_length(%lu).\n", nread, sizeof(LogHdr));
            ret = -1; break;
        }

        if (loghdr->body_length > 0) {
            int max_body_length = MAX_LOG_RECORD_SIZE - sizeof(LogHdr);
            if (max_body_length < loghdr->body_length) {
                logger->log(EXTENSION_LOG_WARNING, NULL,
                            "[RECOVERY - SNAPSHOT] failed : body length is abnormally too big "
                            "max_body_length(%d) < body_length(%u).\n",
                            max_body_length, loghdr->body_length);
                ret = -1; break;
            }
            logrec->body = buf + sizeof(LogHdr);
            nread = read(fd, logrec->body, loghdr->body_length);
            if (nread != loghdr->body_length) {
                logger->log(EXTENSION_LOG_WARNING, NULL,
                            "[RECOVERY - SNAPSHOT] failed : read body data "
                            "nread(%zd) != body_length(%u).\n", nread, loghdr->body_length);
                ret = -1; break;
            }
        }

        if (loghdr->logtype == LOG_IT_LINK) {
            err = lrec_redo_from_record(logrec);
            if (err != ENGINE_SUCCESS) {
                logger->log(EXTENSION_LOG_WARNING, NULL,
                            "[RECOVERY - SNAPSHOT] warning : item link log record redo failed.\n");
                if (err == ENGINE_ENOMEM) {
                    logger->log(EXTENSION_LOG_WARNING, NULL,
                                "[RECOVERY - SNAPSHOT] failed : out of memory.\n");
                    ret = -1; break;
                }
            }
            if (last_coll_it != NULL) {
                item_release(last_coll_it);
            }
            last_coll_it = lrec_get_item_if_collection_link((ITLinkLog*)logrec);
        } else if (loghdr->logtype == LOG_SNAPSHOT_ELEM && last_coll_it) {
            assert(IS_COLL_ITEM(last_coll_it));
            lrec_set_item_in_snapshot_elem((SnapshotElemLog*)logrec, last_coll_it);
            err = lrec_redo_from_record(logrec);
            if (err != ENGINE_SUCCESS) {
                logger->log(EXTENSION_LOG_WARNING, NULL,
                            "[RECOVERY - SNAPSHOT] warning : snapshot elem log record redo failed.\n");
                if (err == ENGINE_ENOMEM) {
                    logger->log(EXTENSION_LOG_WARNING, NULL,
                                "[RECOVERY - SNAPSHOT] failed : out of memory.\n");
                    ret = -1; break;
                }
            }
        } else if (loghdr->logtype == LOG_SNAPSHOT_DONE) {
            if (last_coll_it != NULL) {
                item_release(last_coll_it);
            }
            logger->log(EXTENSION_LOG_INFO, NULL, "[RECOVERY - SNAPSHOT] success.\n");
            break;
        }
    }

    close(fd);
    return ret;
}

#endif

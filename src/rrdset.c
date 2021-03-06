#define NETDATA_RRD_INTERNALS 1
#include "common.h"

#define RRD_DEFAULT_GAP_INTERPOLATIONS 1

void rrdset_check_rdlock_int(RRDSET *st, const char *file, const char *function, const unsigned long line) {
    debug(D_RRD_CALLS, "Checking read lock on chart '%s'", st->id);

    int ret = pthread_rwlock_trywrlock(&st->rrdset_rwlock);
    if(ret == 0)
        fatal("RRDSET '%s' should be read-locked, but it is not, at function %s() at line %lu of file '%s'", st->id, function, line, file);
}

void rrdset_check_wrlock_int(RRDSET *st, const char *file, const char *function, const unsigned long line) {
    debug(D_RRD_CALLS, "Checking write lock on chart '%s'", st->id);

    int ret = pthread_rwlock_tryrdlock(&st->rrdset_rwlock);
    if(ret == 0)
        fatal("RRDSET '%s' should be write-locked, but it is not, at function %s() at line %lu of file '%s'", st->id, function, line, file);
}


// ----------------------------------------------------------------------------
// RRDSET index

int rrdset_compare(void* a, void* b) {
    if(((RRDSET *)a)->hash < ((RRDSET *)b)->hash) return -1;
    else if(((RRDSET *)a)->hash > ((RRDSET *)b)->hash) return 1;
    else return strcmp(((RRDSET *)a)->id, ((RRDSET *)b)->id);
}

static RRDSET *rrdset_index_find(RRDHOST *host, const char *id, uint32_t hash) {
    RRDSET tmp;
    strncpyz(tmp.id, id, RRD_ID_LENGTH_MAX);
    tmp.hash = (hash)?hash:simple_hash(tmp.id);

    return (RRDSET *)avl_search_lock(&(host->rrdset_root_index), (avl *) &tmp);
}

// ----------------------------------------------------------------------------
// RRDSET name index

#define rrdset_from_avlname(avlname_ptr) ((RRDSET *)((avlname_ptr) - offsetof(RRDSET, avlname)))

int rrdset_compare_name(void* a, void* b) {
    RRDSET *A = rrdset_from_avlname(a);
    RRDSET *B = rrdset_from_avlname(b);

    // fprintf(stderr, "COMPARING: %s with %s\n", A->name, B->name);

    if(A->hash_name < B->hash_name) return -1;
    else if(A->hash_name > B->hash_name) return 1;
    else return strcmp(A->name, B->name);
}

RRDSET *rrdset_index_add_name(RRDHOST *host, RRDSET *st) {
    void *result;
    // fprintf(stderr, "ADDING: %s (name: %s)\n", st->id, st->name);
    result = avl_insert_lock(&host->rrdset_root_index_name, (avl *) (&st->avlname));
    if(result) return rrdset_from_avlname(result);
    return NULL;
}

RRDSET *rrdset_index_del_name(RRDHOST *host, RRDSET *st) {
    void *result;
    // fprintf(stderr, "DELETING: %s (name: %s)\n", st->id, st->name);
    result = (RRDSET *)avl_remove_lock(&((host)->rrdset_root_index_name), (avl *)(&st->avlname));
    if(result) return rrdset_from_avlname(result);
    return NULL;
}


// ----------------------------------------------------------------------------
// RRDSET - find charts

static inline RRDSET *rrdset_index_find_name(RRDHOST *host, const char *name, uint32_t hash) {
    void *result = NULL;
    RRDSET tmp;
    tmp.name = name;
    tmp.hash_name = (hash)?hash:simple_hash(tmp.name);

    // fprintf(stderr, "SEARCHING: %s\n", name);
    result = avl_search_lock(&host->rrdset_root_index_name, (avl *) (&(tmp.avlname)));
    if(result) {
        RRDSET *st = rrdset_from_avlname(result);
        if(strcmp(st->magic, RRDSET_MAGIC))
            error("Search for RRDSET %s returned an invalid RRDSET %s (name %s)", name, st->id, st->name);

        // fprintf(stderr, "FOUND: %s\n", name);
        return rrdset_from_avlname(result);
    }
    // fprintf(stderr, "NOT FOUND: %s\n", name);
    return NULL;
}

inline RRDSET *rrdset_find(RRDHOST *host, const char *id) {
    debug(D_RRD_CALLS, "rrdset_find() for chart '%s' in host '%s'", id, host->hostname);
    RRDSET *st = rrdset_index_find(host, id, 0);
    return(st);
}

inline RRDSET *rrdset_find_bytype(RRDHOST *host, const char *type, const char *id) {
    debug(D_RRD_CALLS, "rrdset_find_bytype() for chart '%s.%s' in host '%s'", type, id, host->hostname);

    char buf[RRD_ID_LENGTH_MAX + 1];
    strncpyz(buf, type, RRD_ID_LENGTH_MAX - 1);
    strcat(buf, ".");
    int len = (int) strlen(buf);
    strncpyz(&buf[len], id, (size_t) (RRD_ID_LENGTH_MAX - len));

    return(rrdset_find(host, buf));
}

inline RRDSET *rrdset_find_byname(RRDHOST *host, const char *name) {
    debug(D_RRD_CALLS, "rrdset_find_byname() for chart '%s' in host '%s'", name, host->hostname);
    RRDSET *st = rrdset_index_find_name(host, name, 0);
    return(st);
}

// ----------------------------------------------------------------------------
// RRDSET - rename charts

char *rrdset_strncpyz_name(char *to, const char *from, size_t length) {
    char c, *p = to;

    while (length-- && (c = *from++)) {
        if(c != '.' && !isalnum(c))
            c = '_';

        *p++ = c;
    }

    *p = '\0';

    return to;
}

void rrdset_set_name(RRDSET *st, const char *name) {
    if(unlikely(st->name && !strcmp(st->name, name)))
        return;

    debug(D_RRD_CALLS, "rrdset_set_name() old: %s, new: %s", st->name, name);

    char b[CONFIG_MAX_VALUE + 1];
    char n[RRD_ID_LENGTH_MAX + 1];

    snprintfz(n, RRD_ID_LENGTH_MAX, "%s.%s", st->type, name);
    rrdset_strncpyz_name(b, n, CONFIG_MAX_VALUE);

    if(st->name) {
        rrdset_index_del_name(st->rrdhost, st);
        st->name = config_set_default(st->config_section, "name", b);
        st->hash_name = simple_hash(st->name);
        rrdsetvar_rename_all(st);
    }
    else {
        st->name = config_get(st->config_section, "name", b);
        st->hash_name = simple_hash(st->name);
    }

    rrdset_wrlock(st);
    RRDDIM *rd;
    rrddim_foreach_write(rd, st)
        rrddimvar_rename_all(rd);
    rrdset_unlock(st);

    if(unlikely(rrdset_index_add_name(st->rrdhost, st) != st))
        error("RRDSET: INTERNAL ERROR: attempted to index duplicate chart name '%s'", st->name);
}


// ----------------------------------------------------------------------------
// RRDSET - reset a chart

void rrdset_reset(RRDSET *st) {
    debug(D_RRD_CALLS, "rrdset_reset() %s", st->name);

    st->last_collected_time.tv_sec = 0;
    st->last_collected_time.tv_usec = 0;
    st->last_updated.tv_sec = 0;
    st->last_updated.tv_usec = 0;
    st->current_entry = 0;
    st->counter = 0;
    st->counter_done = 0;

    RRDDIM *rd;
    rrddim_foreach_read(rd, st) {
        rd->last_collected_time.tv_sec = 0;
        rd->last_collected_time.tv_usec = 0;
        rd->collections_counter = 0;
        memset(rd->values, 0, rd->entries * sizeof(storage_number));
    }
}

// ----------------------------------------------------------------------------
// RRDSET - helpers for rrdset_create()

inline long align_entries_to_pagesize(RRD_MEMORY_MODE mode, long entries) {
    if(unlikely(entries < 5)) entries = 5;
    if(unlikely(entries > RRD_HISTORY_ENTRIES_MAX)) entries = RRD_HISTORY_ENTRIES_MAX;

    if(unlikely(mode == RRD_MEMORY_MODE_NONE || mode == RRD_MEMORY_MODE_RAM))
        return entries;

    long page = (size_t)sysconf(_SC_PAGESIZE);
    long size = sizeof(RRDDIM) + entries * sizeof(storage_number);
    if(unlikely(size % page)) {
        size -= (size % page);
        size += page;

        long n = (size - sizeof(RRDDIM)) / sizeof(storage_number);
        return n;
    }

    return entries;
}

static inline void last_collected_time_align(struct timeval *tv, int update_every) {
    tv->tv_sec -= tv->tv_sec % update_every;
    tv->tv_usec = 500000;
}

static inline void last_updated_time_align(struct timeval *tv, int update_every) {
    tv->tv_sec -= tv->tv_sec % update_every;
    tv->tv_usec = 0;
}

// ----------------------------------------------------------------------------
// RRDSET - free a chart

void rrdset_free(RRDSET *st) {
    if(unlikely(!st)) return;

    rrdhost_check_wrlock(st->rrdhost);  // make sure we have a write lock on the host
    rrdset_wrlock(st);                  // lock this RRDSET

    // ------------------------------------------------------------------------
    // free its children structures

    while(st->variables)  rrdsetvar_free(st->variables);
    while(st->alarms)     rrdsetcalc_unlink(st->alarms);
    while(st->dimensions) rrddim_free(st, st->dimensions);

    rrdfamily_free(st->rrdhost, st->rrdfamily);

    // ------------------------------------------------------------------------
    // remove it from the indexes

    if(unlikely(rrdset_index_del(st->rrdhost, st) != st))
        error("RRDSET: INTERNAL ERROR: attempt to remove from index chart '%s', removed a different chart.", st->id);

    rrdset_index_del_name(st->rrdhost, st);

    // ------------------------------------------------------------------------
    // unlink it from the host

    if(st == st->rrdhost->rrdset_root) {
        st->rrdhost->rrdset_root = st->next;
    }
    else {
        // find the previous one
        RRDSET *s;
        for(s = st->rrdhost->rrdset_root; s && s->next != st ; s = s->next) ;

        // bypass it
        if(s) s->next = st->next;
        else error("Request to free RRDSET '%s': cannot find it under host '%s'", st->id, st->rrdhost->hostname);
    }

    rrdset_unlock(st);

    // ------------------------------------------------------------------------
    // free it

    // free directly allocated members
    freez(st->config_section);

    if(st->rrd_memory_mode == RRD_MEMORY_MODE_SAVE || st->rrd_memory_mode == RRD_MEMORY_MODE_MAP) {
        debug(D_RRD_CALLS, "Unmapping stats '%s'.", st->name);
        munmap(st, st->memsize);
    }
    else
        freez(st);
}

// ----------------------------------------------------------------------------
// RRDSET - create a chart

RRDSET *rrdset_create(RRDHOST *host, const char *type, const char *id, const char *name, const char *family
                      , const char *context, const char *title, const char *units, long priority
                      , int update_every, RRDSET_TYPE chart_type) {

    if(!type || !type[0]) {
        fatal("Cannot create rrd stats without a type.");
        return NULL;
    }

    if(!id || !id[0]) {
        fatal("Cannot create rrd stats without an id.");
        return NULL;
    }

    // ------------------------------------------------------------------------
    // check if it already exists

    char fullid[RRD_ID_LENGTH_MAX + 1];
    snprintfz(fullid, RRD_ID_LENGTH_MAX, "%s.%s", type, id);

    RRDSET *st = rrdset_find(host, fullid);
    if(st) {
        debug(D_RRD_CALLS, "RRDSET '%s', already exists.", fullid);
        return st;
    }

    char fullfilename[FILENAME_MAX + 1];

    // ------------------------------------------------------------------------
    // compose the config_section for this chart

    char config_section[RRD_ID_LENGTH_MAX + 1];
    if(host == localhost)
        strcpy(config_section, fullid);
    else
        snprintfz(config_section, RRD_ID_LENGTH_MAX, "%s/%s", host->machine_guid, fullid);

    // ------------------------------------------------------------------------
    // get the options from the config, we need to create it

    long rentries = config_get_number(config_section, "history", host->rrd_history_entries);
    long entries = align_entries_to_pagesize(host->rrd_memory_mode, rentries);
    if(entries != rentries) entries = config_set_number(config_section, "history", entries);

    if(host->rrd_memory_mode == RRD_MEMORY_MODE_NONE && entries != rentries)
        entries = config_set_number(config_section, "history", 10);

    int enabled = config_get_boolean(config_section, "enabled", 1);
    if(!enabled) entries = 5;

    unsigned long size = sizeof(RRDSET);
    char *cache_dir = rrdset_cache_dir(host, fullid, config_section);

    time_t now = now_realtime_sec();

    // ------------------------------------------------------------------------
    // load it or allocate it

    debug(D_RRD_CALLS, "Creating RRD_STATS for '%s.%s'.", type, id);

    snprintfz(fullfilename, FILENAME_MAX, "%s/main.db", cache_dir);
    if(host->rrd_memory_mode == RRD_MEMORY_MODE_SAVE || host->rrd_memory_mode == RRD_MEMORY_MODE_MAP) {
        st = (RRDSET *) mymmap(fullfilename, size, ((host->rrd_memory_mode == RRD_MEMORY_MODE_MAP) ? MAP_SHARED : MAP_PRIVATE), 0);
        if(st) {
            memset(&st->avl, 0, sizeof(avl));
            memset(&st->avlname, 0, sizeof(avl));
            memset(&st->variables_root_index, 0, sizeof(avl_tree_lock));
            memset(&st->dimensions_index, 0, sizeof(avl_tree_lock));
            memset(&st->rrdset_rwlock, 0, sizeof(pthread_rwlock_t));

            st->name = NULL;
            st->type = NULL;
            st->family = NULL;
            st->context = NULL;
            st->title = NULL;
            st->units = NULL;
            st->dimensions = NULL;
            st->next = NULL;
            st->variables = NULL;
            st->alarms = NULL;
            st->flags = 0x00000000;

            if(strcmp(st->magic, RRDSET_MAGIC) != 0) {
                errno = 0;
                info("Initializing file %s.", fullfilename);
                memset(st, 0, size);
            }
            else if(strcmp(st->id, fullid) != 0) {
                errno = 0;
                error("File %s contents are not for chart %s. Clearing it.", fullfilename, fullid);
                // munmap(st, size);
                // st = NULL;
                memset(st, 0, size);
            }
            else if(st->memsize != size || st->entries != entries) {
                errno = 0;
                error("File %s does not have the desired size. Clearing it.", fullfilename);
                memset(st, 0, size);
            }
            else if(st->update_every != update_every) {
                errno = 0;
                error("File %s does not have the desired update frequency. Clearing it.", fullfilename);
                memset(st, 0, size);
            }
            else if((now - st->last_updated.tv_sec) > update_every * entries) {
                errno = 0;
                error("File %s is too old. Clearing it.", fullfilename);
                memset(st, 0, size);
            }
            else if(st->last_updated.tv_sec > now + update_every) {
                errno = 0;
                error("File %s refers to the future. Clearing it.", fullfilename);
                memset(st, 0, size);
            }

            // make sure the database is aligned
            if(st->last_updated.tv_sec)
                last_updated_time_align(&st->last_updated, update_every);


            // make sure we have the right memory mode
            // even if we cleared the memory
            st->rrd_memory_mode = host->rrd_memory_mode;
        }
    }

    if(unlikely(!st)) {
        st = callocz(1, size);
        st->rrd_memory_mode = (host->rrd_memory_mode == RRD_MEMORY_MODE_NONE) ? RRD_MEMORY_MODE_NONE : RRD_MEMORY_MODE_RAM;
    }

    st->config_section = strdup(config_section);
    st->rrdhost = host;
    st->memsize = size;
    st->entries = entries;
    st->update_every = update_every;

    if(st->current_entry >= st->entries) st->current_entry = 0;

    strcpy(st->cache_filename, fullfilename);
    strcpy(st->magic, RRDSET_MAGIC);

    strcpy(st->id, fullid);
    st->hash = simple_hash(st->id);

    st->cache_dir = cache_dir;

    st->chart_type = rrdset_type_id(config_get(st->config_section, "chart type", rrdset_type_name(chart_type)));
    st->type       = config_get(st->config_section, "type", type);
    st->family     = config_get(st->config_section, "family", family?family:st->type);
    st->units      = config_get(st->config_section, "units", units?units:"");

    st->context    = config_get(st->config_section, "context", context?context:st->id);
    st->hash_context = simple_hash(st->context);

    st->priority = config_get_number(st->config_section, "priority", priority);
    if(enabled)
        rrdset_flag_set(st, RRDSET_FLAG_ENABLED);
    else
        rrdset_flag_clear(st, RRDSET_FLAG_ENABLED);

    rrdset_flag_clear(st, RRDSET_FLAG_DETAIL);
    rrdset_flag_clear(st, RRDSET_FLAG_DEBUG);

    // if(!strcmp(st->id, "disk_util.dm-0")) {
    //     st->debug = 1;
    //     error("enabled debugging for '%s'", st->id);
    // }
    // else error("not enabled debugging for '%s'", st->id);

    st->green = NAN;
    st->red = NAN;

    st->last_collected_time.tv_sec = 0;
    st->last_collected_time.tv_usec = 0;
    st->counter_done = 0;

    st->gap_when_lost_iterations_above = (int) (
            config_get_number(st->config_section, "gap when lost iterations above", RRD_DEFAULT_GAP_INTERPOLATIONS) + 2);

    avl_init_lock(&st->dimensions_index, rrddim_compare);
    avl_init_lock(&st->variables_root_index, rrdvar_compare);

    pthread_rwlock_init(&st->rrdset_rwlock, NULL);
    rrdhost_wrlock(host);

    if(name && *name) rrdset_set_name(st, name);
    else rrdset_set_name(st, id);

    {
        char varvalue[CONFIG_MAX_VALUE + 1];
        char varvalue2[CONFIG_MAX_VALUE + 1];
        snprintfz(varvalue, CONFIG_MAX_VALUE, "%s (%s)", title?title:"", st->name);
        json_escape_string(varvalue2, varvalue, sizeof(varvalue2));
        st->title = config_get(st->config_section, "title", varvalue2);
    }

    st->rrdfamily = rrdfamily_create(host, st->family);

    st->next = host->rrdset_root;
    host->rrdset_root = st;

    if(host->health_enabled) {
        rrdsetvar_create(st, "last_collected_t", RRDVAR_TYPE_TIME_T, &st->last_collected_time.tv_sec, 0);
        rrdsetvar_create(st, "collected_total_raw", RRDVAR_TYPE_TOTAL, &st->last_collected_total, 0);
        rrdsetvar_create(st, "green", RRDVAR_TYPE_CALCULATED, &st->green, 0);
        rrdsetvar_create(st, "red", RRDVAR_TYPE_CALCULATED, &st->red, 0);
        rrdsetvar_create(st, "update_every", RRDVAR_TYPE_INT, &st->update_every, 0);
    }

    if(unlikely(rrdset_index_add(host, st) != st))
        error("RRDSET: INTERNAL ERROR: attempt to index duplicate chart '%s'", st->id);

    rrdsetcalc_link_matching(st);
    rrdcalctemplate_link_matching(st);

    rrdhost_unlock(host);

    return(st);
}


// ----------------------------------------------------------------------------
// RRDSET - data collection iteration control

inline void rrdset_next_usec_unfiltered(RRDSET *st, usec_t microseconds) {

    if(unlikely(!st->last_collected_time.tv_sec)) {
        // the first entry
        microseconds = st->update_every * USEC_PER_SEC;
    }
    else if(unlikely(!microseconds)) {
        // no dt given by the plugin
        struct timeval now;
        now_realtime_timeval(&now);
        microseconds = dt_usec(&now, &st->last_collected_time);
    }

    st->usec_since_last_update = microseconds;
}

inline void rrdset_next_usec(RRDSET *st, usec_t microseconds) {
    struct timeval now;
    now_realtime_timeval(&now);

    if(unlikely(!st->last_collected_time.tv_sec)) {
        // the first entry
        microseconds = st->update_every * USEC_PER_SEC;
    }
    else if(unlikely(!microseconds)) {
        // no dt given by the plugin
        microseconds = dt_usec(&now, &st->last_collected_time);
    }
    else {
        // microseconds has the time since the last collection
//#ifdef NETDATA_INTERNAL_CHECKS
//        usec_t now_usec = timeval_usec(&now);
//        usec_t last_usec = timeval_usec(&st->last_collected_time);
//#endif
        susec_t since_last_usec = dt_usec_signed(&now, &st->last_collected_time);

        if(unlikely(since_last_usec < 0)) {
            // oops! the database is in the future
            error("Database for chart '%s' on host '%s' is %lld microseconds in the future. Adjusting it to current time.", st->id, st->rrdhost->hostname, -since_last_usec);

            st->last_collected_time.tv_sec  = now.tv_sec - st->update_every;
            st->last_collected_time.tv_usec = now.tv_usec;
            last_collected_time_align(&st->last_collected_time, st->update_every);

            st->last_updated.tv_sec  = now.tv_sec - st->update_every;
            st->last_updated.tv_usec = now.tv_usec;
            last_updated_time_align(&st->last_updated, st->update_every);

            microseconds    = st->update_every * USEC_PER_SEC;
            since_last_usec = st->update_every * USEC_PER_SEC;
        }

        // verify the microseconds given is good
        if(unlikely(microseconds > (usec_t)since_last_usec)) {
            debug(D_RRD_CALLS, "dt %llu usec given is too big - it leads %llu usec to the future, for chart '%s' (%s).", microseconds, microseconds - (usec_t)since_last_usec, st->name, st->id);

//#ifdef NETDATA_INTERNAL_CHECKS
//            if(unlikely(last_usec + microseconds > now_usec + 1000))
//                error("dt %llu usec given is too big - it leads %llu usec to the future, for chart '%s' (%s).", microseconds, microseconds - (usec_t)since_last_usec, st->name, st->id);
//#endif

            microseconds = (usec_t)since_last_usec;
        }
        else if(unlikely(microseconds < (usec_t)since_last_usec * 0.8)) {
            debug(D_RRD_CALLS, "dt %llu usec given is too small - expected %llu usec up to -20%%, for chart '%s' (%s).", microseconds, (usec_t)since_last_usec, st->name, st->id);

//#ifdef NETDATA_INTERNAL_CHECKS
//            error("dt %llu usec given is too small - expected %llu usec up to -20%%, for chart '%s' (%s).", microseconds, (usec_t)since_last_usec, st->name, st->id);
//#endif
            microseconds = (usec_t)since_last_usec;
        }
    }
    debug(D_RRD_CALLS, "rrdset_next_usec() for chart %s with microseconds %llu", st->name, microseconds);

    if(unlikely(rrdset_flag_check(st, RRDSET_FLAG_DEBUG)))
        debug(D_RRD_STATS, "%s: NEXT: %llu microseconds", st->name, microseconds);

    st->usec_since_last_update = microseconds;
}


// ----------------------------------------------------------------------------
// RRDSET - process the collected values for all dimensions of a chart

static inline void rrdset_init_last_collected_time(RRDSET *st) {
    now_realtime_timeval(&st->last_collected_time);
    last_collected_time_align(&st->last_collected_time, st->update_every);
}

static inline usec_t rrdset_update_last_collected_time(RRDSET *st) {
    usec_t last_collect_ut = st->last_collected_time.tv_sec * USEC_PER_SEC + st->last_collected_time.tv_usec;
    usec_t ut = last_collect_ut + st->usec_since_last_update;
    st->last_collected_time.tv_sec = (time_t) (ut / USEC_PER_SEC);
    st->last_collected_time.tv_usec = (suseconds_t) (ut % USEC_PER_SEC);
    return last_collect_ut;
}

static inline void rrdset_init_last_updated_time(RRDSET *st) {
    // copy the last collected time to last updated time
    st->last_updated.tv_sec  = st->last_collected_time.tv_sec;
    st->last_updated.tv_usec = st->last_collected_time.tv_usec;
    last_updated_time_align(&st->last_updated, st->update_every);
}

static inline void rrdset_done_push_exclusive(RRDSET *st) {
    if(unlikely(!st->last_collected_time.tv_sec)) {
        // it is the first entry
        // set the last_collected_time to now
        rrdset_init_last_collected_time(st);
    }
    else {
        // it is not the first entry
        // calculate the proper last_collected_time, using usec_since_last_update
        rrdset_update_last_collected_time(st);
    }

    st->counter_done++;

    rrdset_rdlock(st);
    rrdset_done_push(st);
    rrdset_unlock(st);
}

void rrdset_done(RRDSET *st) {
    if(unlikely(netdata_exit)) return;

    if(unlikely(st->rrd_memory_mode == RRD_MEMORY_MODE_NONE)) {
        if(unlikely(st->rrdhost->rrdpush_enabled))
            rrdset_done_push_exclusive(st);

        return;
    }

    debug(D_RRD_CALLS, "rrdset_done() for chart %s", st->name);

    RRDDIM *rd;

    int
            pthreadoldcancelstate;  // store the old cancelable pthread state, to restore it at the end

    char
            store_this_entry = 1,   // boolean: 1 = store this entry, 0 = don't store this entry
            first_entry = 0;        // boolean: 1 = this is the first entry seen for this chart, 0 = all other entries

    unsigned int
            stored_entries = 0;     // the number of entries we have stored in the db, during this call to rrdset_done()

    usec_t
            last_collect_ut,        // the timestamp in microseconds, of the last collected value
            now_collect_ut,         // the timestamp in microseconds, of this collected value (this is NOW)
            last_stored_ut,         // the timestamp in microseconds, of the last stored entry in the db
            next_store_ut,          // the timestamp in microseconds, of the next entry to store in the db
            update_every_ut = st->update_every * USEC_PER_SEC; // st->update_every in microseconds

    if(unlikely(pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &pthreadoldcancelstate) != 0))
        error("Cannot set pthread cancel state to DISABLE.");

    // a read lock is OK here
    rrdset_rdlock(st);

/*
    // enable the chart, if it was disabled
    if(unlikely(rrd_delete_unupdated_dimensions) && !st->enabled)
        st->enabled = 1;
*/

    // check if the chart has a long time to be updated
    if(unlikely(st->usec_since_last_update > st->entries * update_every_ut)) {
        info("%s: took too long to be updated (%0.3Lf secs). Resetting it.", st->name, (long double)(st->usec_since_last_update / 1000000.0));
        rrdset_reset(st);
        st->usec_since_last_update = update_every_ut;
        first_entry = 1;
    }

    if(unlikely(rrdset_flag_check(st, RRDSET_FLAG_DEBUG)))
        debug(D_RRD_STATS, "%s: microseconds since last update: %llu", st->name, st->usec_since_last_update);

    // set last_collected_time
    if(unlikely(!st->last_collected_time.tv_sec)) {
        // it is the first entry
        // set the last_collected_time to now
        rrdset_init_last_collected_time(st);

        last_collect_ut = st->last_collected_time.tv_sec * USEC_PER_SEC + st->last_collected_time.tv_usec - update_every_ut;

        // the first entry should not be stored
        store_this_entry = 0;
        first_entry = 1;

        if(unlikely(rrdset_flag_check(st, RRDSET_FLAG_DEBUG)))
            debug(D_RRD_STATS, "%s: has not set last_collected_time. Setting it now. Will not store the next entry.", st->name);
    }
    else {
        // it is not the first entry
        // calculate the proper last_collected_time, using usec_since_last_update
        last_collect_ut = rrdset_update_last_collected_time(st);
    }

    // if this set has not been updated in the past
    // we fake the last_update time to be = now - usec_since_last_update
    if(unlikely(!st->last_updated.tv_sec)) {
        // it has never been updated before
        // set a fake last_updated, in the past using usec_since_last_update
        rrdset_init_last_updated_time(st);

        // the first entry should not be stored
        store_this_entry = 0;
        first_entry = 1;

        if(unlikely(rrdset_flag_check(st, RRDSET_FLAG_DEBUG)))
            debug(D_RRD_STATS, "%s: initializing last_updated to last_collected_time - %llu microseconds. Will not store the next entry.", st->name, st->usec_since_last_update);
    }

    // check if we will re-write the entire data set
    if(unlikely(dt_usec(&st->last_collected_time, &st->last_updated) > st->entries * update_every_ut)) {
        info("%s: too old data (last updated at %ld.%ld, last collected at %ld.%ld). Resetting it. Will not store the next entry.", st->name, st->last_updated.tv_sec, st->last_updated.tv_usec, st->last_collected_time.tv_sec, st->last_collected_time.tv_usec);
        rrdset_reset(st);
        rrdset_init_last_updated_time(st);

        st->usec_since_last_update = update_every_ut;

        // the first entry should not be stored
        store_this_entry = 0;
        first_entry = 1;
    }

    // these are the 3 variables that will help us in interpolation
    // last_stored_ut = the last time we added a value to the storage
    // now_collect_ut = the time the current value has been collected
    // next_store_ut  = the time of the next interpolation point
    last_stored_ut = st->last_updated.tv_sec * USEC_PER_SEC + st->last_updated.tv_usec;
    now_collect_ut = st->last_collected_time.tv_sec * USEC_PER_SEC + st->last_collected_time.tv_usec;
    next_store_ut  = (st->last_updated.tv_sec + st->update_every) * USEC_PER_SEC;

    if(unlikely(rrdset_flag_check(st, RRDSET_FLAG_DEBUG))) {
        debug(D_RRD_STATS, "%s: last_collect_ut = %0.3Lf (last collection time)", st->name, (long double)last_collect_ut/1000000.0);
        debug(D_RRD_STATS, "%s: now_collect_ut  = %0.3Lf (current collection time)", st->name, (long double)now_collect_ut/1000000.0);
        debug(D_RRD_STATS, "%s: last_stored_ut  = %0.3Lf (last updated time)", st->name, (long double)last_stored_ut/1000000.0);
        debug(D_RRD_STATS, "%s: next_store_ut   = %0.3Lf (next interpolation point)", st->name, (long double)next_store_ut/1000000.0);
    }

    if(unlikely(!st->counter_done)) {
        store_this_entry = 0;
        if(unlikely(rrdset_flag_check(st, RRDSET_FLAG_DEBUG)))
            debug(D_RRD_STATS, "%s: Will not store the next entry.", st->name);
    }
    st->counter_done++;

    if(unlikely(st->rrdhost->rrdpush_enabled))
        rrdset_done_push(st);

    // calculate totals and count the dimensions
    int dimensions = 0;
    st->collected_total = 0;
    rrddim_foreach_read(rd, st) {
        dimensions++;
        if(likely(rd->updated))
            st->collected_total += rd->collected_value;
    }

    uint32_t storage_flags = SN_EXISTS;

    // process all dimensions to calculate their values
    // based on the collected figures only
    // at this stage we do not interpolate anything
    rrddim_foreach_read(rd, st) {

        if(unlikely(!rd->updated)) {
            rd->calculated_value = 0;
            continue;
        }

        if(unlikely(rrdset_flag_check(st, RRDSET_FLAG_DEBUG)))
            debug(D_RRD_STATS, "%s/%s: START "
                    " last_collected_value = " COLLECTED_NUMBER_FORMAT
                    " collected_value = " COLLECTED_NUMBER_FORMAT
                    " last_calculated_value = " CALCULATED_NUMBER_FORMAT
                    " calculated_value = " CALCULATED_NUMBER_FORMAT
                                      , st->id, rd->name
                                      , rd->last_collected_value
                                      , rd->collected_value
                                      , rd->last_calculated_value
                                      , rd->calculated_value
            );

        switch(rd->algorithm) {
            case RRD_ALGORITHM_ABSOLUTE:
                rd->calculated_value = (calculated_number)rd->collected_value
                                       * (calculated_number)rd->multiplier
                                       / (calculated_number)rd->divisor;

                if(unlikely(rrdset_flag_check(st, RRDSET_FLAG_DEBUG)))
                    debug(D_RRD_STATS, "%s/%s: CALC ABS/ABS-NO-IN "
                            CALCULATED_NUMBER_FORMAT " = "
                            COLLECTED_NUMBER_FORMAT
                            " * " CALCULATED_NUMBER_FORMAT
                            " / " CALCULATED_NUMBER_FORMAT
                          , st->id, rd->name
                          , rd->calculated_value
                          , rd->collected_value
                          , (calculated_number)rd->multiplier
                          , (calculated_number)rd->divisor
                    );
                break;

            case RRD_ALGORITHM_PCENT_OVER_ROW_TOTAL:
                if(unlikely(!st->collected_total))
                    rd->calculated_value = 0;
                else
                    // the percentage of the current value
                    // over the total of all dimensions
                    rd->calculated_value =
                            (calculated_number)100
                            * (calculated_number)rd->collected_value
                            / (calculated_number)st->collected_total;

                if(unlikely(rrdset_flag_check(st, RRDSET_FLAG_DEBUG)))
                    debug(D_RRD_STATS, "%s/%s: CALC PCENT-ROW "
                            CALCULATED_NUMBER_FORMAT " = 100"
                                    " * " COLLECTED_NUMBER_FORMAT
                            " / " COLLECTED_NUMBER_FORMAT
                          , st->id, rd->name
                          , rd->calculated_value
                          , rd->collected_value
                          , st->collected_total
                    );
                break;

            case RRD_ALGORITHM_INCREMENTAL:
                if(unlikely(rd->collections_counter <= 1)) {
                    rd->calculated_value = 0;
                    continue;
                }

                // if the new is smaller than the old (an overflow, or reset), set the old equal to the new
                // to reset the calculation (it will give zero as the calculation for this second)
                if(unlikely(rd->last_collected_value > rd->collected_value)) {
                    debug(D_RRD_STATS, "%s.%s: RESET or OVERFLOW. Last collected value = " COLLECTED_NUMBER_FORMAT ", current = " COLLECTED_NUMBER_FORMAT
                          , st->name, rd->name
                          , rd->last_collected_value
                          , rd->collected_value);

                    if(!(rrddim_flag_check(rd, RRDDIM_FLAG_DONT_DETECT_RESETS_OR_OVERFLOWS)))
                        storage_flags = SN_EXISTS_RESET;

                    rd->last_collected_value = rd->collected_value;
                }

                rd->calculated_value +=
                        (calculated_number)(rd->collected_value - rd->last_collected_value)
                        * (calculated_number)rd->multiplier
                        / (calculated_number)rd->divisor;

                if(unlikely(rrdset_flag_check(st, RRDSET_FLAG_DEBUG)))
                    debug(D_RRD_STATS, "%s/%s: CALC INC PRE "
                            CALCULATED_NUMBER_FORMAT " = ("
                            COLLECTED_NUMBER_FORMAT " - " COLLECTED_NUMBER_FORMAT
                            ")"
                                    " * " CALCULATED_NUMBER_FORMAT
                            " / " CALCULATED_NUMBER_FORMAT
                          , st->id, rd->name
                          , rd->calculated_value
                          , rd->collected_value, rd->last_collected_value
                          , (calculated_number)rd->multiplier
                          , (calculated_number)rd->divisor
                    );
                break;

            case RRD_ALGORITHM_PCENT_OVER_DIFF_TOTAL:
                if(unlikely(rd->collections_counter <= 1)) {
                    rd->calculated_value = 0;
                    continue;
                }

                // if the new is smaller than the old (an overflow, or reset), set the old equal to the new
                // to reset the calculation (it will give zero as the calculation for this second)
                if(unlikely(rd->last_collected_value > rd->collected_value)) {
                    debug(D_RRD_STATS, "%s.%s: RESET or OVERFLOW. Last collected value = " COLLECTED_NUMBER_FORMAT ", current = " COLLECTED_NUMBER_FORMAT
                          , st->name, rd->name
                          , rd->last_collected_value
                          , rd->collected_value);

                    if(!(rrddim_flag_check(rd, RRDDIM_FLAG_DONT_DETECT_RESETS_OR_OVERFLOWS)))
                        storage_flags = SN_EXISTS_RESET;

                    rd->last_collected_value = rd->collected_value;
                }

                // the percentage of the current increment
                // over the increment of all dimensions together
                if(unlikely(st->collected_total == st->last_collected_total))
                    rd->calculated_value = 0;
                else
                    rd->calculated_value =
                            (calculated_number)100
                            * (calculated_number)(rd->collected_value - rd->last_collected_value)
                            / (calculated_number)(st->collected_total - st->last_collected_total);

                if(unlikely(rrdset_flag_check(st, RRDSET_FLAG_DEBUG)))
                    debug(D_RRD_STATS, "%s/%s: CALC PCENT-DIFF "
                            CALCULATED_NUMBER_FORMAT " = 100"
                                    " * (" COLLECTED_NUMBER_FORMAT " - " COLLECTED_NUMBER_FORMAT ")"
                                    " / (" COLLECTED_NUMBER_FORMAT " - " COLLECTED_NUMBER_FORMAT ")"
                          , st->id, rd->name
                          , rd->calculated_value
                          , rd->collected_value, rd->last_collected_value
                          , st->collected_total, st->last_collected_total
                    );
                break;

            default:
                // make the default zero, to make sure
                // it gets noticed when we add new types
                rd->calculated_value = 0;

                if(unlikely(rrdset_flag_check(st, RRDSET_FLAG_DEBUG)))
                    debug(D_RRD_STATS, "%s/%s: CALC "
                            CALCULATED_NUMBER_FORMAT " = 0"
                          , st->id, rd->name
                          , rd->calculated_value
                    );
                break;
        }

        if(unlikely(rrdset_flag_check(st, RRDSET_FLAG_DEBUG)))
            debug(D_RRD_STATS, "%s/%s: PHASE2 "
                    " last_collected_value = " COLLECTED_NUMBER_FORMAT
                    " collected_value = " COLLECTED_NUMBER_FORMAT
                    " last_calculated_value = " CALCULATED_NUMBER_FORMAT
                    " calculated_value = " CALCULATED_NUMBER_FORMAT
                                      , st->id, rd->name
                                      , rd->last_collected_value
                                      , rd->collected_value
                                      , rd->last_calculated_value
                                      , rd->calculated_value
            );

    }

    // at this point we have all the calculated values ready
    // it is now time to interpolate values on a second boundary

    if(unlikely(now_collect_ut < next_store_ut)) {
        // this is collected in the same interpolation point

        if(unlikely(rrdset_flag_check(st, RRDSET_FLAG_DEBUG)))
            debug(D_RRD_STATS, "%s: THIS IS IN THE SAME INTERPOLATION POINT", st->name);

//#ifdef NETDATA_INTERNAL_CHECKS
//        info("%s is collected in the same interpolation point: short by %llu microseconds", st->name, next_store_ut - now_collect_ut);
//#endif
    }

    usec_t first_ut = last_stored_ut;
    long long iterations = (now_collect_ut - last_stored_ut) / (update_every_ut);
    if((now_collect_ut % (update_every_ut)) == 0) iterations++;

    for( ; next_store_ut <= now_collect_ut ; last_collect_ut = next_store_ut, next_store_ut += update_every_ut, iterations-- ) {
//#ifdef NETDATA_INTERNAL_CHECKS
//        if(iterations < 0) { error("%s: iterations calculation wrapped! first_ut = %llu, last_stored_ut = %llu, next_store_ut = %llu, now_collect_ut = %llu", st->name, first_ut, last_stored_ut, next_store_ut, now_collect_ut); }
//#endif

        if(unlikely(rrdset_flag_check(st, RRDSET_FLAG_DEBUG))) {
            debug(D_RRD_STATS, "%s: last_stored_ut = %0.3Lf (last updated time)", st->name, (long double)last_stored_ut/1000000.0);
            debug(D_RRD_STATS, "%s: next_store_ut  = %0.3Lf (next interpolation point)", st->name, (long double)next_store_ut/1000000.0);
        }

        st->last_updated.tv_sec = (time_t) (next_store_ut / USEC_PER_SEC);
        st->last_updated.tv_usec = 0;

        rrddim_foreach_read(rd, st) {
            calculated_number new_value;

            switch(rd->algorithm) {
                case RRD_ALGORITHM_INCREMENTAL:
                    new_value = (calculated_number)
                            (      rd->calculated_value
                                   * (calculated_number)(next_store_ut - last_collect_ut)
                                   / (calculated_number)(now_collect_ut - last_collect_ut)
                            );

                    if(unlikely(rrdset_flag_check(st, RRDSET_FLAG_DEBUG)))
                        debug(D_RRD_STATS, "%s/%s: CALC2 INC "
                                CALCULATED_NUMBER_FORMAT " = "
                                CALCULATED_NUMBER_FORMAT
                                " * %llu"
                                        " / %llu"
                              , st->id, rd->name
                              , new_value
                              , rd->calculated_value
                              , (next_store_ut - last_stored_ut)
                              , (now_collect_ut - last_stored_ut)
                        );

                    rd->calculated_value -= new_value;
                    new_value += rd->last_calculated_value;
                    rd->last_calculated_value = 0;
                    new_value /= (calculated_number)st->update_every;

                    if(unlikely(next_store_ut - last_stored_ut < update_every_ut)) {
                        if(unlikely(rrdset_flag_check(st, RRDSET_FLAG_DEBUG)))
                            debug(D_RRD_STATS, "%s/%s: COLLECTION POINT IS SHORT " CALCULATED_NUMBER_FORMAT " - EXTRAPOLATING",
                                    st->id, rd->name
                                  , (calculated_number)(next_store_ut - last_stored_ut)
                            );
                        new_value = new_value * (calculated_number)(st->update_every * 1000000) / (calculated_number)(next_store_ut - last_stored_ut);
                    }
                    break;

                case RRD_ALGORITHM_ABSOLUTE:
                case RRD_ALGORITHM_PCENT_OVER_ROW_TOTAL:
                case RRD_ALGORITHM_PCENT_OVER_DIFF_TOTAL:
                default:
                    if(iterations == 1) {
                        // this is the last iteration
                        // do not interpolate
                        // just show the calculated value

                        new_value = rd->calculated_value;
                    }
                    else {
                        // we have missed an update
                        // interpolate in the middle values

                        new_value = (calculated_number)
                                (   (     (rd->calculated_value - rd->last_calculated_value)
                                          * (calculated_number)(next_store_ut - last_collect_ut)
                                          / (calculated_number)(now_collect_ut - last_collect_ut)
                                    )
                                    +  rd->last_calculated_value
                                );

                        if(unlikely(rrdset_flag_check(st, RRDSET_FLAG_DEBUG)))
                            debug(D_RRD_STATS, "%s/%s: CALC2 DEF "
                                    CALCULATED_NUMBER_FORMAT " = ((("
                                            "(" CALCULATED_NUMBER_FORMAT " - " CALCULATED_NUMBER_FORMAT ")"
                                            " * %llu"
                                            " / %llu) + " CALCULATED_NUMBER_FORMAT
                                  , st->id, rd->name
                                  , new_value
                                  , rd->calculated_value, rd->last_calculated_value
                                  , (next_store_ut - first_ut)
                                  , (now_collect_ut - first_ut), rd->last_calculated_value
                            );
                    }
                    break;
            }

            if(unlikely(!store_this_entry)) {
                rd->values[st->current_entry] = pack_storage_number(0, SN_NOT_EXISTS);
                continue;
            }

            if(likely(rd->updated && rd->collections_counter > 1 && iterations < st->gap_when_lost_iterations_above)) {
                rd->values[st->current_entry] = pack_storage_number(new_value, storage_flags );
                rd->last_stored_value = new_value;

                if(unlikely(rrdset_flag_check(st, RRDSET_FLAG_DEBUG)))
                    debug(D_RRD_STATS, "%s/%s: STORE[%ld] "
                            CALCULATED_NUMBER_FORMAT " = " CALCULATED_NUMBER_FORMAT
                          , st->id, rd->name
                          , st->current_entry
                          , unpack_storage_number(rd->values[st->current_entry]), new_value
                    );
            }
            else {
                if(unlikely(rrdset_flag_check(st, RRDSET_FLAG_DEBUG)))
                    debug(D_RRD_STATS, "%s/%s: STORE[%ld] = NON EXISTING "
                          , st->id, rd->name
                          , st->current_entry
                    );
                rd->values[st->current_entry] = pack_storage_number(0, SN_NOT_EXISTS);
                rd->last_stored_value = NAN;
            }

            stored_entries++;

            if(unlikely(rrdset_flag_check(st, RRDSET_FLAG_DEBUG))) {
                calculated_number t1 = new_value * (calculated_number)rd->multiplier / (calculated_number)rd->divisor;
                calculated_number t2 = unpack_storage_number(rd->values[st->current_entry]);
                calculated_number accuracy = accuracy_loss(t1, t2);
                debug(D_RRD_STATS, "%s/%s: UNPACK[%ld] = " CALCULATED_NUMBER_FORMAT " FLAGS=0x%08x (original = " CALCULATED_NUMBER_FORMAT ", accuracy loss = " CALCULATED_NUMBER_FORMAT "%%%s)"
                      , st->id, rd->name
                      , st->current_entry
                      , t2
                      , get_storage_number_flags(rd->values[st->current_entry])
                      , t1
                      , accuracy
                      , (accuracy > ACCURACY_LOSS) ? " **TOO BIG** " : ""
                );

                rd->collected_volume += t1;
                rd->stored_volume += t2;
                accuracy = accuracy_loss(rd->collected_volume, rd->stored_volume);
                debug(D_RRD_STATS, "%s/%s: VOLUME[%ld] = " CALCULATED_NUMBER_FORMAT ", calculated  = " CALCULATED_NUMBER_FORMAT ", accuracy loss = " CALCULATED_NUMBER_FORMAT "%%%s"
                      , st->id, rd->name
                      , st->current_entry
                      , rd->stored_volume
                      , rd->collected_volume
                      , accuracy
                      , (accuracy > ACCURACY_LOSS) ? " **TOO BIG** " : ""
                );

            }
        }
        // reset the storage flags for the next point, if any;
        storage_flags = SN_EXISTS;

        st->counter++;
        st->current_entry = ((st->current_entry + 1) >= st->entries) ? 0 : st->current_entry + 1;
        last_stored_ut = next_store_ut;
    }

    st->last_collected_total  = st->collected_total;

    rrddim_foreach_read(rd, st) {
        if(unlikely(!rd->updated))
            continue;

        if(unlikely(rrdset_flag_check(st, RRDSET_FLAG_DEBUG)))
            debug(D_RRD_STATS, "%s/%s: setting last_collected_value (old: " COLLECTED_NUMBER_FORMAT ") to last_collected_value (new: " COLLECTED_NUMBER_FORMAT ")", st->id, rd->name, rd->last_collected_value, rd->collected_value);

        rd->last_collected_value = rd->collected_value;

        switch(rd->algorithm) {
            case RRD_ALGORITHM_INCREMENTAL:
                if(unlikely(!first_entry)) {
                    if(unlikely(rrdset_flag_check(st, RRDSET_FLAG_DEBUG)))
                        debug(D_RRD_STATS, "%s/%s: setting last_calculated_value (old: " CALCULATED_NUMBER_FORMAT ") to last_calculated_value (new: " CALCULATED_NUMBER_FORMAT ")", st->id, rd->name, rd->last_calculated_value + rd->calculated_value, rd->calculated_value);
                    rd->last_calculated_value += rd->calculated_value;
                }
                else {
                    if(unlikely(rrdset_flag_check(st, RRDSET_FLAG_DEBUG)))
                        debug(D_RRD_STATS, "%s: THIS IS THE FIRST POINT", st->name);
                }
                break;

            case RRD_ALGORITHM_ABSOLUTE:
            case RRD_ALGORITHM_PCENT_OVER_ROW_TOTAL:
            case RRD_ALGORITHM_PCENT_OVER_DIFF_TOTAL:
                if(unlikely(rrdset_flag_check(st, RRDSET_FLAG_DEBUG)))
                    debug(D_RRD_STATS, "%s/%s: setting last_calculated_value (old: " CALCULATED_NUMBER_FORMAT ") to last_calculated_value (new: " CALCULATED_NUMBER_FORMAT ")", st->id, rd->name, rd->last_calculated_value, rd->calculated_value);
                rd->last_calculated_value = rd->calculated_value;
                break;
        }

        rd->calculated_value = 0;
        rd->collected_value = 0;
        rd->updated = 0;

        if(unlikely(rrdset_flag_check(st, RRDSET_FLAG_DEBUG)))
            debug(D_RRD_STATS, "%s/%s: END "
                    " last_collected_value = " COLLECTED_NUMBER_FORMAT
                    " collected_value = " COLLECTED_NUMBER_FORMAT
                    " last_calculated_value = " CALCULATED_NUMBER_FORMAT
                    " calculated_value = " CALCULATED_NUMBER_FORMAT
                                      , st->id, rd->name
                                      , rd->last_collected_value
                                      , rd->collected_value
                                      , rd->last_calculated_value
                                      , rd->calculated_value
            );
    }

    // ALL DONE ABOUT THE DATA UPDATE
    // --------------------------------------------------------------------

/*
    // find if there are any obsolete dimensions (not updated recently)
    if(unlikely(rrd_delete_unupdated_dimensions)) {

        for( rd = st->dimensions; likely(rd) ; rd = rd->next )
            if((rd->last_collected_time.tv_sec + (rrd_delete_unupdated_dimensions * st->update_every)) < st->last_collected_time.tv_sec)
                break;

        if(unlikely(rd)) {
            RRDDIM *last;
            // there is dimension to free
            // upgrade our read lock to a write lock
            pthread_rwlock_unlock(&st->rrdset_rwlock);
            pthread_rwlock_wrlock(&st->rrdset_rwlock);

            for( rd = st->dimensions, last = NULL ; likely(rd) ; ) {
                // remove it only it is not updated in rrd_delete_unupdated_dimensions seconds

                if(unlikely((rd->last_collected_time.tv_sec + (rrd_delete_unupdated_dimensions * st->update_every)) < st->last_collected_time.tv_sec)) {
                    info("Removing obsolete dimension '%s' (%s) of '%s' (%s).", rd->name, rd->id, st->name, st->id);

                    if(unlikely(!last)) {
                        st->dimensions = rd->next;
                        rd->next = NULL;
                        rrddim_free(st, rd);
                        rd = st->dimensions;
                        continue;
                    }
                    else {
                        last->next = rd->next;
                        rd->next = NULL;
                        rrddim_free(st, rd);
                        rd = last->next;
                        continue;
                    }
                }

                last = rd;
                rd = rd->next;
            }

            if(unlikely(!st->dimensions)) {
                info("Disabling chart %s (%s) since it does not have any dimensions", st->name, st->id);
                st->enabled = 0;
            }
        }
    }
*/

    rrdset_unlock(st);

    if(unlikely(pthread_setcancelstate(pthreadoldcancelstate, NULL) != 0))
        error("Cannot set pthread cancel state to RESTORE (%d).", pthreadoldcancelstate);
}

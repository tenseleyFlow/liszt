#ifndef LISZT_IDCACHE_H
#define LISZT_IDCACHE_H

#include <sys/types.h>

/* uid/gid to name, cached exactly and unbounded within a run (GNU
   idcache semantics). NULL when the id has no name - callers render the
   numeric id. */
const char *liszt_getuser(uid_t uid);
const char *liszt_getgroup(gid_t gid);

#endif

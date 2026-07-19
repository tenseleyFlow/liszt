#include "idcache.h"

#include <grp.h>
#include <pwd.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

/* Linear caches: listings rarely span more than a handful of distinct
   ids, and misses hit NSS which dwarfs the scan (aspen's shape). */

struct ident {
    unsigned long id;
    char *name;     /* NULL = negative entry */
};

struct idmap {
    struct ident *v;
    size_t len;
    size_t cap;
};

static struct idmap users;
static struct idmap groups;

static const char *
lookup(struct idmap *m, unsigned long id, char *(*fetch)(unsigned long))
{
    for (size_t i = 0; i < m->len; i++)
        if (m->v[i].id == id)
            return m->v[i].name;

    if (m->len == m->cap) {
        m->cap = m->cap ? m->cap * 2 : 16;
        m->v = liszt_xrealloc(m->v, m->cap * sizeof *m->v);
    }
    char *name = fetch(id);
    m->v[m->len].id = id;
    m->v[m->len].name = name;
    m->len++;
    return name;
}

static char *
fetch_user(unsigned long id)
{
    struct passwd *pw = getpwuid((uid_t)id);
    return pw ? liszt_xstrdup(pw->pw_name) : NULL;
}

static char *
fetch_group(unsigned long id)
{
    struct group *gr = getgrgid((gid_t)id);
    return gr ? liszt_xstrdup(gr->gr_name) : NULL;
}

const char *
liszt_getuser(uid_t uid)
{
    return lookup(&users, (unsigned long)uid, fetch_user);
}

const char *
liszt_getgroup(gid_t gid)
{
    return lookup(&groups, (unsigned long)gid, fetch_group);
}

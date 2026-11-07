#include "amiga_folder.h"
#include <dos/dosextens.h>
#include <proto/dos.h>
#include <proto/utility.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int compare_names(const void *left, const void *right)
{
    const DockFolderEntry *a = left;
    const DockFolderEntry *b = right;
    return Stricmp((STRPTR)a->name,(STRPTR)b->name);
}

static int compare_entries(const void *left, const void *right)
{
    const DockFolderEntry *a = left, *b = right;
    int folded = compare_names(a,b);
    return folded ? folded : strcmp(a->name,b->name);
}

static size_t sidecar_base_length(const DockFolderEntry *entry)
{
    size_t length = strlen(entry->name);
    if (entry->drawer || length <= 5 || Stricmp((STRPTR)(entry->name+length-5),(STRPTR)".info"))
        return 0;
    return length-5;
}

static size_t mark_custom_icons(DockFolderEntry *entries, size_t count)
{
    size_t i, visible = 0;
    for (i = 0; i < count; ++i) {
        size_t length = sidecar_base_length(&entries[i]);
        DockFolderEntry key, *entry;
        if (!length) continue;
        memcpy(key.name,entries[i].name,length);
        key.name[length] = '\0';
        entry = bsearch(&key,entries,count,sizeof(*entries),compare_names);
        if (entry) entry->custom_icon = 1;
    }
    for (i = 0; i < count; ++i) {
        if (!sidecar_base_length(&entries[i])) {
            if (visible != i) entries[visible] = entries[i];
            ++visible;
        }
    }
    return visible;
}

static int link_is_drawer(BPTR directory, const char *name, struct FileInfoBlock *fib)
{
    BPTR previous = CurrentDir(directory);
    BPTR target = Lock((STRPTR)name, ACCESS_READ);
    int drawer;

    CurrentDir(previous);
    if (!target) return 0;
    drawer = Examine(target, fib) && fib->fib_DirEntryType > 0 &&
        fib->fib_DirEntryType != ST_SOFTLINK;
    UnLock(target);
    return drawer;
}

int amiga_folder_read(const char *path, DockFolderEntry **entries, size_t *count)
{
    DockFolderEntry *result = NULL;
    size_t used = 0, capacity = 0;
    struct FileInfoBlock *fib, *target_fib = NULL;
    BPTR lock;
    int ok = 0;

    *entries = NULL;
    *count = 0;
    lock = Lock((STRPTR)path, ACCESS_READ);
    if (!lock) return 0;
    fib = AllocDosObject(DOS_FIB, NULL);
    if (!fib || !Examine(lock, fib) || fib->fib_DirEntryType <= 0 ||
        fib->fib_DirEntryType == ST_SOFTLINK) goto done;

    while (ExNext(lock, fib)) {
        const char *name = (const char *)fib->fib_FileName;
        const char *end = memchr(name, '\0', sizeof(fib->fib_FileName));
        size_t length;
        int drawer = fib->fib_DirEntryType > 0;

        if (!end || end == name) continue;
        length = (size_t)(end - name);
        if (length >= sizeof(result->name)) continue;
        if (fib->fib_DirEntryType == ST_SOFTLINK) {
            if (!target_fib) target_fib = AllocDosObject(DOS_FIB, NULL);
            if (!target_fib) goto done;
            drawer = link_is_drawer(lock, name, target_fib);
        }
        if (!drawer && !Stricmp((STRPTR)name,(STRPTR)".info")) continue;
        if (used == capacity) {
            size_t limit = SIZE_MAX / sizeof(*result);
            size_t next = !capacity ? 32 : (capacity <= limit / 2 ? capacity * 2 : limit);
            DockFolderEntry *grown;

            if (next <= capacity) goto done;
            grown = realloc(result, next * sizeof(*result));
            if (!grown) goto done;
            result = grown;
            capacity = next;
        }
        memcpy(result[used].name, name, length + 1);
        result[used].drawer = drawer;
        result[used].custom_icon = 0;
        ++used;
    }
    ok = IoErr() == ERROR_NO_MORE_ENTRIES;

done:
    if (target_fib) FreeDosObject(DOS_FIB, target_fib);
    if (fib) FreeDosObject(DOS_FIB, fib);
    UnLock(lock);
    if (!ok) {
        free(result);
        return 0;
    }
    if (used > 1) qsort(result, used, sizeof(*result), compare_entries);
    *entries = result;
    *count = mark_custom_icons(result,used);
    return 1;
}

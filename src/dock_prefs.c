#include "dock_prefs.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

enum { VERSION, SIZE, ZOOM, SCALE_PERCENT, LEVELS, EDGE, SNAPSHOT, PIP,
       PIP_X_OFFSET, PIP_Y_OFFSET, PATHS, FOLDER, FIELD_COUNT };

static const char *const keys[FIELD_COUNT] = {
    "version", "size", "zoom", "scale_percent", "levels", "edge", "snapshot", "pip",
    "pip_x_offset", "pip_y_offset", "paths", "folder"
};

static const char *const edge_names[] = {"bottom", "left", "right"};

void dock_prefs_defaults(DockPrefs *prefs)
{
    DockSettings defaults = dock_settings_default();
    memset(prefs, 0, sizeof(*prefs));
    prefs->size = defaults.base_size;
    prefs->zoom = defaults.max_size;
    prefs->levels = defaults.levels;
    prefs->edge = DOCK_EDGE_BOTTOM;
    prefs->snapshot = 1;
    prefs->pip_x_offset = 17;
    prefs->pip_y_offset = -1;
}

static int valid_prefs(const DockPrefs *prefs)
{
    DockSettings settings = dock_settings_default();
    int i;
    settings.base_size = prefs->size;
    settings.max_size = prefs->zoom;
    settings.levels = prefs->levels;
    if (!dock_settings_valid(&settings) ||
        (prefs->scale_percent && dock_scaled_size(prefs->size,prefs->scale_percent) != prefs->zoom) ||
        (unsigned)prefs->edge >= sizeof(edge_names)/sizeof(*edge_names) ||
        (unsigned)prefs->snapshot > 1 || (unsigned)prefs->pip_mode > 1 ||
        (prefs->snapshot && prefs->pip_mode) ||
        prefs->pip_x_offset < -128 || prefs->pip_x_offset > 128 ||
        prefs->pip_y_offset < -128 || prefs->pip_y_offset > 128 ||
        (unsigned)prefs->count > DOCK_MAX_ITEMS ||
        !memchr(prefs->folder,0,sizeof(prefs->folder))) return 0;
    for (i = 0; i < prefs->count; ++i)
        if (!prefs->paths[i][0] ||
            !memchr(prefs->paths[i], 0, DOCK_PREFS_PATH_CAP)) return 0;
    return 1;
}

static int next_token(FILE *file)
{
    int c;
    do c = fgetc(file); while (c == ' ' || c == '\t' || c == '\n' || c == '\r');
    return c;
}

static int hex_digit(int c)
{
    if (c >= '0' && c <= '9') return c-'0';
    if (c >= 'a' && c <= 'f') return c-'a'+10;
    if (c >= 'A' && c <= 'F') return c-'A'+10;
    return -1;
}

static int read_escape(FILE *file)
{
    int i, value = 0, c = fgetc(file);
    switch (c) {
    case '"': case '\\': case '/': return c;
    case 'b': return '\b';
    case 'f': return '\f';
    case 'n': return '\n';
    case 'r': return '\r';
    case 't': return '\t';
    case 'u':
        for (i = 0; i < 4; ++i) {
            int digit = hex_digit(fgetc(file));
            if (digit < 0) return -1;
            value = value*16+digit;
        }
        return value > 0 && value <= 255 ? value : -1;
    default: return -1;
    }
}

/* Amiga path names are byte strings. Encode them as Latin-1 code points
   and accept both JSON escapes and their literal UTF-8 spelling. */
static int read_string(FILE *file, char *buffer, size_t capacity)
{
    size_t length = 0;
    int c;
    if (next_token(file) != '"') return 0;
    while ((c = fgetc(file)) != '"') {
        if (c == '\\') c = read_escape(file);
        else if (c == 0xc2 || c == 0xc3) {
            int tail = fgetc(file);
            if (tail < 0x80 || tail > 0xbf) return 0;
            c = (c-0xc2)*64+tail;
        } else if (c < 32 || c >= 0x80) return 0;
        if (c <= 0 || length+1 >= capacity) return 0;
        buffer[length++] = (char)c;
    }
    buffer[length] = 0;
    return 1;
}

static int read_integer(FILE *file, int *result)
{
    int c = next_token(file), negative = c == '-';
    unsigned int value = 0, limit = (unsigned int)INT_MAX+negative;
    if (negative) c = fgetc(file);
    if (c < '0' || c > '9') return 0;
    if (c == '0') c = fgetc(file);
    else do {
        unsigned int digit = (unsigned int)(c-'0');
        if (value > (limit-digit)/10) return 0;
        value = value*10+digit;
        c = fgetc(file);
    } while (c >= '0' && c <= '9');
    if (c != EOF && ungetc(c,file) == EOF) return 0;
    if (negative) *result = value == limit ? INT_MIN : -(int)value;
    else *result = (int)value;
    return 1;
}

static int read_boolean(FILE *file, int *result)
{
    int c = next_token(file);
    const char *tail;
    if (c == 't') tail = "rue";
    else if (c == 'f') tail = "alse";
    else return 0;
    while (*tail) if (fgetc(file) != *tail++) return 0;
    *result = c == 't';
    return 1;
}

static int read_edge(FILE *file, DockEdge *edge)
{
    char name[12];
    unsigned int i;
    if (!read_string(file,name,sizeof(name))) return 0;
    for (i = 0; i < sizeof(edge_names)/sizeof(*edge_names); ++i) {
        if (strcmp(name,edge_names[i])) continue;
        *edge = (DockEdge)i;
        return 1;
    }
    return 0;
}

static int read_paths(FILE *file, DockPrefs *prefs)
{
    int c;
    if (next_token(file) != '[') return 0;
    c = next_token(file);
    if (c == ']') return 1;
    if (c == EOF || ungetc(c,file) == EOF) return 0;
    do {
        if (prefs->count >= DOCK_MAX_ITEMS ||
            !read_string(file,prefs->paths[prefs->count],DOCK_PREFS_PATH_CAP)) return 0;
        ++prefs->count;
        c = next_token(file);
    } while (c == ',');
    return c == ']';
}

static int read_object(FILE *file, DockPrefs *prefs)
{
    unsigned int seen = 0;
    int c;
    if (next_token(file) != '{') return 0;
    do {
        char key[24];
        int field, ok = 0, version;
        if (!read_string(file,key,sizeof(key)) || next_token(file) != ':') return 0;
        for (field = 0; field < FIELD_COUNT; ++field)
            if (!strcmp(key,keys[field])) break;
        if (field == FIELD_COUNT || (seen & (1U<<field))) return 0;
        seen |= 1U<<field;
        switch (field) {
        case VERSION: ok = read_integer(file,&version) && version == 1; break;
        case SIZE: ok = read_integer(file,&prefs->size); break;
        case ZOOM: ok = read_integer(file,&prefs->zoom); break;
        case SCALE_PERCENT:
            ok = read_integer(file,&prefs->scale_percent) && prefs->scale_percent >= DOCK_SCALE_ONE;
            break;
        case LEVELS: ok = read_integer(file,&prefs->levels); break;
        case EDGE: ok = read_edge(file,&prefs->edge); break;
        case SNAPSHOT: ok = read_boolean(file,&prefs->snapshot); break;
        case PIP: ok = read_boolean(file,&prefs->pip_mode); break;
        case PIP_X_OFFSET: ok = read_integer(file,&prefs->pip_x_offset); break;
        case PIP_Y_OFFSET: ok = read_integer(file,&prefs->pip_y_offset); break;
        case PATHS: ok = read_paths(file,prefs); break;
        case FOLDER: ok = read_string(file,prefs->folder,sizeof(prefs->folder)); break;
        }
        if (!ok) return 0;
        c = next_token(file);
    } while (c == ',');
    if (prefs->pip_mode && !(seen & (1U<<SNAPSHOT))) prefs->snapshot = 0;
    if (seen & (1U<<SCALE_PERCENT)) {
        if (seen & (1U<<ZOOM)) return 0;
        prefs->zoom = dock_scaled_size(prefs->size,prefs->scale_percent);
    }
    return c == '}' && (seen & (1U<<VERSION)) && next_token(file) == EOF &&
        !ferror(file) && valid_prefs(prefs);
}

int dock_prefs_read(FILE *file, DockPrefs *prefs)
{
    DockPrefs *parsed;
    int ok;
    if (!file || !prefs) return 0;
    parsed = malloc(sizeof(*parsed));
    if (!parsed) return 0;
    dock_prefs_defaults(parsed);
    ok = read_object(file,parsed);
    if (ok) *prefs = *parsed;
    free(parsed);
    return ok;
}

static int write_string(FILE *file, const char *string)
{
    const unsigned char *p = (const unsigned char *)string;
    if (fputc('"',file) == EOF) return 0;
    for (; *p; ++p) {
        if (*p < 32 || *p >= 127) {
            if (fprintf(file,"\\u%04x",(unsigned int)*p) < 0) return 0;
        } else {
            if ((*p == '"' || *p == '\\') && fputc('\\',file) == EOF) return 0;
            if (fputc(*p,file) == EOF) return 0;
        }
    }
    return fputc('"',file) != EOF;
}

int dock_prefs_write(FILE *file, const DockPrefs *prefs)
{
    int i;
    if (!file || !prefs || !valid_prefs(prefs)) return 0;
    if (fprintf(file,
            "{\n  \"version\": 1,\n  \"size\": %d,\n  \"%s\": %d,\n  \"levels\": %d,\n"
            "  \"edge\": \"%s\",\n  \"snapshot\": %s,\n  \"pip\": %s,\n"
            "  \"pip_x_offset\": %d,\n  \"pip_y_offset\": %d,\n",
            prefs->size,prefs->scale_percent ? "scale_percent" : "zoom",
            prefs->scale_percent ? prefs->scale_percent : prefs->zoom,prefs->levels,edge_names[prefs->edge],
            prefs->snapshot ? "true" : "false",prefs->pip_mode ? "true" : "false",
            prefs->pip_x_offset,prefs->pip_y_offset) < 0) return 0;
    if (prefs->folder[0] && (fputs("  \"folder\": ",file) == EOF ||
            !write_string(file,prefs->folder) || fputs(",\n",file) == EOF)) return 0;
    if (fputs("  \"paths\": [",file) == EOF) return 0;
    for (i = 0; i < prefs->count; ++i) {
        if (fputs(i ? ",\n    " : "\n    ",file) == EOF ||
            !write_string(file,prefs->paths[i])) return 0;
    }
    return fputs(prefs->count ? "\n  ]\n}\n" : "]\n}\n",file) != EOF && !ferror(file);
}

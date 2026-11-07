#include "dock_prefs.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int read_text(const char *text, DockPrefs *prefs)
{
    FILE *file = tmpfile();
    int ok;
    assert(file);
    assert(fputs(text,file) != EOF);
    rewind(file);
    ok = dock_prefs_read(file,prefs);
    assert(fclose(file) == 0);
    return ok;
}

static void test_roundtrip(void)
{
    DockPrefs before, after;
    FILE *file = tmpfile();
    char json[4096];
    size_t length;
    assert(file);
    dock_prefs_defaults(&before);
    before.edge = DOCK_EDGE_RIGHT;
    before.zoom = 70;
    before.pip_mode = 1;
    before.snapshot = 0;
    before.pip_x_offset = -128;
    before.pip_y_offset = 128;
    strcpy(before.folder,"SYS:My \"folder\"/F\xf6r");
    before.count = 3;
    strcpy(before.paths[0],"SYS:Tools/A quoted \"tool\"\\here");
    strcpy(before.paths[1],"RAM:control\t\x01\x7f\xe5\xff");
    memset(before.paths[2],'x',DOCK_PREFS_PATH_CAP-1);
    assert(dock_prefs_write(file,&before));
    rewind(file);
    length = fread(json,1,sizeof(json)-1,file);
    json[length] = 0;
    assert(strstr(json,"\\\"tool\\\"\\\\here"));
    assert(strstr(json,"\\u0009\\u0001\\u007f\\u00e5\\u00ff"));
    assert(strstr(json,"\"folder\": \"SYS:My \\\"folder\\\"/F\\u00f6r\""));
    rewind(file);
    assert(dock_prefs_read(file,&after));
    assert(!memcmp(&before,&after,sizeof(before)));
    assert(fclose(file) == 0);
}

static void test_edited_file(void)
{
    DockPrefs prefs;
    assert(read_text(" {\n  \"paths\" : [ \"RAM:\xc3\xa5\", \"SYS:Prefs\\/\" ],"
        "\r\n\"edge\":\"left\", \"pip\":true, \"version\": 1 }\t\n",&prefs));
    assert(prefs.count == 2);
    assert(!strcmp(prefs.paths[0],"RAM:\xe5"));
    assert(!strcmp(prefs.paths[1],"SYS:Prefs/"));
    assert(prefs.edge == DOCK_EDGE_LEFT && prefs.pip_mode && !prefs.snapshot);
    assert(prefs.size == dock_settings_default().base_size);
    assert(read_text("{\"version\":1,\"paths\":[]}",&prefs));
    assert(prefs.count == 0 && prefs.snapshot && !prefs.pip_mode && !prefs.folder[0]);
}

static void test_scale(void)
{
    DockPrefs before, after;
    FILE *file = tmpfile();
    assert(file);
    assert(read_text("{\"version\":1,\"scale_percent\":133,\"size\":16}",&before));
    assert(before.scale_percent == 133 && before.zoom == 21);
    assert(dock_prefs_write(file,&before));
    rewind(file);
    assert(dock_prefs_read(file,&after));
    assert(after.scale_percent == 133 && after.zoom == 21);
    assert(fclose(file) == 0);
    assert(dock_scaled_size(41,195) == 80);
    assert(dock_scaled_size(50,195) == 98);
    assert(dock_scaled_size(96,200) == 192);
}

static void test_bad_file(void)
{
    static const char *const bad[] = {
        "", "{}", "{\"version\":2}", "{\"version\":1,\"version\":1}",
        "{\"version\":1,\"paths\":[\"SYS:ok\",",
        "{\"version\":1,\"paths\":[\"SYS:ok\",]}",
        "{\"version\":1,\"paths\":[\"\"]}",
        "{\"version\":1,\"folder\":[\"SYS:One\",\"SYS:Two\"]}",
        "{\"version\":1,\"folder\":\"SYS:One\",\"folder\":\"SYS:Two\"}",
        "{\"version\":1,\"paths\":[\"\\u0000\"]}",
        "{\"version\":1,\"paths\":[\"\\ud800\"]}",
        "{\"version\":1,\"paths\":[\"\\u00gg\"]}",
        "{\"version\":1,\"paths\":[\"\xff\"]}",
        "{\"version\":1,\"paths\":[\"SYS:\nno\"]}",
        "{\"version\":1,\"size\":04}",
        "{\"version\":1,\"size\":2147483649}",
        "{\"version\":1,\"zoom\":31}",
        "{\"version\":1,\"scale_percent\":0}",
        "{\"version\":1,\"scale_percent\":99}",
        "{\"version\":1,\"size\":96,\"scale_percent\":201}",
        "{\"version\":1,\"zoom\":64,\"scale_percent\":200}",
        "{\"version\":1,\"snapshot\":1}",
        "{\"version\":1,\"snapshot\":true,\"pip\":true}",
        "{\"version\":1,\"pip_x_offset\":129}",
        "{\"version\":1,\"edge\":\"top\"}",
        "{\"version\":1,}",
        "{\"version\":1} trailing"
    };
    DockPrefs current, original;
    char long_path[DOCK_PREFS_PATH_CAP+64];
    FILE *file;
    size_t i;
    dock_prefs_defaults(&original);
    original.count = 1;
    strcpy(original.paths[0],"SYS:Survivor");
    current = original;
    for (i = 0; i < sizeof(bad)/sizeof(*bad); ++i) {
        assert(!read_text(bad[i],&current));
        assert(!memcmp(&current,&original,sizeof(current)));
    }
    strcpy(long_path,"{\"version\":1,\"paths\":[\"");
    i = strlen(long_path);
    memset(long_path+i,'x',DOCK_PREFS_PATH_CAP);
    strcpy(long_path+i+DOCK_PREFS_PATH_CAP,"\"]}");
    assert(!read_text(long_path,&current));
    assert(!memcmp(&current,&original,sizeof(current)));

    file = tmpfile();
    assert(file);
    assert(fputs("{\"version\":1,\"paths\":[",file) != EOF);
    for (i = 0; i <= DOCK_MAX_ITEMS; ++i)
        assert(fputs(i ? ",\"SYS:Extra\"" : "\"SYS:First\"",file) != EOF);
    assert(fputs("]}",file) != EOF);
    rewind(file);
    assert(!dock_prefs_read(file,&current));
    assert(!memcmp(&current,&original,sizeof(current)));
    assert(fclose(file) == 0);
}

int main(void)
{
    test_roundtrip();
    test_edited_file();
    test_scale();
    test_bad_file();
    puts("JSON preferences: ordered paths, escapes, bounds and transactional read PASS");
    return 0;
}

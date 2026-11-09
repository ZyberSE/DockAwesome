/* Workbench-style launches for desktops that are not Workbench.
   workbench.library's OpenWorkbenchObjectA needs the Workbench process;
   Directory Opus 5 on 68k (AmiKit) replaces Workbench without patching it.
   This starts tools and projects the way Workbench does: LoadSeg, a new
   process with the icon's stack, and a WBStartup message whose reply says
   the program has ended. The program's code stays loaded until then. */
#include "amiga_app.h"
#include <rexx/storage.h>
#include <proto/rexxsyslib.h>
#include "amiga_folder.h"

struct RxsLib *RexxSysBase;

enum { NAME_CAP = 108, MIN_STACK = 4096 };

struct DockLaunched {
    struct WBStartup startup; /* first: the reply is this message */
    struct WBArg args[2];
    char names[2][NAME_CAP];
    char path[PATH_CAP];      /* what the dock was asked to open */
    struct DockLaunched *next;
};

/* The file `name` names, from `dir` unless it names a volume. */
static BPTR lock_in(BPTR dir, const char *name)
{
    BPTR old = CurrentDir(dir), lock = Lock((STRPTR)name,ACCESS_READ);
    CurrentDir(old);
    return lock;
}

/* A default tool by bare name, searched as Workbench would: next to the
   project, then Workbench's search path (Directory Opus answers this on
   68k too), then the usual system drawers. */
static BPTR find_tool(BPTR project_dir, const char *tool)
{
    static const char *const fallback[] = { "C:", "SYS:Utilities", "SYS:Tools", "SYS:System" };
    BPTR found = 0, path = 0;
    struct TagItem get[] = {{WBCTRLA_DuplicateSearchPath,(ULONG)&path},{TAG_DONE,0}};
    unsigned int i;
    if (strchr(tool,':') || strchr(tool,'/') || (found = lock_in(project_dir,tool)))
        return found ? found : lock_in(project_dir,tool);
    if (WorkbenchControlA(NULL,get) && path) {
        BPTR node;
        struct TagItem release[] = {{WBCTRLA_FreeSearchPath,(ULONG)path},{TAG_DONE,0}};
        /* A BCPL path list: {next, lock} per entry. */
        for (node = path; node && !found; node = ((BPTR *)BADDR(node))[0])
            found = lock_in(((BPTR *)BADDR(node))[1],tool);
        WorkbenchControlA(NULL,release);
        if (found) return found;
    }
    for (i = 0; i < sizeof(fallback)/sizeof(fallback[0]) && !found; ++i) {
        BPTR dir = Lock((STRPTR)fallback[i],ACCESS_READ);
        if (dir) { found = lock_in(dir,tool); UnLock(dir); }
    }
    return found;
}

/* Shows or clears the running dot that our own launches give `path`. */
static int mark_items(App *app, const char *path, int running)
{
    int i, changed = 0;
    for (i = 0; i < app->count; ++i) {
        Item *item = &app->items[i];
        int self = !!(item->running & RUNNING_SELF);
        if (Stricmp((STRPTR)item->path,(STRPTR)path) || self == running) continue;
        item->running ^= RUNNING_SELF;
        changed = 1;
    }
    return changed;
}

/* ARexx to Directory Opus 5 (port DOPUS.1). Commands from the main task
   are sent without waiting; their replies arrive on launch_port. The status
   worker asks and waits for answers. */
static struct RexxMsg *rexx_message(struct MsgPort *reply, const char *command, LONG action)
{
    struct RexxMsg *msg;
    if (!RexxSysBase || !(msg = CreateRexxMsg(reply,NULL,NULL))) return NULL;
    if (!(msg->rm_Args[0] = (STRPTR)CreateArgstring((CONST_STRPTR)command,strlen(command)))) {
        DeleteRexxMsg(msg);
        return NULL;
    }
    msg->rm_Action = action;
    return msg;
}

static void rexx_free(struct RexxMsg *msg)
{
    if ((msg->rm_Action & RXFF_RESULT) && !msg->rm_Result1 && msg->rm_Result2)
        DeleteArgstring((UBYTE *)msg->rm_Result2);
    DeleteArgstring((UBYTE *)msg->rm_Args[0]);
    DeleteRexxMsg(msg);
}

static int rexx_put(struct RexxMsg *msg)
{
    struct MsgPort *port;
    Forbid();
    if ((port = amiga_caps_dopus_port())) PutMsg(port,&msg->rm_Node);
    Permit();
    return port != NULL;
}

static int rexx_send(struct MsgPort *reply, const char *command)
{
    struct RexxMsg *msg = rexx_message(reply,command,RXCOMM);
    if (msg && !rexx_put(msg)) { rexx_free(msg); msg = NULL; }
    return msg != NULL;
}

/* Sends and waits. The result string, if any, goes to `result`. Returns
   the ARexx return code, or -1 when Directory Opus is not there. */
static LONG rexx_call(const char *command, char *result, size_t size)
{
    struct MsgPort *reply = CreateMsgPort();
    struct RexxMsg *msg = reply ? rexx_message(reply,command,RXCOMM|RXFF_RESULT) : NULL;
    LONG rc = -1;
    if (size) result[0] = 0;
    if (msg && rexx_put(msg)) {
        WaitPort(reply);
        GetMsg(reply);
        rc = msg->rm_Result1;
        if (!rc && msg->rm_Result2 && size) {
            strncpy(result,(const char *)msg->rm_Result2,size-1);
            result[size-1] = 0;
        }
        rexx_free(msg);
    } else if (msg) rexx_free(msg);
    if (reply) DeleteMsgPort(reply);
    return rc;
}

/* Fills args[n] with the parent of `file` and its name; takes `file`. */
static int set_arg(struct DockLaunched *launch, int n, BPTR file)
{
    struct FileInfoBlock *fib = AllocDosObject(DOS_FIB,NULL);
    BPTR parent = ParentDir(file);
    int ok = fib && parent && Examine(file,fib);
    if (ok) {
        strncpy(launch->names[n],(const char *)fib->fib_FileName,NAME_CAP-1);
        launch->args[n].wa_Lock = parent;
        launch->args[n].wa_Name = (STRPTR)launch->names[n];
    } else if (parent) UnLock(parent);
    if (fib) FreeDosObject(DOS_FIB,fib);
    UnLock(file);
    return ok;
}

static void launch_free(struct DockLaunched *launch)
{
    int i;
    if (launch->startup.sm_Segment) UnLoadSeg(launch->startup.sm_Segment);
    for (i = 0; i < 2; ++i) if (launch->args[i].wa_Lock) UnLock(launch->args[i].wa_Lock);
    FreeVec(launch);
}

static LONG icon_stack(const struct DiskObject *icon)
{
    return icon ? icon->do_StackSize : 0;
}

static int has_tool_type(const struct DiskObject *icon, const char *type)
{
    return icon && FindToolType((CONST_STRPTR *)icon->do_ToolTypes,(CONST_STRPTR)type) != NULL;
}

/* A "$STACK:<bytes>" string in the program's hunks, which Workbench
   honours since V45 (wb release notes, 45.165). */
static LONG embedded_stack(BPTR seglist)
{
    static const char key[] = "$STACK:";
    BPTR seg;
    for (seg = seglist; seg; seg = *(const BPTR *)BADDR(seg)) {
        const UBYTE *mem = (const UBYTE *)BADDR(seg);
        ULONG size = ((const ULONG *)mem)[-1], i;
        /* The allocation holds its size before and the next pointer at
           the start; data runs from mem+4 to the end of the allocation. */
        if (size < 16+sizeof(key)) continue;
        for (i = 4; i+sizeof(key)+4 < size-4; ++i) {
            LONG value;
            if (mem[i] == '$' && !memcmp(mem+i,key,sizeof(key)-1) &&
                StrToLong((STRPTR)mem+i+sizeof(key)-1,&value) > 0) return value;
        }
    }
    return 0;
}

/* Icons set to "Start from Shell" (tool type CLI) or "Start from ARexx"
   (REXX) run as Shell commands in the object's drawer, with an output
   window that opens only when the command writes. */
static int shell_launch(const char *path, const struct DiskObject *icon, LONG *error)
{
    char command[PATH_CAP*2+16], window[PATH_CAP+48];
    BPTR file = Lock((STRPTR)path,ACCESS_READ), dir = file ? ParentDir(file) : 0, old, input;
    LONG stack = icon_stack(icon), result;
    if (file) UnLock(file);
    if (!dir) { *error = IoErr() ? IoErr() : ERROR_OBJECT_NOT_FOUND; return 0; }
    if (has_tool_type(icon,"REXX")) snprintf(command,sizeof(command),"RX \"%s\"",path);
    else if (icon->do_Type == WBPROJECT && icon->do_DefaultTool && icon->do_DefaultTool[0])
        snprintf(command,sizeof(command),"\"%s\" \"%s\"",(const char *)icon->do_DefaultTool,path);
    else snprintf(command,sizeof(command),"\"%s\"",path);
    snprintf(window,sizeof(window),"CON:0/50//130/%s/AUTO/CLOSE/WAIT",FilePart((STRPTR)path));
    if (!(input = Open((STRPTR)window,MODE_OLDFILE))) input = Open((STRPTR)"NIL:",MODE_OLDFILE);
    if (stack < MIN_STACK) stack = MIN_STACK;
    old = CurrentDir(dir);
    result = SystemTags((STRPTR)command, SYS_Input,(ULONG)input, SYS_Output,0,
        SYS_Asynch,TRUE, SYS_UserShell,TRUE, NP_StackSize,(stack+3)&~3L, TAG_DONE);
    CurrentDir(old);
    UnLock(dir);
    if (result == -1) {
        *error = IoErr() ? IoErr() : ERROR_NO_FREE_STORE;
        if (input) Close(input);
        return 0;
    }
    return 1;
}

int launch_object(App *app, const char *path, LONG *error)
{
    struct DockLaunched *launch;
    struct DiskObject *icon, *tool_icon = NULL;
    LONG stack, embedded, priority = 0;
    BPTR tool_dir, old_dir, file, current, home;
    struct Process *process;
    const char *pri;
    *error = 0;
    if (!app->launch_port) { *error = ERROR_NO_FREE_STORE; return 0; }
    if (!(launch = AllocVec(sizeof(*launch),MEMF_PUBLIC|MEMF_CLEAR))) { *error = ERROR_NO_FREE_STORE; return 0; }
    strncpy(launch->path,path,PATH_CAP-1);
    {
        struct TagItem get[] = {{ICONGETA_FailIfUnavailable,FALSE},{TAG_DONE,0}};
        icon = GetIconTagList((STRPTR)path,get);
    }
    if (icon && icon->do_Type != WBTOOL && icon->do_Type != WBPROJECT) {
        /* Drawers and disks open in the desktop's own windows. */
        char command[PATH_CAP+16];
        snprintf(command,sizeof(command),"lister new \"%s\"",path);
        if (rexx_send(app->launch_port,command)) ++app->rexx_pending;
        else *error = ERROR_OBJECT_WRONG_TYPE;
        FreeDiskObject(icon);
        launch_free(launch);
        return !*error;
    }
    if (!icon) { *error = ERROR_OBJECT_NOT_FOUND; goto fail; }
    if (has_tool_type(icon,"CLI") || has_tool_type(icon,"REXX")) {
        int started = shell_launch(path,icon,error);
        FreeDiskObject(icon);
        launch_free(launch);
        return started;
    }
    if (!(file = Lock((STRPTR)path,ACCESS_READ))) { *error = IoErr(); goto fail; }
    if (icon->do_Type == WBPROJECT) {
        BPTR project_dir = ParentDir(file), tool;
        if (!icon->do_DefaultTool || !icon->do_DefaultTool[0]) {
            *error = ERROR_OBJECT_NOT_FOUND;
            UnLock(file); if (project_dir) UnLock(project_dir);
            goto fail;
        }
        tool = find_tool(project_dir,(const char *)icon->do_DefaultTool);
        if (project_dir) UnLock(project_dir);
        if (!tool) { *error = ERROR_OBJECT_NOT_FOUND; UnLock(file); goto fail; }
        if (!set_arg(launch,1,file)) { *error = IoErr(); UnLock(tool); goto fail; }
        file = tool;
    }
    if (!set_arg(launch,0,file)) { *error = IoErr(); goto fail; }
    launch->startup.sm_NumArgs = icon->do_Type == WBPROJECT ? 2 : 1;
    tool_dir = launch->args[0].wa_Lock;
    old_dir = CurrentDir(tool_dir);
    tool_icon = icon->do_Type == WBTOOL ? icon : GetDiskObject((STRPTR)launch->names[0]);
    launch->startup.sm_Segment = LoadSeg((STRPTR)launch->names[0]);
    if (!launch->startup.sm_Segment) *error = IoErr();
    CurrentDir(old_dir);
    if (!launch->startup.sm_Segment) goto fail;
    /* As workbench.library 45+: the opened icon's stack, raised to the
       tool icon's and to a $STACK: in the program when those are larger
       (RKM Libraries ch. 14; wb release notes 45.134, 45.165). TOOLPRI
       comes from the icon opened. */
    stack = icon_stack(icon);
    if (icon_stack(tool_icon) > stack) stack = icon_stack(tool_icon);
    if ((embedded = embedded_stack(launch->startup.sm_Segment)) > stack) stack = embedded;
    if (stack < MIN_STACK) stack = MIN_STACK;
    stack = (stack+3)&~3L;
    if ((pri = (const char *)FindToolType((CONST_STRPTR *)icon->do_ToolTypes,(CONST_STRPTR)"TOOLPRI"))) {
        LONG value;
        if (StrToLong((STRPTR)pri,&value) > 0) priority = dock_clamp(value,-128,127);
    }
    /* The new process owns both locks and frees them when it ends. */
    current = DupLock(tool_dir);
    home = DupLock(tool_dir);
    process = current && home ? CreateNewProcTags(NP_Seglist,(ULONG)launch->startup.sm_Segment,
        NP_FreeSeglist,FALSE, NP_StackSize,stack, NP_Priority,priority,
        NP_Name,(ULONG)launch->names[0], NP_CurrentDir,(ULONG)current,
        NP_HomeDir,(ULONG)home, NP_WindowPtr,0, NP_ConsoleTask,0,
        TAG_DONE) : NULL;
    if (!process) {
        *error = IoErr() ? IoErr() : ERROR_NO_FREE_STORE;
        if (current) UnLock(current);
        if (home) UnLock(home);
        goto fail;
    }
    launch->startup.sm_Message.mn_Node.ln_Type = NT_MESSAGE;
    launch->startup.sm_Message.mn_Node.ln_Name = NULL; /* not a RexxMsg */
    launch->startup.sm_Message.mn_ReplyPort = app->launch_port;
    launch->startup.sm_Message.mn_Length = sizeof(launch->startup);
    launch->startup.sm_Process = &process->pr_MsgPort;
    launch->startup.sm_ArgList = launch->args;
    launch->next = app->launched;
    app->launched = launch;
    PutMsg(&process->pr_MsgPort,&launch->startup.sm_Message);
    mark_items(app,path,1);
    if (tool_icon && tool_icon != icon) FreeDiskObject(tool_icon);
    FreeDiskObject(icon);
    return 1;
fail:
    if (tool_icon && tool_icon != icon) FreeDiskObject(tool_icon);
    if (icon) FreeDiskObject(icon);
    launch_free(launch);
    return 0;
}

int launch_active(const App *app, const char *path)
{
    const struct DockLaunched *launch;
    for (launch = app->launched; launch; launch = launch->next)
        if (!Stricmp((STRPTR)launch->path,(STRPTR)path)) return 1;
    return 0;
}

/* Frees the programs that ended; 1 when a pin's dot went out. */
int launch_reap(App *app)
{
    struct Message *msg;
    int ended = 0;
    if (!app->launch_port) return 0;
    while ((msg = GetMsg(app->launch_port))) {
        struct DockLaunched **link = &app->launched;
        while (*link && &(*link)->startup.sm_Message != msg) link = &(*link)->next;
        if (!*link) {
            struct RexxMsg *rexx = (struct RexxMsg *)msg;
            if (!RexxSysBase || !IsRexxMsg(rexx)) continue;
            if (rexx->rm_Result1 && !app->notice[0])
                snprintf(app->notice,sizeof(app->notice),"Directory Opus could not do\n%s",
                    (const char *)rexx->rm_Args[0]);
            rexx_free(rexx);
            --app->rexx_pending;
            continue;
        }
        {
            struct DockLaunched *launch = *link;
            *link = launch->next;
            if (!launch_active(app,launch->path)) ended |= mark_items(app,launch->path,0);
            launch_free(launch);
        }
    }
    return ended;
}

int launch_open(App *app)
{
    RexxSysBase = (struct RxsLib *)OpenLibrary((STRPTR)"rexxsyslib.library",36);
    app->launch_port = CreateMsgPort();
    return app->launch_port != NULL;
}

/* The programs' code is in the dock's memory until they reply: wait for
   every one. The dock's windows are already closed. */
void launch_close(App *app)
{
    if (!app->launch_port) return;
    while (app->launched || app->rexx_pending) {
        WaitPort(app->launch_port);
        launch_reap(app);
    }
    DeleteMsgPort(app->launch_port);
    app->launch_port = NULL;
    if (RexxSysBase) CloseLibrary((struct Library *)RexxSysBase);
    RexxSysBase = NULL;
}

int workbench_running(void)
{
    int running;
    Forbid();
    running = FindPort((STRPTR)"WORKBENCH") != NULL;
    Permit();
    return running;
}

int dopus_running(void)
{
    int running;
    Forbid();
    running = amiga_caps_dopus_port() != NULL;
    Permit();
    return running;
}

/* Workbench, unless --self-launch, opens what it can; otherwise we do. */
int self_launching(const App *app)
{
    return app->self_launch || !workbench_running();
}

void dopus_close_lister(App *app, ULONG lister)
{
    char command[32];
    snprintf(command,sizeof(command),"lister close %lu",(unsigned long)lister);
    if (rexx_send(app->launch_port,command)) ++app->rexx_pending;
}

/* Status worker: open Directory Opus listers count as open drawers. */
void dopus_drawers(ProgramPoll *poll)
{
    char handles[256], *cursor, *end;
    void *requesters;
    int i;
    if (rexx_call("lister query all",handles,sizeof(handles)) < 0) return;
    requesters = amiga_requesters_off(); /* a pin on an absent disk */
    for (i = 0; i < poll->count; ++i)
        if (poll->pins[i].kind == PIN_DRAWER) { poll->pins[i].running = 0; poll->pins[i].lister = 0; }
    for (cursor = handles; *cursor; cursor = end) {
        char command[48], path[PATH_CAP];
        ULONG lister = strtoul(cursor,&end,10);
        BPTR place;
        if (end == cursor) break;
        snprintf(command,sizeof(command),"lister query %lu path",(unsigned long)lister);
        if (rexx_call(command,path,sizeof(path)) || !(place = Lock((STRPTR)path,ACCESS_READ))) continue;
        for (i = 0; i < poll->count; ++i) {
            BPTR pin;
            if (poll->pins[i].kind != PIN_DRAWER || poll->pins[i].running) continue;
            if (!(pin = Lock((STRPTR)poll->pins[i].path,ACCESS_READ))) continue;
            if (SameLock(pin,place) == LOCK_SAME) {
                poll->pins[i].running = 1;
                poll->pins[i].lister = lister;
            }
            UnLock(pin);
        }
        UnLock(place);
    }
    amiga_requesters_restore(requesters);
    poll->valid |= PIN_DRAWER;
}

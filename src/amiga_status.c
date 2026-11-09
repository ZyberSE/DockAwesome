/* Workbench status: which pins are open. A worker process polls
   Workbench's program and drawer lists, window titles and Shells, so the main
   task never waits on a handler; the main task schedules polls and applies
   the results. Also window actions (focus, close) and Shell menu targets. */
#include "amiga_app.h"

static int shell_target_live(const ShellTarget *target)
{
    struct Process *process = FindCliProc(target->number);
    return process && process == target->process && process->pr_CIS &&
        process->pr_ConsoleTask == target->port;
}

/* Call under Forbid to read the process and its borrowed file handle. */
static int is_interactive_shell(const struct Process *process)
{
    const struct CommandLineInterface *cli = process ? BADDR(process->pr_CLI) : NULL;
    return cli && process->pr_CIS && process->pr_ConsoleTask &&
        cli->cli_StandardInput && IsInteractive(cli->cli_StandardInput);
}

/* Worker only: the console handler's reply can wait. Forbid keeps each
   process valid until its packet is queued. The main task validates the
   cached targets with shell_target_live before using them. */
static void poll_shells(ProgramPoll *poll)
{
    struct InfoData *info = poll->probe_consoles ?
        AllocVec(sizeof(*info),MEMF_PUBLIC|MEMF_CLEAR) : NULL;
    ULONG number, last = MaxCli();
    int i, running = 0;
    poll->shell_count = 0;
    poll->probed = info != NULL;
    for (number = 1; ; ++number) {
        struct Process *process;
        ShellTarget candidate = {0};
        int found;
        Forbid();
        process = FindCliProc(number);
        found = process != NULL;
        if (is_interactive_shell(process)) {
            running = 1;
            if (info && process->pr_CIS ==
                    ((struct CommandLineInterface *)BADDR(process->pr_CLI))->cli_StandardInput) {
                candidate.number = number;
                candidate.process = process;
                candidate.port = process->pr_ConsoleTask;
                memset(info,0,sizeof(*info));
                if (DoPkt(candidate.port,ACTION_DISK_INFO,MKBADDR(info),0,0,0,0)) {
                    candidate.window = (struct Window *)info->id_VolumeNode;
                    candidate.cooked = info->id_DiskType == 0x434f4e00UL; /* CON */
                }
            }
        }
        Permit();
        if (candidate.window && poll->shell_count < DOCK_MAX_ITEMS)
            poll->shells[poll->shell_count++] = candidate;
        /* ApolloDOS can report the last empty slot when the high slots are
           full. Include the occupied run just beyond its reported bound. */
        if (number > last && !found) break;
    }
    FreeVec(info);
    for (i = 0; i < poll->count; ++i)
        if (poll->pins[i].kind == PIN_SHELL) poll->pins[i].running = running;
    poll->valid |= PIN_SHELL;
}

static void poll_window_programs(ProgramPoll *poll, const struct List *programs);

static void poll_workbench_list(ProgramPoll *poll, int kind, ULONG get_tag, ULONG free_tag)
{
    struct List *list = NULL;
    struct TagItem get[] = {{get_tag,(ULONG)&list},{TAG_DONE,0}};
    struct TagItem release[] = {{free_tag,0},{TAG_DONE,0}};
    const struct Node *node;
    int i, success = WorkbenchControlA(NULL,get);
    if (kind == PIN_PROGRAM) {
        /* A failed Workbench query leaves its previous evidence intact.
           The window source can still report a Shell launch or closure. */
        if (success) for (i = 0; i < poll->count; ++i)
            if (poll->pins[i].kind == kind) poll->pins[i].running = 0;
    }
    if (!success || !list) {
        if (kind == PIN_PROGRAM) {
            poll_window_programs(poll,NULL);
            poll->valid |= kind;
        } else if (success) poll->unavailable |= kind;
        return;
    }
    for (node = list->lh_Head; node->ln_Succ; node = node->ln_Succ) {
        char absolute[PATH_CAP];
        const char *path = node->ln_Name;
        if (!path) continue;
        for (i = 0; i < poll->count; ++i)
            if (poll->pins[i].kind == kind &&
                !Stricmp((STRPTR)poll->pins[i].path,(STRPTR)path)) {
                poll->pins[i].running = kind == PIN_PROGRAM ? RUNNING_WORKBENCH : 1;
                break;
            }
        if (i < poll->count || kind != PIN_PROGRAM || !strchr(path,':')) continue;
        for (i = 0; i < poll->count; ++i) {
            const char *pin = poll->pins[i].path;
            if (poll->pins[i].kind != PIN_PROGRAM || poll->pins[i].running ||
                Stricmp(FilePart((STRPTR)pin),FilePart((STRPTR)path))) continue;
            if (path == node->ln_Name) {
                if (!canonical_path(path,absolute)) break;
                path = absolute;
            }
            if (!Stricmp((STRPTR)pin,(STRPTR)path)) {
                poll->pins[i].running = RUNNING_WORKBENCH;
                break;
            }
        }
    }
    if (kind == PIN_PROGRAM) poll_window_programs(poll,list);
    release[0].ti_Data = (ULONG)list;
    WorkbenchControlA(NULL,release);
    poll->valid |= kind;
}

/* Window chrome may add a label on either side. Single spaces are legal
   path characters and do not separate a path from that label. */
static int title_separator(const char *text)
{
    if (text[0] == ':' && text[1] == ' ') return 2;
    if (*text == '\t') return 1;
    if (*text != ' ') return 0;
    if (text[1] == ' ' || text[1] == '\t') return 1;
    return (text[1] == '-' || text[1] == '|') && text[2] == ' ' ? 3 : 0;
}

static const char *next_title_part(const char *part)
{
    int separator;
    while (*part && !title_separator(part)) ++part;
    while ((separator = title_separator(part))) part += separator;
    while (*part == ' ' || *part == '\t') ++part;
    return part;
}

static int title_names_document(const char *title, const char *path, size_t length)
{
    const char *part = title;
    while (*part) {
        if (*part == '"' && !Strnicmp((STRPTR)(part+1),(STRPTR)path,length) &&
            part[1+length] == '"') return 1;
        if (!Strnicmp((STRPTR)part,(STRPTR)path,length) &&
            (!part[length] || title_separator(part+length))) return 1;
        part = next_title_part(part);
    }
    return 0;
}

/* Extract only qualified title paths with the same file name as a pin.
   The caller validates the complete path through DOS after releasing IBase. */
static size_t title_document_alias(const char *title, const char *path, char *candidate)
{
    const char *name = strrchr(path,'/');
    const char *part;
    size_t length;
    if (!name) name = strrchr(path,':');
    if (!name || !*++name) return 0;
    length = strlen(name);
    for (part = title; *part; part = next_title_part(part)) {
        int quoted = *part == '"';
        const char *start = part+quoted, *volume = strchr(start,':'), *file;
        if (!volume || volume == start || volume[1] == ' ' ||
            memchr(start,'/',(size_t)(volume-start))) continue;
        for (file = volume+1; *file; ++file) {
            size_t n;
            const char *end;
            if (file[-1] != ':' && file[-1] != '/') continue;
            if (Strnicmp((STRPTR)file,(STRPTR)name,length)) continue;
            end = file+length;
            if (quoted ? *end != '"' : (*end && !title_separator(end))) continue;
            n = end-start;
            if (n >= PATH_CAP) continue;
            memcpy(candidate,start,n); candidate[n] = 0;
            return n;
        }
    }
    return 0;
}

static void poll_document_windows(ProgramPoll *poll)
{
    struct Screen *screen;
    char (*aliases)[PATH_CAP] = poll->aliases;
    size_t *lengths = poll->lengths;
    unsigned char alias_current[DOCK_MAX_ITEMS] = {0};
    ULONG lock;
    int i, alias_count = 0;
    for (i = 0; i < poll->count; ++i) {
        const char *path = poll->pins[i].path;
        const char *volume = strchr(path,':');
        lengths[i] = poll->pins[i].kind == PIN_DOCUMENT && volume && volume != path ? strlen(path) : 0;
    }
    lock = LockIBase(0);
    for (screen = IntuitionBase->FirstScreen; screen; screen = screen->NextScreen) {
        const struct Window *window;
        for (window = screen->FirstWindow; window; window = window->NextWindow) {
            char title[PATH_CAP+128];
            int exact = -1, best = -1;
            size_t n = 0;
            /* Title storage is owned by the window's application. Copy it
               without allowing that task to replace it halfway through. */
            Forbid();
            if (window->Title) while (n < sizeof(title)-1 && window->Title[n]) {
                title[n] = window->Title[n];
                ++n;
            }
            Permit();
            if (!n || n == sizeof(title)-1) continue;
            title[n] = 0;
            for (i = 0; i < poll->count; ++i) {
                if (!lengths[i]) continue;
                if (lengths[i] == n && !Stricmp((STRPTR)title,(STRPTR)poll->pins[i].path)) {
                    exact = i;
                    break;
                }
                if ((best < 0 || lengths[i] > lengths[best]) &&
                    title_names_document(title,poll->pins[i].path,lengths[i])) best = i;
            }
            if (exact >= 0) poll->pins[exact].running = 1;
            else if (best >= 0) poll->pins[best].running = 1;
            else if (alias_count < DOCK_MAX_ITEMS) {
                char candidate[PATH_CAP];
                size_t longest = 0;
                for (i = 0; i < poll->count; ++i) if (lengths[i]) {
                    size_t length = title_document_alias(title,poll->pins[i].path,candidate);
                    if (length > longest) {
                        strcpy(aliases[alias_count],candidate);
                        longest = length;
                    }
                }
                if (longest) ++alias_count;
            }
        }
    }
    UnlockIBase(lock);
    for (i = 0; i < alias_count; ++i) {
        char canonical[PATH_CAP];
        int pin;
        /* The regular title-only pass reuses the last verified DOS alias.
           A full refresh checks it again, as does any close request. */
        if (poll->query == PIN_DOCUMENT) {
            for (pin = 0; pin < poll->count; ++pin)
                if (lengths[pin] && !Stricmp((STRPTR)aliases[i],(STRPTR)poll->pins[pin].title_path)) break;
            if (pin < poll->count) {
                alias_current[pin] = 1;
                poll->pins[pin].running = 1;
                continue;
            }
        }
        if (!canonical_path(aliases[i],canonical)) continue;
        for (pin = 0; pin < poll->count; ++pin)
            if (lengths[pin] && !Stricmp((STRPTR)canonical,(STRPTR)poll->pins[pin].path)) {
                strcpy(poll->pins[pin].title_path,aliases[i]);
                alias_current[pin] = 1;
                poll->pins[pin].running = 1;
                break;
            }
    }
    for (i = 0; i < poll->count; ++i) if (!alias_current[i]) poll->pins[i].title_path[0] = 0;
    poll->valid |= PIN_DOCUMENT;
}

static DirectoryIdentity directory_identity(BPTR lock)
{
    const struct FileLock *file = BADDR(lock);
    return file ? (DirectoryIdentity){file->fl_Task,file->fl_Volume,file->fl_Key} :
        (DirectoryIdentity){0};
}

static int same_directory(DirectoryIdentity a, DirectoryIdentity b)
{
    return a.handler && a.handler == b.handler && a.volume == b.volume && a.key == b.key;
}

static DirectoryIdentity executable_directory(const char *path)
{
    char parent[PATH_CAP];
    size_t n = (const char *)FilePart((STRPTR)path)-path;
    DirectoryIdentity identity = {0};
    BPTR lock;
    if (n && path[n-1] == '/') --n;
    if (!n || n >= sizeof(parent)) return identity;
    memcpy(parent,path,n); parent[n] = 0;
    lock = Lock((STRPTR)parent,ACCESS_READ);
    if (lock) { identity = directory_identity(lock); UnLock(lock); }
    return identity;
}

/* Call with Intuition's window list locked and owner tasks unable to exit. */
static const struct Process *window_process(const struct Window *window)
{
    const struct MsgPort *port = window->UserPort;
    const struct Task *task = port && (port->mp_Flags & PF_ACTION) == PA_SIGNAL ? port->mp_SigTask : NULL;
    return task && task->tc_Node.ln_Type == NT_PROCESS ? (const struct Process *)task : NULL;
}

static void copy_cli_name(char dest[DOS_NAME_CAP], BSTR value)
{
    const unsigned char *name = BADDR(value);
    size_t length = name ? name[0] : 0;
    if (length) memcpy(dest,name+1,length);
    dest[length] = 0;
}

/* Capture process data while the owner cannot exit. A Shell runs commands
   inside its own task, so its task name is not the executable's name. */
static int process_program_name(const struct Process *owner, char name[DOS_NAME_CAP],
    char *current_dir)
{
    const struct CommandLineInterface *cli = BADDR(owner->pr_CLI);
    const char *task_name = owner->pr_Task.tc_Node.ln_Name;
    if (current_dir) current_dir[0] = 0;
    if (cli && cli->cli_Module) {
        copy_cli_name(name,cli->cli_CommandName);
        if (current_dir && name[0] && !strchr(name,':'))
            copy_cli_name(current_dir,cli->cli_SetName);
        if (name[0]) return 1;
    }
    if (!task_name || strlen(task_name) >= DOS_NAME_CAP) return 0;
    strcpy(name,task_name);
    return name[0] != 0;
}

static int program_path_matches(const WindowProgram *owner, const char *path)
{
    return !Stricmp(FilePart((STRPTR)owner->name),FilePart((STRPTR)path)) &&
        same_directory(owner->directory,executable_directory(path));
}

static void discover_path(ProgramPoll *poll, const char *path)
{
    DockImage source = {0};
    char absolute[PATH_CAP];
    int i;
    if (!strchr(path,':') || !canonical_path(path,absolute)) return;
    for (i = 0; i < poll->count; ++i)
        if (!Stricmp((STRPTR)poll->pins[i].path,(STRPTR)absolute)) return;
    if (!load_icon(poll->app,absolute,&source,NULL)) return;
    if (dock_cache_build(&poll->new_cache,&source,&poll->settings))
        strcpy(poll->new_path,absolute);
    free(source.pixels);
}

static void discover_program(ProgramPoll *poll, const WindowProgram *owner, const char *path)
{
    if (strchr(path,':') && program_path_matches(owner,path)) discover_path(poll,path);
}

/* A window a user can see as an application: it has a layer, is not a
   backdrop, is at least 16x16 and has a title, close gadget or drag bar.
   Call with IBase locked. */
static int visible_app_window(const struct Window *window)
{
    return window->WLayer && !(window->Flags & WFLG_BACKDROP) &&
        window->Width >= 16 && window->Height >= 16 &&
        ((window->Title && window->Title[0]) ||
         (window->Flags & (WFLG_CLOSEGADGET|WFLG_DRAGBAR)));
}

static int pin_by_id(const ProgramPoll *poll, uint32_t pin_id)
{
    int i;
    for (i = 0; i < poll->count; ++i) if (poll->pins[i].pin_id == pin_id) return i;
    return -1;
}

static void mark_owner(ProgramPoll *poll, int pin, const WindowProgram *owner)
{
    poll->pins[pin].running |= RUNNING_WINDOW;
    if (!poll->pins[pin].owner) poll->pins[pin].owner = &owner->process->pr_Task;
}

/* An app no pin explains, shown with the default tool icon and its task
   name. The name, without a volume, is the temporary item's path. */
static void discover_unnamed(ProgramPoll *poll, const WindowProgram *owner)
{
    DockImage source = {0};
    if (!owner->task_name[0] || strchr(owner->task_name,':') ||
        !load_icon(poll->app,owner->task_name,&source,NULL)) return;
    if (dock_cache_build(&poll->new_cache,&source,&poll->settings))
        strcpy(poll->new_path,owner->task_name);
    free(source.pixels);
}

/* Every process that shows a visible window is an app; background tasks
   are not. In order, an app belongs to: the pin it was attributed to, a
   pinned program with its path, the pin launched just before it appeared,
   a temporary icon for its program file, or a temporary default icon. */
static void poll_window_programs(ProgramPoll *poll, const struct List *programs)
{
    WindowProgram *owners = poll->owners;
    struct Screen *screen;
    const struct MsgPort *desktop;
    const struct Task *desktop_task;
    const DockLaunch *launch = NULL;
    int count = 0, i, k;
    ULONG lock = LockIBase(0);
    Forbid();
    desktop = FindPort((STRPTR)"WORKBENCH");
    desktop_task = desktop ? desktop->mp_SigTask : NULL;
    for (screen = IntuitionBase->FirstScreen; screen && count < DOCK_MAX_ITEMS; screen = screen->NextScreen) {
        const struct Window *window;
        for (window = screen->FirstWindow; window && count < DOCK_MAX_ITEMS; window = window->NextWindow) {
            const struct Process *owner = window_process(window);
            WindowProgram *entry = &owners[count];
            const char *task_name;
            int existing;
            if (!owner || &owner->pr_Task == poll->dock_task ||
                &owner->pr_Task == desktop_task || !window->WLayer) continue;
            for (existing = 0; existing < count; ++existing)
                if (owners[existing].process == owner) break;
            if (existing < count) {
                owners[existing].visible |= visible_app_window(window);
                continue;
            }
            task_name = owner->pr_Task.tc_Node.ln_Name;
            entry->process = owner;
            entry->visible = visible_app_window(window);
            entry->directory = directory_identity(owner->pr_HomeDir);
            entry->task_name[0] = 0;
            if (task_name && strlen(task_name) < DOS_NAME_CAP) strcpy(entry->task_name,task_name);
            if (!process_program_name(owner,entry->name,entry->current_dir)) entry->name[0] = 0;
            ++count;
        }
    }
    Permit();
    UnlockIBase(lock);
    /* Attributions end when the task no longer shows a visible window. */
    for (k = 0; k < poll->attribution_count; ) {
        for (i = 0; i < count; ++i)
            if (&owners[i].process->pr_Task == poll->attributions[k].task && owners[i].visible) break;
        if (i < count && pin_by_id(poll,poll->attributions[k].pin_id) >= 0) ++k;
        else poll->attributions[k] = poll->attributions[--poll->attribution_count];
    }
    for (k = poll->launch_count-1; k >= 0 && !launch; --k)
        if (poll->now-poll->launches[k].at <= poll->launch_ticks) launch = &poll->launches[k];
    for (i = 0; i < count; ++i) {
        const WindowProgram *owner = &owners[i];
        const struct Task *task = &owner->process->pr_Task;
        const struct Node *node;
        int pin, pinned = 0, seen = 0;
        for (k = 0; k < poll->seen_count; ++k) if (poll->seen[k] == task) seen = 1;
        for (k = 0; k < poll->attribution_count && !pinned; ++k)
            if (poll->attributions[k].task == task &&
                (pin = pin_by_id(poll,poll->attributions[k].pin_id)) >= 0) {
                mark_owner(poll,pin,owner);
                poll->pins[pin].attributed = 1;
                pinned = 1;
            }
        for (pin = 0; pin < poll->count && !pinned; ++pin)
            if (poll->pins[pin].kind == PIN_PROGRAM && owner->name[0] &&
                strchr(poll->pins[pin].path,':') && owner->directory.handler &&
                program_path_matches(owner,poll->pins[pin].path)) {
                mark_owner(poll,pin,owner);
                pinned = 1;
            }
        if (!pinned && launch && !seen && owner->visible &&
            (pin = pin_by_id(poll,launch->pin_id)) >= 0 &&
            poll->attribution_count < DOCK_MAX_ITEMS) {
            poll->attributions[poll->attribution_count].task = task;
            poll->attributions[poll->attribution_count++].pin_id = launch->pin_id;
            mark_owner(poll,pin,owner);
            poll->pins[pin].attributed = 1;
            pinned = 1;
        }
        for (pin = 0; pin < poll->count && !pinned; ++pin)
            if (poll->pins[pin].temporary && !strchr(poll->pins[pin].path,':') &&
                !Stricmp((STRPTR)poll->pins[pin].path,(STRPTR)owner->task_name)) {
                mark_owner(poll,pin,owner);
                pinned = 1;
            }
        if (pinned || !owner->visible || !poll->allow_new || poll->new_path[0]) continue;
        if (owner->name[0] && owner->directory.handler) {
            if (programs)
                for (node = programs->lh_Head; node->ln_Succ && !poll->new_path[0]; node = node->ln_Succ)
                    if (node->ln_Name) discover_program(poll,owner,node->ln_Name);
            if (!poll->new_path[0] && strchr(owner->name,':')) discover_program(poll,owner,owner->name);
            else if (!poll->new_path[0] && owner->current_dir[0]) {
                char path[PATH_CAP];
                strcpy(path,owner->current_dir);
                if (AddPart((STRPTR)path,(STRPTR)owner->name,sizeof(path)))
                    discover_program(poll,owner,path);
                if (!poll->new_path[0] && !strchr(owner->name,'/')) {
                    strcpy(path,"C:");
                    if (AddPart((STRPTR)path,(STRPTR)owner->name,sizeof(path)))
                        discover_program(poll,owner,path);
                }
            }
        }
        if (!poll->new_path[0]) discover_unnamed(poll,owner);
    }
    for (poll->seen_count = 0; poll->seen_count < count; ++poll->seen_count)
        poll->seen[poll->seen_count] = &owners[poll->seen_count].process->pr_Task;
}

/* Document-only polls skip the window pass: keep attributed pins running
   while their task still shows a visible window. */
static void apply_attributions(ProgramPoll *poll)
{
    int k;
    for (k = 0; k < poll->attribution_count; ++k) {
        const struct Screen *screen;
        int pin = pin_by_id(poll,poll->attributions[k].pin_id), shown = 0;
        ULONG lock;
        if (pin < 0 || poll->pins[pin].attributed) continue;
        lock = LockIBase(0);
        Forbid();
        for (screen = IntuitionBase->FirstScreen; screen && !shown; screen = screen->NextScreen) {
            const struct Window *window;
            for (window = screen->FirstWindow; window && !shown; window = window->NextWindow)
                shown = window_process(window) &&
                    &window_process(window)->pr_Task == poll->attributions[k].task &&
                    visible_app_window(window);
        }
        Permit();
        UnlockIBase(lock);
        if (!shown) continue;
        poll->pins[pin].attributed = 1;
        poll->pins[pin].owner = poll->attributions[k].task;
    }
}

/* Call with IBase locked. */
static int window_listed(const struct Window *target)
{
    const struct Screen *screen;
    const struct Window *window;
    for (screen = IntuitionBase->FirstScreen; screen; screen = screen->NextScreen)
        for (window = screen->FirstWindow; window; window = window->NextWindow)
            if (window == target) return 1;
    return 0;
}

enum { WINDOW_CALL_SCREEN, WINDOW_CALL_FRONT, WINDOW_CALL_ACTIVATE, WINDOW_CALL_CLOSE };

/* Revalidate under IBase immediately before each Intuition call, never while
   holding it. Forbid keeps the owner from closing the window until Intuition
   has the request. A call that waits cannot invalidate the next call's check. */
static int window_call(struct Window *window, int call)
{
    enum { SEND_IDCMP_CLOSE = 3 };
    ULONG lock = LockIBase(0);
    int live;
    Forbid();
    live = window_listed(window);
    UnlockIBase(lock);
    if (live) {
        if (call == WINDOW_CALL_SCREEN) ScreenToFront(window->WScreen);
        else if (call == WINDOW_CALL_FRONT) WindowToFront(window);
        else if (call == WINDOW_CALL_ACTIVATE) ActivateWindow(window);
        else amiga_window_action(window,SEND_IDCMP_CLOSE,NULL);
    }
    Permit();
    return live;
}

/* AmigaOS 3.x has no WindowAction: post IDCMP_CLOSEWINDOW as the close
   gadget would. The message is ours: wait up to a second for the reply,
   and never free one that was not replied (the app may still reply, so its
   port stays too). Intuition's CloseWindow replies messages still queued. */
static int send_close_message(struct Window *window)
{
    struct MsgPort *reply = CreateMsgPort();
    struct IntuiMessage *message;
    ULONG lock;
    int live, i, replied = 0;
    if (!reply) return 0;
    message = AllocVec(sizeof(struct ExtIntuiMessage),MEMF_PUBLIC|MEMF_CLEAR);
    if (!message) { DeleteMsgPort(reply); return 0; }
    message->ExecMessage.mn_Node.ln_Type = NT_MESSAGE;
    message->ExecMessage.mn_ReplyPort = reply;
    message->ExecMessage.mn_Length = sizeof(struct IntuiMessage);
    message->Class = IDCMP_CLOSEWINDOW;
    CurrentTime(&message->Seconds,&message->Micros);
    lock = LockIBase(0);
    Forbid();
    live = window_listed(window) && window->UserPort && (window->IDCMPFlags & IDCMP_CLOSEWINDOW);
    UnlockIBase(lock);
    if (live) {
        message->IDCMPWindow = window;
        message->IAddress = window;
        message->MouseX = window->MouseX;
        message->MouseY = window->MouseY;
        PutMsg(window->UserPort,&message->ExecMessage);
    }
    Permit();
    if (!live) { FreeVec(message); DeleteMsgPort(reply); return 0; }
    for (i = 0; i < 50 && !replied; ++i) {
        if (GetMsg(reply)) replied = 1;
        else Delay(1);
    }
    if (replied) { FreeVec(message); DeleteMsgPort(reply); }
    return 1;
}

static void focus_window(struct Window *window)
{
    if (window_call(window,WINDOW_CALL_SCREEN) && window_call(window,WINDOW_CALL_FRONT))
        window_call(window,WINDOW_CALL_ACTIVATE);
}

/* Use the worker's console probe. Prefer the active Shell, then layer order
   on the Dock's screen. No DOS packet is sent from the main task. */
ShellTarget shell_menu_target(const App *app)
{
    struct Screen *screen = app->screen;
    ShellTarget targets[DOCK_MAX_ITEMS], chosen = {0};
    const struct Window *active = IntuitionBase->ActiveWindow;
    const struct Layer *layer;
    int count = 0, i;
    Forbid();
    for (i = 0; i < app->shell_count; ++i)
        if (shell_target_live(&app->shells[i])) targets[count++] = app->shells[i];
    Permit();
    for (i = 0; i < count; ++i) if (targets[i].window == active) return targets[i];
    if (!count || !AttemptSemaphore(&screen->LayerInfo.Lock)) return chosen;
    for (layer = screen->LayerInfo.top_layer; layer && !chosen.window; layer = layer->back)
        for (i = 0; i < count; ++i)
            if (layer->Window == targets[i].window) { chosen = targets[i]; break; }
    ReleaseSemaphore(&screen->LayerInfo.Lock);
    return chosen;
}

int show_shell_window(const App *app, const ShellTarget *selected)
{
    struct Window *target = selected->window;
    ULONG lock;
    int i, live;
    if (!target) {
        Forbid();
        for (i = 0; i < app->shell_count && !target; ++i)
            if (shell_target_live(&app->shells[i])) target = app->shells[i].window;
        Permit();
        if (!target) return 0;
    }
    lock = LockIBase(0);
    Forbid();
    live = (!selected->window || shell_target_live(selected)) && window_listed(target);
    UnlockIBase(lock);
    Permit();
    if (live) focus_window(target);
    return live;
}

InputHandlerCode apollo_console_close_handler(const App *app)
{
    const struct Library *console;
    InputHandlerCode code = NULL;
    enum { CD_INPUT_HANDLER_LVO = 42 };
    /* The close dispatch is specific to the audited Apollo implementations
       (caps.apollo_console). Validate the installed handler against
       console.device's own vector. */
    if (!app->caps.apollo_console) return NULL;
    Forbid();
    console = (const struct Library *)FindName(&SysBase->DeviceList,(STRPTR)"console.device");
    if (console && console->lib_NegSize >= CD_INPUT_HANDLER_LVO) {
        const unsigned char *vector = (const unsigned char *)console-CD_INPUT_HANDLER_LVO;
        if (vector[0] == 0x4e && vector[1] == 0xf9) memcpy(&code,vector+2,sizeof(code));
    }
    Permit();
    return code;
}

/* Validate and activate on the main task so input.device never blocks.
   Activation is asynchronous; wait up to 200 ms for it. The handler then
   delivers the close only if the window is still the active one. */
static int prepare_shell_close(const ShellTarget *target)
{
    int attempt;
    for (attempt = 0; attempt < 10; ++attempt) {
        ULONG lock = LockIBase(0);
        struct Window *window;
        int live, active;
        Forbid();
        live = shell_target_live(target);
        window = IntuitionBase->FirstScreen ? IntuitionBase->FirstScreen->FirstWindow : NULL;
        for (; window; window = window->NextWindow)
            if (window == target->window) break;
        live = live && window && !window->FirstRequest && (window->Flags & WFLG_CLOSEGADGET);
        active = live && IntuitionBase->ActiveWindow == window;
        UnlockIBase(lock);
        if (live && !active) ActivateWindow(window);
        Permit();
        if (!live) return 0;
        if (active) return 1;
        Delay(1);
    }
    return 0;
}

int quit_shell_window(App *app)
{
    ShellClose close = {app->menu_shell,&app->input_handler,apollo_console_close_handler(app),0};
    struct InputEvent event = {0};
    LONG error;
    if (!close.target.window || !close.target.cooked || !close.code ||
        !prepare_shell_close(&close.target)) return 0;
    event.ie_EventAddress = &close;
    app->input_context.shell_close = &close;
    app->input_io->io_Command = IND_WRITEEVENT;
    app->input_io->io_Data = &event;
    app->input_io->io_Length = sizeof(event);
    error = DoIO((struct IORequest *)app->input_io);
    app->input_context.shell_close = NULL;
    return !error && close.sent;
}

static int document_window_matches(const App *app, const Item *item,
    const char *title, const char *alias)
{
    int i;
    if (!title || (!title_names_document(title,item->path,strlen(item->path)) &&
        (!alias || !title_names_document(title,alias,strlen(alias))))) return 0;
    /* A title that could refer to two pins is not a safe close target. */
    for (i = 0; i < app->count; ++i) {
        const Item *other = &app->items[i];
        if (other != item && other->kind == PIN_DOCUMENT &&
            (title_names_document(title,other->path,strlen(other->path)) ||
             (other->title_path[0] &&
              title_names_document(title,other->title_path,strlen(other->title_path))))) return 0;
    }
    return 1;
}

int item_window_action(const App *app, const Item *item, int action)
{
    DirectoryIdentity directory = {0};
    const char *name = NULL, *alias = NULL;
    char canonical[PATH_CAP];
    struct Window *found = NULL;
    struct Screen *screen;
    ULONG lock;
    int matches = 0;
    if (item->owner) {
        /* The task known to show this item's window, whatever its name. */
    } else if (item->kind == PIN_PROGRAM) {
        directory = executable_directory(item->path);
        if (!directory.handler) return 0;
        name = (const char *)FilePart((STRPTR)item->path);
    } else if (item->kind != PIN_DOCUMENT || !strchr(item->path,':')) return 0;
    else if (item->title_path[0] && canonical_path(item->title_path,canonical) &&
        !Stricmp((STRPTR)canonical,(STRPTR)item->path)) alias = item->title_path;
    lock = LockIBase(0);
    Forbid();
    for (screen = IntuitionBase->FirstScreen; screen; screen = screen->NextScreen) {
        struct Window *window;
        for (window = screen->FirstWindow; window; window = window->NextWindow) {
            if (item->owner) {
                const struct Process *owner = window_process(window);
                if (!owner || &owner->pr_Task != item->owner || !visible_app_window(window)) continue;
            } else if (name) {
                const struct Process *owner = window_process(window);
                char command[DOS_NAME_CAP];
                if (!owner || !process_program_name(owner,command,NULL) ||
                    Stricmp((STRPTR)name,FilePart((STRPTR)command)) ||
                    !same_directory(directory,directory_identity(owner->pr_HomeDir))) continue;
            } else if (!document_window_matches(app,item,(const char *)window->Title,alias)) continue;
            ++matches;
            if (!found && (action == WINDOW_FOCUS || (window->UserPort &&
                (window->IDCMPFlags & IDCMP_CLOSEWINDOW) && (window->Flags & WFLG_CLOSEGADGET) &&
                !window->FirstRequest))) found = window;
        }
    }
    UnlockIBase(lock);
    Permit();
    /* Each call below revalidates the window before it runs. */
    if (found && action == WINDOW_FOCUS) focus_window(found);
    else if (found && matches == 1 && action == WINDOW_CLOSE) {
        if (app->caps.window_action) window_call(found,WINDOW_CALL_CLOSE);
        else send_close_message(found);
    }
    return found && (action == WINDOW_FOCUS || matches == 1);
}

static LONG program_worker_entry(void)
{
    struct Process *self = (struct Process *)FindTask(NULL);
    for (;;) {
        ProgramPoll *poll;
        WaitPort(&self->pr_MsgPort);
        poll = (ProgramPoll *)GetMsg(&self->pr_MsgPort);
        if (!poll) continue;
        if (poll->stop) {
            /* Return to DOS before the caller can unload our program code. */
            Forbid();
            ReplyMsg(&poll->message);
            return 0;
        }
        poll->valid = poll->unavailable = 0;
        if (poll->query & PIN_PROGRAM)
            poll_workbench_list(poll,PIN_PROGRAM,WBCTRLA_GetProgramList,WBCTRLA_FreeProgramList);
        if ((poll->query & PIN_DRAWER) && poll->dopus_drawers) dopus_drawers(poll);
        else if (poll->query & PIN_DRAWER)
            poll_workbench_list(poll,PIN_DRAWER,WBCTRLA_GetOpenDrawerList,WBCTRLA_FreeOpenDrawerList);
        if (poll->query & PIN_DOCUMENT) poll_document_windows(poll);
        if (poll->query & PIN_SHELL) poll_shells(poll);
        apply_attributions(poll);
        ReplyMsg(&poll->message);
    }
}

/* Native notifications include our tooltip and popup. Their windows do not
   change the Workbench objects being tracked. */
static uint32_t status_window_stamp(const App *app)
{
    struct Screen *screen;
    ULONG lock = LockIBase(0);
    uint32_t stamp = UINT32_C(2166136261);
    for (screen = IntuitionBase->FirstScreen; screen; screen = screen->NextScreen) {
        const struct Window *window;
        for (window = screen->FirstWindow; window; window = window->NextWindow) {
            if (window == app->window || dock_ui_owns_window(app->ui,window) ||
                amiga_drag_preview_owns(app->drag_preview,window)) continue;
            stamp = (stamp ^ (uintptr_t)window) * UINT32_C(16777619);
            stamp = (stamp ^ (uintptr_t)window->UserPort) * UINT32_C(16777619);
        }
    }
    UnlockIBase(lock);
    return stamp;
}

void program_poll_open(App *app)
{
    if (!app->live_open) return;
    app->program_replies = CreateMsgPort();
    if (!app->program_replies) return;
    app->program_worker = CreateNewProcTags(NP_Entry,(ULONG)program_worker_entry,
        NP_Name,(ULONG)"DockAwesome Workbench status",NP_Priority,0,
        NP_StackSize,49152,NP_CopyVars,FALSE,NP_WindowPtr,(ULONG)-1,TAG_DONE);
    app->program_poll.app = app;
    app->program_poll.dock_task = FindTask(NULL);
    app->program_poll.message.mn_ReplyPort = app->program_replies;
    app->program_poll.message.mn_Length = offsetof(ProgramPoll,owners);
    if (app->program_worker) app->status_window_stamp = status_window_stamp(app);
    if (app->program_worker && app->caps.status_notify) {
        LONG bit = AllocSignal(-1);
        if (bit >= 0) {
            enum { AFTER_OPENWINDOW = 1<<4, AFTER_CLOSEWINDOW = 1<<10 };
            struct TagItem tags[] = {
                {TAG_USER+2,AFTER_OPENWINDOW|AFTER_CLOSEWINDOW},
                {TAG_USER+4,(ULONG)FindTask(NULL)}, {TAG_USER+5,(ULONG)bit}, {TAG_DONE,0}
            };
            app->status_notify = amiga_start_status_notify(tags);
            if (app->status_notify) app->status_notify_bit = bit;
            else FreeSignal(bit);
        }
    }
}

void status_observe_windows(App *app)
{
    uint32_t stamp = status_window_stamp(app);
    if (stamp == app->status_window_stamp) return;
    app->status_window_stamp = stamp;
    if (app->profile) ++app->stats.status_events;
    status_changed(app);
}

static int room_for_running_icon(const App *app)
{
    return dock_items_fit(&app->settings,app->count+1,viewport_limit(app,app->edge));
}

void program_poll_schedule(App *app)
{
    ProgramPoll *poll = &app->program_poll;
    uint64_t now;
    int i, query = 0, kinds = PIN_PROGRAM;
    if (!app->program_worker || app->program_poll_pending ||
        animation_needed(app) ||
        app->gesture.active || buttons_engaged(app,ANY_BUTTON) || dock_ui_busy(app->ui) ||
        IntuitionBase->FirstScreen != app->screen) return;
    now = profile_clock();
    /* Classic Intuition has no native notifications. A cheap window-list
       comparison replaces the per-second Workbench round trips there. */
    if (!app->status_notify && now >= app->status_watch_at) {
        app->status_watch_at = now+app->clock_hz/2;
        status_observe_windows(app);
    }
    if (now < app->program_poll_at && now < app->document_poll_at &&
        (!app->status_recheck || now < app->status_event_at) &&
        (!app->discovery_at || now < app->discovery_at) && !app->shell_probe_wanted) return;
    for (i = 0; i < app->count; ++i) kinds |= app->items[i].kind;
    if (app->shell_probe_wanted) query |= kinds & PIN_SHELL;
    if (now >= app->program_poll_at || (app->status_recheck && now >= app->status_event_at)) {
        query |= kinds;
        app->program_poll_at = now+(uint64_t)app->clock_hz*8;
        if (app->status_recheck) {
            --app->status_recheck;
            app->status_event_at = now+(uint64_t)app->clock_hz*3/4;
        }
    }
    if (app->discovery_at && now >= app->discovery_at) {
        query |= PIN_PROGRAM;
        app->discovery_at = 0;
    }
    if (now >= app->document_poll_at) query |= kinds & PIN_DOCUMENT;
    if (now >= app->document_poll_at || (query & PIN_DOCUMENT)) app->document_poll_at = now+app->clock_hz;
    query &= ~app->status_unavailable;
    if (!query) return;
    app->program_poll_started = now;
    poll->count = app->count;
    poll->query = query;
    poll->settings = app->settings;
    poll->allow_new = room_for_running_icon(app);
    poll->dopus_drawers = self_launching(app) && dopus_running();
    poll->new_path[0] = 0;
    for (i = 0; i < poll->count; ++i) {
        strcpy(poll->pins[i].path,app->items[i].path);
        strcpy(poll->pins[i].title_path,app->items[i].title_path);
        poll->pins[i].pin_id = app->items[i].pin_id;
        poll->pins[i].running = app->items[i].kind == PIN_PROGRAM ?
            app->items[i].running & RUNNING_WORKBENCH : 0;
        poll->pins[i].kind = app->items[i].kind;
        poll->pins[i].temporary = app->items[i].temporary;
        poll->pins[i].attributed = 0;
        poll->pins[i].owner = NULL;
        poll->pins[i].lister = app->items[i].lister;
    }
    poll->now = now;
    poll->launch_ticks = (uint64_t)app->clock_hz*LAUNCH_SECONDS;
    poll->launch_count = app->launch_count;
    memcpy(poll->launches,app->launches,sizeof(poll->launches));
    poll->probe_consoles = app->shell_probe_wanted && (query & PIN_SHELL);
    app->shell_probe_wanted = 0;
    app->program_poll_pending = 1;
    PutMsg(&app->program_worker->pr_MsgPort,&poll->message);
}

int program_poll_result(App *app, int *recreate)
{
    ProgramPoll *poll = &app->program_poll;
    int i, before = app->count, changed = 0, layout_changed = 0, added = -1;
    if (!app->program_replies || !GetMsg(app->program_replies)) return 0;
    app->program_poll_pending = 0;
    app->status_unavailable |= poll->unavailable;
    if ((poll->valid & PIN_SHELL) && poll->probed) {
        memcpy(app->shells,poll->shells,sizeof(app->shells));
        app->shell_count = poll->shell_count;
        app->shells_at = profile_clock();
    }
    if (poll->new_path[0]) app->discovery_at = profile_clock()+app->clock_hz/5;
    for (i = 0; i < poll->count; ++i) {
        int pin, running;
        if (!(poll->valid & poll->pins[i].kind) && !poll->pins[i].attributed) continue;
        pin = pin_index(app,poll->pins[i].pin_id);
        if (pin < 0) continue;
        running = poll->pins[i].running || poll->pins[i].attributed;
        if (launch_active(app,app->items[pin].path)) running |= RUNNING_SELF;
        app->items[pin].owner = running ? poll->pins[i].owner : NULL;
        if (poll->pins[i].kind == PIN_DRAWER) app->items[pin].lister = running ? poll->pins[i].lister : 0;
        if (poll->pins[i].kind == PIN_DOCUMENT)
            strcpy(app->items[pin].title_path,poll->pins[i].title_path);
        if (!!app->items[pin].running != !!running) changed = 1;
        app->items[pin].running = running;
    }
    if (!*recreate && !dock_ui_busy(app->ui) && !app->gesture.active &&
        !buttons_engaged(app,IEQUALIFIER_LEFTBUTTON|IEQUALIFIER_RBUTTON)) {
        if (poll->valid & PIN_PROGRAM) for (i = app->count-1; i >= 0; --i) {
            if (app->items[i].temporary && !app->items[i].running) {
                erase_item(app,i);
                layout_changed = 1;
            }
        }
        if (poll->new_path[0] && room_for_running_icon(app) &&
            app->settings.base_size == poll->settings.base_size &&
            app->settings.max_size == poll->settings.max_size && app->settings.levels == poll->settings.levels &&
            item_path_index(app,poll->new_path) < 0) {
            Item *item;
            input_invalidate(app);
            item = append_item(app,poll->new_path,PIN_PROGRAM,&poll->new_cache);
            item->running = RUNNING_WINDOW;
            item->temporary = 1;
            added = app->count-1;
            layout_changed = 1;
        }
        /* Like drops and unpins: a new icon appears in its slot and the rest
           glide to the new row. Snapping the whole row made the dock jump
           when a temporary icon's program quit. */
        if (layout_changed) {
            *recreate = !rest_after_pin_change(app,before);
            if (!*recreate && added >= 0) app->placement[added] = app->target[added];
        }
    }
    changed |= layout_changed;
    dock_cache_free(&poll->new_cache);
    if (app->profile) {
        uint64_t ticks = profile_clock()-app->program_poll_started;
        ++app->stats.program_polls;
        if (poll->query & PIN_PROGRAM) ++app->stats.program_checks;
        if (poll->query & PIN_DRAWER) ++app->stats.drawer_checks;
        if (poll->query & PIN_DOCUMENT) ++app->stats.document_checks;
        app->stats.program_changes += changed;
        app->stats.program_poll_ticks += ticks;
        if (ticks > app->stats.program_poll_peak) app->stats.program_poll_peak = ticks;
    }
    return changed;
}

void program_poll_close(App *app)
{
    ProgramPoll *poll = &app->program_poll;
    if (app->status_notify) {
        /* Intuition signals this task until the notification ends, so the
           process must not exit before then: waiting is safer than a crash.
           Back off after 5 s instead of spinning. */
        int attempt;
        for (attempt = 0; !amiga_end_status_notify(app->status_notify); ++attempt)
            Delay(attempt < 250 ? 1 : 25);
        FreeSignal(app->status_notify_bit);
        app->status_notify = NULL;
    }
    if (app->program_worker) {
        if (app->program_poll_pending) {
            WaitPort(app->program_replies);
            GetMsg(app->program_replies);
        }
        poll->stop = 1;
        PutMsg(&app->program_worker->pr_MsgPort,&poll->message);
        WaitPort(app->program_replies);
        GetMsg(app->program_replies);
    }
    if (app->program_replies) DeleteMsgPort(app->program_replies);
    dock_cache_free(&poll->new_cache);
}

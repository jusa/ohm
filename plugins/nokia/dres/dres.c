#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>

#include <glib.h>
#include <gmodule.h>
#include <dbus/dbus.h>

#include <dres/dres.h>
#include <prolog/prolog.h>
#include <prolog/ohm-fact.h>           /* XXX */

#include <ohm-plugin.h>

#define CONSOLE_PROMPT "ohm-dres> "


typedef void (*completion_cb_t)(int transid, int success);

/* rule engine methods */
OHM_IMPORTABLE(int , prolog_setup , (char **extensions, char **files));
OHM_IMPORTABLE(void, prolog_free  , (void *retval));
OHM_IMPORTABLE(void, prolog_dump  , (void *retval));
OHM_IMPORTABLE(void, prolog_shell , (void));

OHM_IMPORTABLE(prolog_predicate_t *, prolog_lookup ,
               (char *name, int arity));
OHM_IMPORTABLE(int                 , prolog_invoke ,
               (prolog_predicate_t *pred, void *retval, ...));
OHM_IMPORTABLE(int                 , prolog_vinvoke,
               (prolog_predicate_t *pred, void *retval, va_list ap));
OHM_IMPORTABLE(int                 , prolog_ainvoke,
               (prolog_predicate_t *pred, void *retval,void **args, int narg));

#if 1
OHM_IMPORTABLE(int, signal_changed,(char *signal, int transid,
                                    int factc, char **factv,
                                    completion_cb_t callback,
                                    unsigned long timeout));
#else
int signal_changed(char *, int, int, char**, completion_cb_t, unsigned long);
#endif


/* console methods */
OHM_IMPORTABLE(int, console_open , (char *address,
                                    void (*opened)(int, struct sockaddr *, int),
                                    void (*closed)(int),
                                    void (*input)(int, char *, void *),
                                    void  *data, int multiple));
OHM_IMPORTABLE(int, console_close , (int id));
OHM_IMPORTABLE(int, console_write , (int id, char *buf, size_t size));
OHM_IMPORTABLE(int, console_printf, (int id, char *fmt, ...));

OHM_IMPORTABLE(void, completion_cb, (int transid, int success));

static int  prolog_handler (dres_t *dres,
                            char *name, dres_action_t *action, void **ret);
static int  signal_handler (dres_t *dres,
                            char *name, dres_action_t *action, void **ret);
static void dump_signal_changed_args(char *signame, int transid, int factc,
                                     char**factv, completion_cb_t callback,
                                     unsigned long timeout);
static int  retval_to_facts(char ***objects, OhmFact **facts, int max);

static void console_opened (int id, struct sockaddr *peer, int peerlen);
static void console_closed (int id);
static void console_input  (int id, char *buf, void *data);



static dres_t *dres;
static int     console;
 

/*****************************************************************************
 *                       *** initialization & cleanup ***                    *
 *****************************************************************************/

/**
 * plugin_init:
 **/
static void
plugin_init(OhmPlugin *plugin)
{
#define DRES_RULE_PATH "/tmp/policy/policy.dres"
#define PROLOG_SYSDIR  "/usr/share/prolog/"
#define PROLOG_RULEDIR "/tmp/policy/prolog/"

#define FAIL(fmt, args...) do {                                   \
        g_warning("DRES plugin, %s: "fmt, __FUNCTION__, ## args); \
        goto fail;                                                \
    } while (0)

    char *extensions[] = {
        PROLOG_SYSDIR"extensions/fact",
        PROLOG_SYSDIR"extensions/relation",
        NULL
    };

    char *rules[] = {
        PROLOG_RULEDIR"hwconfig",
        PROLOG_RULEDIR"devconfig",
        PROLOG_RULEDIR"interface",
        PROLOG_RULEDIR"profile",
        PROLOG_RULEDIR"audio",
#if 0
        PROLOG_RULEDIR"test",
#endif
        NULL
    };

    if ((dres = dres_init(NULL)) == NULL)
        FAIL("failed to initialize DRES library");
    
    if (dres_register_handler(dres, "prolog", prolog_handler) != 0)
        FAIL("failed to register DRES prolog handler");

    if (dres_register_handler(dres, "signal_changed", signal_handler) != 0)
        FAIL("failed to register DRES signal_changed handler");

    if (dres_parse_file(dres, DRES_RULE_PATH))
        FAIL("failed to parse DRES rule file \"%s\"", DRES_RULE_PATH);

    if (prolog_setup(extensions, rules) != 0)
        FAIL("failed to load extensions and rules to prolog interpreter");
    
    console = console_open("127.0.0.1:2000",
                           console_opened, console_closed, console_input,
                           NULL, FALSE);
    if (console < 0)
        g_warning("DRES plugin, %s: failed to open console", __FUNCTION__);
    
    return;

 fail:
    if (dres) {
        dres_exit(dres);
        dres = NULL;
    }
    exit(1);

#undef FAIL
}


/**
 * plugin_exit:
 **/
static void
plugin_exit(OhmPlugin *plugin)
{
    if (dres) {
        dres_exit(dres);
        dres = NULL;
    }

#if 0
    if (console > 0)
        console_close(console);
    console = 0;
#endif
}


/********************
 * dres_parse_error
 ********************/
void
dres_parse_error(dres_t *dres, int lineno, const char *msg, const char *token)
{
    g_warning("error: %s, on line %d near input %s\n", msg, lineno, token);
    exit(1);
}


/*****************************************************************************
 *                           *** exported methods ***                        *
 *****************************************************************************/

/********************
 * dres/resolve
 ********************/
OHM_EXPORTABLE(int, update_goal, (char *goal, char **locals))
{
    
    /* XXX local variable assignments... */
    return dres_update_goal(dres, goal, locals);
}


/*****************************************************************************
 *                          *** DRES action handlers ***                     *
 *****************************************************************************/

/********************
 * prolog_handler
 ********************/
static int
prolog_handler(dres_t *dres, char *actname, dres_action_t *action, void **ret)
{
#define FAIL(ec) do { err = ec; goto fail; } while (0)
#define MAX_FACTS 63
    prolog_predicate_t *predicate;
    char                name[64];
    char             ***retval;
    OhmFact           **facts = NULL;
    int                 nfact, err;
    
    if (action->nargument < 1)
        return EINVAL;

    name[0] = '\0';
    dres_name(dres, action->arguments[0], name, sizeof(name));
    
    if ((predicate = prolog_lookup(name, action->nargument)) == NULL)
        FAIL(ENOENT);
    
    if (!prolog_invoke(predicate, &retval))
        FAIL(EINVAL);
    
    DEBUG("rule engine gave the following results:");
    prolog_dump(retval);

    if ((facts = ALLOC_ARR(OhmFact *, MAX_FACTS + 1)) == NULL)
        FAIL(ENOMEM);

    if ((nfact = retval_to_facts(retval, facts, MAX_FACTS)) < 0)
        FAIL(EINVAL);
    
    facts[nfact] = NULL;
    
    if (ret == NULL)
        FAIL(0);                     /* kludge: free facts and return 0 */
    
    *ret = facts;
    return 0;
    
 fail:
    if (facts) {
        int i;
        for (i = 0; i < nfact; i++)
            g_object_unref(facts[i]);
        FREE(facts);
    }
    
    return err;

#undef MAX_FACTS
}


/********************
 * signal_handler
 ********************/
static int
signal_handler(dres_t *dres, char *name, dres_action_t *action, void **ret)
{
#define MAX_FACTS 128
#define MAX_LENGTH 64
    static int        transid = 1;

    dres_variable_t  *var;
    char             *signature;
    unsigned long     timeout;
    int               factc;
    char              signal_name[MAX_LENGTH];
    char             *cb_name;
    char              prefix[MAX_LENGTH];
    char              arg[MAX_LENGTH];
    char             *factv[MAX_FACTS + 1];
    char              buf[MAX_FACTS * MAX_LENGTH];
    char              namebuf[MAX_LENGTH];
    char             *p;
    int               i, j;
    int               offs;
    int               success;
    
    *ret = NULL;

    factc = action->nargument - 2;

    if (factc < 1 || factc > MAX_FACTS)
        return EINVAL;
    
    signal_name[0] = '\0';
    dres_name(dres, action->arguments[0], signal_name, MAX_LENGTH);

    namebuf[0] = '\0';
    dres_name(dres, action->arguments[1], namebuf, MAX_LENGTH);

    prefix[MAX_LENGTH-1] = '\0';
    strncpy(prefix, dres_get_prefix(dres), MAX_LENGTH-1);

    if ((j = strlen(prefix)) > 0 && j < MAX_LENGTH-2 && prefix[j-1] != '.')
        strcpy(prefix+j, ".");

    switch (namebuf[0]) {

    case '$':
        cb_name = "";
        break;

    case '&':
        cb_name = "";
        if (!(var = dres_lookup_variable(dres, action->arguments[1]))) {
            dres_var_get_field(var->var, "value",NULL, VAR_STRING, &cb_name);
        }
        break;

    default:
        cb_name = namebuf;
        break;
    }
    DEBUG("%s(): cb_name='%s'\n", __FUNCTION__, cb_name);

    
    timeout = 5 * 1000;

    for (p = buf, i = 0;   i < factc;   i++) {

        dres_name(dres, action->arguments[i+2], arg, MAX_LENGTH);
            
        switch (arg[0]) {

        case '$':
            offs = 1;
            goto copy_string_arg;

        case '&':
            factv[i] = "";

            if (!(var = dres_lookup_variable(dres, action->arguments[i+2]))) {
                dres_var_get_field(var->var, "value", NULL,
                                   VAR_STRING, &factv[i]);
            }
            break;

        default:
            offs = 0;
            /* intentional fall trough */

        copy_string_arg:
            factv[i] = p;
            p += snprintf(p, MAX_LENGTH, "%s%s",
                          strchr(arg+offs, '.') ? "" : prefix, arg+offs) + 1;
            break;
        }
    }
    factv[factc] = NULL;

    if (cb_name[0] == '\0') {
        dump_signal_changed_args(signal_name, 0, factc,factv, NULL, timeout);
        success = signal_changed(signal_name, 0, factc,factv, NULL, timeout);
    }
    else {
        signature = (char *)completion_cb_SIGNATURE;

        if (ohm_module_find_method(cb_name,&signature,(void *)&completion_cb)){
            dump_signal_changed_args(signal_name, transid, factc,factv,
                                     completion_cb, timeout);
            success = signal_changed(signal_name, transid++, factc,factv,
                                     completion_cb, timeout);
        }
        else {
            DEBUG("Could not resolve signal.\n");
            success = FALSE;
        }
    }

    return success ? 0 : EINVAL;

#undef MAX_LENGTH
#undef MAX_FACTS
}

static void dump_signal_changed_args(char *signame, int transid, int factc,
                                     char**factv, completion_cb_t callback,
                                     unsigned long timeout)
{
    int i;

    DEBUG("calling signal_changed(%s, %d,  %d, %p, %p, %lu)",
          signame, transid, factc, factv, callback, timeout);

    for (i = 0;  i < factc;  i++) {
        DEBUG("   fact[%d]: '%s'", i, factv[i]);
    }
}


/*****************************************************************************
 *                        *** misc. helper routines ***                      *
 *****************************************************************************/


/********************
 * object_to_fact
 ********************/
static OhmFact *
object_to_fact(char *name, char **object)
{
    OhmFact *fact;
    GValue   value;
    char    *field;
    int      i;

    if (object == NULL || strcmp(object[0], "name") || object[1] == NULL)
        return NULL;
    
    if ((fact = ohm_fact_new(name)) == NULL)
        return NULL;
    
    for (i = 2; object[i] != NULL; i += 2) {
        field = object[i];
        value = ohm_value_from_string(object[i+1]);
        ohm_fact_set(fact, field, &value);
    }
    
    return fact;
}


/********************
 * retval_to_facts
 ********************/
static int
retval_to_facts(char ***objects, OhmFact **facts, int max)
{
    char **object;
    int    i;
    
    for (i = 0; (object = objects[i]) != NULL && i < max; i++) {
        if ((facts[i] = object_to_fact("foo", object)) == NULL)
            return -EINVAL;
    }
    
    return i;
}



/*
 * copy-paste code from variables.c in dres...
 */
typedef struct {
    char              *name;
    char              *value;
} dres_fldsel_t;

typedef struct {
    int                count;
    dres_fldsel_t     *field;
} dres_selector_t;


static dres_selector_t *parse_selector(char *descr)
{
    dres_selector_t *selector;
    dres_fldsel_t   *field;
    char            *p, *q, c;
    char            *str;
    char            *name;
    char            *value;
    char             buf[1024];
    int              i;

    
    if (descr == NULL) {
        errno = 0;
        return NULL;
    }

    for (p = descr, q = buf;  (c = *p) != '\0';   p++) {
        if (c > 0x20 && c < 0x7f)
            *q++ = c;
    }
    *q = '\0';

    if ((selector = malloc(sizeof(*selector))) == NULL)
        return NULL;
    memset(selector, 0, sizeof(*selector));

    for (i = 0, str = buf;   (name = strtok(str, ",")) != NULL;   str = NULL) {
        if ((p = strchr(name, ':')) == NULL)
            DEBUG("Invalid selctor: '%s'", descr);
        else {
            *p++ = '\0';
            value = p;

            selector->count++;
            selector->field = realloc(selector->field,
                                      sizeof(dres_fldsel_t) * selector->count);

            if (selector->field == NULL)
                return NULL; /* maybe better not to attempt to free anything */
            
            field = selector->field + selector->count - 1;

            field->name  = strdup(name);
            field->value = strdup(value);
        }
    }
   
    return selector;
}

static void free_selector(dres_selector_t *selector)
{
    int i;

    if (selector != NULL) {
        for (i = 0; i < selector->count; i++) {
            free(selector->field[i].name);
            free(selector->field[i].value);
        }

        free(selector);
    }
}

static int is_matching(OhmFact *fact, dres_selector_t *selector)
{
    dres_fldsel_t *fldsel;
    GValue        *gval;
    long int       ival;
    char          *e;
    int            i;
    int            match;
  
    if (fact == NULL || selector == NULL)
        match = FALSE;
    else {
        match = TRUE;

        for (i = 0;    match && i < selector->count;    i++) {
            fldsel = selector->field + i;

            if ((gval = ohm_fact_get(fact, fldsel->name)) == NULL)
                match = FALSE;
            else {
                switch (G_VALUE_TYPE(gval)) {
                    
                case G_TYPE_STRING:
                    match = !strcmp(g_value_get_string(gval), fldsel->value);
                    break;
                    
                case G_TYPE_INT:
                    ival  = strtol(fldsel->value, &e, 10);
                    match = (*e == '\0' && g_value_get_int(gval) == ival);
                    break;

                default:
                    match = FALSE;
                    break;
                }
            }
        } /* for */
    }

    return match;
}

static int find_facts(char *name, char *select, OhmFact **facts, int max)
{
    dres_selector_t *selector = parse_selector(select);
    
    GSList            *list;
    int                llen;
    OhmFact           *fact;
    int                flen;
    int                i;

    list   = ohm_fact_store_get_facts_by_name(ohm_fact_store_get_fact_store(),
                                              name);
    llen   = list ? g_slist_length(list) : 0;

    for (i = flen = 0;    list != NULL;   i++, list = g_slist_next(list)) {
        fact = (OhmFact *)list->data;

        if (!selector || is_matching(fact, selector))
            facts[flen++] = fact;

        if (flen >= max) {
            free_selector(selector);
            errno = ENOMEM;
            return -1;
        }
        
    }

    free_selector(selector);
    return flen;
}


/********************
 * set_fact
 ********************/
static void
set_fact(int cid, char *buf)
{
    GValue  gval;
    char    selector[128], fullname[128];
    char   *str, *name, *member, *selfld, *selval, *value, *p, *q;
    int     len;
    int      n = 128;
    OhmFact *facts[n];
    
    selector[0] = '\0';
    /*
     * here we parse command lines like:
     *    com.nokia.policy.audio_route[device:ihf].status = 0, ...
     * where
     *    'com.nokia.policy.audio_route' is a fact that has two fields:
     *    'device' and 'status'
     */
    
    for (str = buf; (name = strtok(str, ",")) != NULL; str = NULL) {
        if ((p = strchr(name, '=')) != NULL) {
            *p++ = 0;
            value = p;
       
            if ((p = strchr(name, '[')) != NULL &&
                (q = strchr(name, '.')) != NULL && p < q) {
                sprintf(fullname, "%s%s", dres_get_prefix(dres), name);
                name = fullname;
            }

            if ((p = strrchr(name, '.')) != NULL) {
                *p++ = 0;
                member = p;
                    
                if (p[-2] == ']' && (q = strchr(name, '[')) != NULL) {
                        
                    len = p - 2 - q - 1;
                    strncpy(selector, q + 1, len);
                    selector[len] = '\0';
                        
                    *q = p[-2] = 0;
                    selfld = q + 1;
                    if ((p = strchr(selfld, ':')) == NULL) {
                        console_printf(cid, "Invalid input: %s\n", selfld);
                        continue;
                    }
                    else {
                        *p++ = 0;
                        selval = p;
                    }
                }
                    
                gval = ohm_value_from_string(value);
                    
                if ((n = find_facts(name, selector, facts, n)) < 0)
                    console_printf(cid, "no fact matches %s[%s]\n",
                                   name, selector ?: "");
                else {
                    int i;
                    for (i = 0; i < n; i++) {
                        ohm_fact_set(facts[i], member, &gval);
                        console_printf(cid, "%s:%s = %s\n", name,
                                       member, value);
                    }
                }
            }
        }
    }
}


/********************
 * dres_goal
 ********************/
static void
dres_goal(int cid, char *cmd)
{
#define MAX_ARGS  32
    char  buf[2048];
    char *goal;
    char *varname;
    char *value;
    char *args[MAX_ARGS * 2 + 1];
    int   i;
    char  dbgstr[MAX_ARGS * 32];
    char *p, *e, *sep;

    if (!cmd || !*cmd) 
        goto syntax_error;

    strncpy(buf, cmd, sizeof(buf));
    buf[sizeof(buf)-1] = '\0';

    goal = value = buf;
    i = 0;

    if ((varname = strchr(value, ' ')) != NULL) {
        *varname++ = '\0';

        while ((value = strchr(varname, '=')) != NULL) {
            *value++ = '\0';

            args[i++] = varname;
            args[i++] = value;
            
            if (i >= MAX_ARGS * 2 || (varname = strchr(value, ' ')) == NULL)
                break;
            *varname++ = '\0';
       }
    }

    args[i] = NULL;

    p  = dbgstr;
    e  = p + sizeof(dbgstr);
    p += snprintf(p, e-p, "building goal '%s' with arguments '", goal);
    for (i = 0, sep="";  args[i] != NULL;  i += 2, sep = " ") {
        p += snprintf(p, e-p, "%s%s %s", sep, args[i], args[i+1]);
    }
    snprintf(p, e-p, "%s<null>'", sep);
    DEBUG("%s", dbgstr);

    dres_update_goal(dres, goal, args);

    return;

 syntax_error:
    console_printf(cid, "invalid dres arguments \"%s\"\n", cmd);
}



/********************
 * console_opened
 ********************/
static void
console_opened(int id, struct sockaddr *peer, int peerlen)
{
    printf("***** console 0x%x opened.\n", id);

    console_printf(id, "Welcome to the OHMng Dependency Resolver Console.\n");
    console_printf(id, "Type help to get a list of available commands.\n\n");
    console_printf(id, CONSOLE_PROMPT);
}

/********************
 * console_closed
 ********************/
static void
console_closed(int id)
{
    printf("***** console 0x%x closed.\n", id);
}


/********************
 * command_bye
 ********************/
static void
command_bye(int id, char *input)
{
    console_close(id);
}


/********************
 * command_dump
 ********************/
static void
command_dump(int id, char *input)
{
    OhmFactStore *fs = ohm_fact_store_get_fact_store();;
    OhmFact      *fact;
    GSList       *list;

    char factname[128], fullname[128], *name, *p, *q, *dump;
    
    p = input;
    if (!*p)
        p = "all";
    while (*p) {
        while (*p == ' ' || *p == ',')
            p++;
        for (q = factname; isalnum(*p) || *p == '_' || *p == '.'; *q++ = *p++)
            ;
        *q = '\0';
        if (!strcmp(factname, "all")) {
            dump = ohm_fact_store_to_string(fs);
            console_printf(id, "fact store: %s\n", dump);
            g_free(dump);
        }
        else {
            if (!strchr(factname, '.')) {
                sprintf(fullname, "%s%s", dres_get_prefix(dres), factname);
                name = fullname;
            }
            else
                name = factname;
            for (list = ohm_fact_store_get_facts_by_name(fs, name);
                 list != NULL;
                 list = g_slist_next(list)) {
                fact = (OhmFact *)list->data;
                dump = ohm_structure_to_string(OHM_STRUCTURE(fact));
                console_printf(id, "%s\n", dump ?: "");
                g_free(dump);
            }
        }
    }
}


/********************
 * command_set
 ********************/
static void
command_set(int id, char *input)
{
    set_fact(id, input);    
}


/********************
 * command_resolve
 ********************/
static void
command_resolve(int id, char *input)
{
    dres_goal(id, input);
}


/********************
 * command_prolog
 ********************/
static void
command_prolog(int id, char *input)
{
    prolog_shell();
}



#define COMMAND(c, a, d) {                                              \
    command:      #c,                                                   \
    args:         a,                                                    \
    description:  d,                                                    \
    handler:      command_##c,                                          \
}

#define LAST() { command: NULL }


typedef struct {
    char  *command;
    char  *args;
    char  *description;
    void (*handler)(int, char *);
} command_t;


static void command_help(int id, char *input);


static command_t commands[] = {
    COMMAND(help   , NULL       , "Get help on the available commands."     ),
    COMMAND(dump   , "[var]"    , "Dump a given or all factstore variables."),
    COMMAND(set    , "var value", "Set/change a given fact store variable." ),
    COMMAND(resolve, "[goal]"   , "Run the dependency resolver for a goal." ),
    COMMAND(prolog , NULL       , "Start an interactive prolog shell."      ),
    COMMAND(bye    , NULL       , "Close the resolver terminal session."    ),
    LAST()
};



/********************
 * command_help
 ********************/
static void
command_help(int id, char *input)
{
    command_t *c;
    char       syntax[128];

    console_printf(id, "Available commands:\n");
    for (c = commands; c->command != NULL; c++) {
        sprintf(syntax, "%s%s%s", c->command, c->args ? " ":"", c->args ?: ""); 
        console_printf(id, "    %-30.30s %s\n", syntax, c->description);
    }
}


/********************
 * find_command
 ********************/
command_t *
find_command(char *name)
{
    command_t *c;

    for (c = commands; c->command != NULL; c++)
        if (!strcmp(c->command, name))
            return c;
    
    return NULL;
}

 
/********************
 * console_input
 ********************/
static void
console_input(int id, char *input, void *data)
{
    command_t *command;
    char       cmd[64], *p, *q;
    int        len;

    q   = cmd;
    len = 0;
    for (p = input; *p && *p != ' ' && len < sizeof(cmd) - 1; *q++ = *p++)
        ;
    *q = '\0';
    while (*p == ' ' || *p == '\t')
        p++;

    if ((command = find_command(cmd)) != NULL)
        command->handler(id, p);
    else
        console_printf(id, "unknown command \"%s\"\n", input);

    console_printf(id, CONSOLE_PROMPT);
}



OHM_PLUGIN_DESCRIPTION("dres",
                       "0.0.0",
                       "krisztian.litkey@nokia.com",
                       OHM_LICENSE_NON_FREE,
                       plugin_init,
                       plugin_exit,
                       NULL);

OHM_PLUGIN_REQUIRES("prolog", "console");

OHM_PLUGIN_PROVIDES_METHODS(dres, 1,
    OHM_EXPORT(update_goal, "resolve")
);

OHM_PLUGIN_REQUIRES_METHODS(dres, 13,
    OHM_IMPORT("prolog.setup"         , prolog_setup),
    OHM_IMPORT("prolog.lookup"        , prolog_lookup),
    OHM_IMPORT("prolog.call"          , prolog_invoke),
    OHM_IMPORT("prolog.vcall"         , prolog_vinvoke),
    OHM_IMPORT("prolog.acall"         , prolog_ainvoke),
    OHM_IMPORT("prolog.free_retval"   , prolog_free),
    OHM_IMPORT("prolog.dump_retval"   , prolog_dump),
    OHM_IMPORT("prolog.shell"         , prolog_shell),
    OHM_IMPORT("console.open"         , console_open),
    OHM_IMPORT("console.close"        , console_close),
    OHM_IMPORT("console.write"        , console_write),
    OHM_IMPORT("console.printf"       , console_printf),
    OHM_IMPORT("signaling.signal_changed", signal_changed)
);
    
#if 0
int signal_changed(char *signame, int transid, int factc, char**factv,
                   completion_cb_t callback, unsigned long timeout)
{
    return TRUE;
}
#endif
                            

/* 
 * Local Variables:
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 * vim:set expandtab shiftwidth=4:
 */
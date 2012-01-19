/*
 * vim:ts=4:sw=4:expandtab
 *
 * i3 - an improved dynamic tiling window manager
 * © 2009-2012 Michael Stapelberg and contributors (see also: LICENSE)
 *
 * commands_parser.c: hand-written parser to parse commands (commands are what
 * you bind on keys and what you can send to i3 using the IPC interface, like
 * 'move left' or 'workspace 4').
 *
 * We use a hand-written parser instead of lex/yacc because our commands are
 * easy for humans, not for computers. Thus, it’s quite hard to specify a
 * context-free grammar for the commands. A PEG grammar would be easier, but
 * there’s downsides to every PEG parser generator I have come accross so far.
 *
 * This parser is basically a state machine which looks for literals or strings
 * and can push either on a stack. After identifying a literal or string, it
 * will either transition to the current state, to a different state, or call a
 * function (like cmd_move()).
 *
 * Special care has been taken that error messages are useful and the code is
 * well testable (when compiled with -DTEST_PARSER it will output to stdout
 * instead of actually calling any function).
 *
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <stdint.h>

#include "all.h"
#include "queue.h"

/*******************************************************************************
 * The data structures used for parsing. Essentially the current state and a
 * list of tokens for that state.
 *
 * The GENERATED_* files are generated by generate-commands-parser.pl with the
 * input parser-specs/commands.spec.
 ******************************************************************************/

#include "GENERATED_enums.h"

typedef struct token {
    char *name;
    char *identifier;
    /* This might be __CALL */
    cmdp_state next_state;
    union {
        uint16_t call_identifier;
    } extra;
} cmdp_token;

typedef struct tokenptr {
    cmdp_token *array;
    int n;
} cmdp_token_ptr;

#include "GENERATED_tokens.h"

/*******************************************************************************
 * The (small) stack where identified literals are stored during the parsing
 * of a single command (like $workspace).
 ******************************************************************************/

struct stack_entry {
    /* Just a pointer, not dynamically allocated. */
    const char *identifier;
    char *str;
};

/* 10 entries should be enough for everybody. */
static struct stack_entry stack[10];

/*
 * Pushes a string (identified by 'identifier') on the stack. We simply use a
 * single array, since the number of entries we have to store is very small.
 *
 */
static void push_string(const char *identifier, char *str) {
    for (int c = 0; c < 10; c++) {
        if (stack[c].identifier != NULL)
            continue;
        /* Found a free slot, let’s store it here. */
        stack[c].identifier = identifier;
        stack[c].str = str;
        return;
    }

    /* When we arrive here, the stack is full. This should not happen and
     * means there’s either a bug in this parser or the specification
     * contains a command with more than 10 identified tokens. */
    printf("argh! stack full\n");
    exit(1);
}

// XXX: ideally, this would be const char. need to check if that works with all
// called functions.
static char *get_string(const char *identifier) {
    DLOG("Getting string %s from stack...\n", identifier);
    for (int c = 0; c < 10; c++) {
        if (stack[c].identifier == NULL)
            break;
        if (strcmp(identifier, stack[c].identifier) == 0)
            return stack[c].str;
    }
    return NULL;
}

static void clear_stack() {
    DLOG("clearing stack.\n");
    for (int c = 0; c < 10; c++) {
        if (stack[c].str != NULL)
            free(stack[c].str);
        stack[c].identifier = NULL;
        stack[c].str = NULL;
    }
}

// TODO: remove this if it turns out we don’t need it for testing.
#if 0
/*******************************************************************************
 * A dynamically growing linked list which holds the criteria for the current
 * command.
 ******************************************************************************/

typedef struct criterion {
    char *type;
    char *value;

    TAILQ_ENTRY(criterion) criteria;
} criterion;

static TAILQ_HEAD(criteria_head, criterion) criteria =
  TAILQ_HEAD_INITIALIZER(criteria);

/*
 * Stores the given type/value in the list of criteria.
 * Accepts a pointer as first argument, since it is 'call'ed by the parser.
 *
 */
static void push_criterion(void *unused_criteria, const char *type,
                           const char *value) {
    struct criterion *criterion = malloc(sizeof(struct criterion));
    criterion->type = strdup(type);
    criterion->value = strdup(value);
    TAILQ_INSERT_TAIL(&criteria, criterion, criteria);
}

/*
 * Clears the criteria linked list.
 * Accepts a pointer as first argument, since it is 'call'ed by the parser.
 *
 */
static void clear_criteria(void *unused_criteria) {
    struct criterion *criterion;
    while (!TAILQ_EMPTY(&criteria)) {
        criterion = TAILQ_FIRST(&criteria);
        free(criterion->type);
        free(criterion->value);
        TAILQ_REMOVE(&criteria, criterion, criteria);
        free(criterion);
    }
}
#endif

/*******************************************************************************
 * The parser itself.
 ******************************************************************************/

static cmdp_state state;
#ifndef TEST_PARSER
static Match current_match;
#endif
static char *json_output;

#include "GENERATED_call.h"


static void next_state(const cmdp_token *token) {
    if (token->next_state == __CALL) {
        DLOG("should call stuff, yay. call_id = %d\n",
                token->extra.call_identifier);
        json_output = GENERATED_call(token->extra.call_identifier);
        clear_stack();
        return;
    }

    state = token->next_state;
    if (state == INITIAL) {
        clear_stack();
    }
}

/* TODO: Return parsing errors via JSON. */
char *parse_command(const char *input) {
    DLOG("new parser handling: %s\n", input);
    state = INITIAL;
    json_output = NULL;

    const char *walk = input;
    const size_t len = strlen(input);
    int c;
    const cmdp_token *token;
    bool token_handled;

    // TODO: make this testable
#ifndef TEST_PARSER
    cmd_criteria_init(&current_match);
#endif

    /* The "<=" operator is intentional: We also handle the terminating 0-byte
     * explicitly by looking for an 'end' token. */
    while ((walk - input) <= len) {
        /* skip whitespace before every token */
        while ((*walk == ' ' || *walk == '\t') && *walk != '\0')
            walk++;

        DLOG("remaining input = %s\n", walk);

        cmdp_token_ptr *ptr = &(tokens[state]);
        token_handled = false;
        for (c = 0; c < ptr->n; c++) {
            token = &(ptr->array[c]);
            DLOG("trying token %d = %s\n", c, token->name);

            /* A literal. */
            if (token->name[0] == '\'') {
                DLOG("literal\n");
                if (strncasecmp(walk, token->name + 1, strlen(token->name) - 1) == 0) {
                    DLOG("found literal, moving to next state\n");
                    if (token->identifier != NULL)
                        push_string(token->identifier, strdup(token->name + 1));
                    walk += strlen(token->name) - 1;
                    next_state(token);
                    token_handled = true;
                    break;
                }
                continue;
            }

            if (strcmp(token->name, "string") == 0 ||
                strcmp(token->name, "word") == 0) {
                DLOG("parsing this as a string\n");
                const char *beginning = walk;
                /* Handle quoted strings (or words). */
                if (*walk == '"') {
                    beginning++;
                    walk++;
                    while (*walk != '"' || *(walk-1) == '\\')
                        walk++;
                } else {
                    if (token->name[0] == 's') {
                        /* For a string (starting with 's'), the delimiters are
                         * comma (,) and semicolon (;) which introduce a new
                         * operation or command, respectively. */
                        while (*walk != ';' && *walk != ',' && *walk != '\0')
                            walk++;
                    } else {
                        /* For a word, the delimiters are white space (' ' or
                         * '\t'), closing square bracket (]), comma (,) and
                         * semicolon (;). */
                        while (*walk != ' ' && *walk != '\t' && *walk != ']' &&
                               *walk != ',' && *walk !=  ';' && *walk != '\0')
                            walk++;
                    }
                }
                if (walk != beginning) {
                    char *str = calloc(walk-beginning + 1, 1);
                    strncpy(str, beginning, walk-beginning);
                    if (token->identifier)
                        push_string(token->identifier, str);
                    DLOG("str is \"%s\"\n", str);
                    /* If we are at the end of a quoted string, skip the ending
                     * double quote. */
                    if (*walk == '"')
                        walk++;
                    next_state(token);
                    token_handled = true;
                    break;
                }
            }

            if (strcmp(token->name, "end") == 0) {
                DLOG("checking for the end token.\n");
                if (*walk == '\0' || *walk == ',' || *walk == ';') {
                    DLOG("yes, indeed. end\n");
                    next_state(token);
                    token_handled = true;
                    /* To make sure we start with an appropriate matching
                     * datastructure for commands which do *not* specify any
                     * criteria, we re-initialize the criteria system after
                     * every command. */
                    // TODO: make this testable
#ifndef TEST_PARSER
                    if (*walk == '\0' || *walk == ';')
                        cmd_criteria_init(&current_match);
#endif
                    walk++;
                    break;
               }
           }
        }

        if (!token_handled) {
            /* Figure out how much memory we will need to fill in the names of
             * all tokens afterwards. */
            int tokenlen = 0;
            for (c = 0; c < ptr->n; c++)
                tokenlen += strlen(ptr->array[c].name) + strlen("'', ");

            /* Build up a decent error message. We include the problem, the
             * full input, and underline the position where the parser
             * currently is. */
            char *errormessage;
            char *possible_tokens = malloc(tokenlen + 1);
            char *tokenwalk = possible_tokens;
            for (c = 0; c < ptr->n; c++) {
                token = &(ptr->array[c]);
                if (token->name[0] == '\'') {
                    /* A literal is copied to the error message enclosed with
                     * single quotes. */
                    *tokenwalk++ = '\'';
                    strcpy(tokenwalk, token->name + 1);
                    tokenwalk += strlen(token->name + 1);
                    *tokenwalk++ = '\'';
                } else {
                    /* Any other token is copied to the error message enclosed
                     * with angle brackets. */
                    *tokenwalk++ = '<';
                    strcpy(tokenwalk, token->name);
                    tokenwalk += strlen(token->name);
                    *tokenwalk++ = '>';
                }
                if (c < (ptr->n - 1)) {
                    *tokenwalk++ = ',';
                    *tokenwalk++ = ' ';
                }
            }
            *tokenwalk = '\0';
            asprintf(&errormessage, "Expected one of these tokens: %s",
                     possible_tokens);
            free(possible_tokens);

            /* Contains the same amount of characters as 'input' has, but with
             * the unparseable part highlighted using ^ characters. */
            char *position = malloc(len + 1);
            for (const char *copywalk = input; *copywalk != '\0'; copywalk++)
                position[(copywalk - input)] = (copywalk >= walk ? '^' : ' ');
            position[len] = '\0';

            printf("%s\n", errormessage);
            printf("Your command: %s\n", input);
            printf("              %s\n", position);

            free(position);
            free(errormessage);
            break;
        }
    }

    DLOG("json_output = %s\n", json_output);
    return json_output;
}

/*******************************************************************************
 * Code for building the stand-alone binary test.commands_parser which is used
 * by t/187-commands-parser.t.
 ******************************************************************************/

#ifdef TEST_PARSER

/*
 * Logs the given message to stdout while prefixing the current time to it,
 * but only if the corresponding debug loglevel was activated.
 * This is to be called by DLOG() which includes filename/linenumber
 *
 */
void debuglog(uint64_t lev, char *fmt, ...) {
    va_list args;

    va_start(args, fmt);
    fprintf(stdout, "# ");
    vfprintf(stdout, fmt, args);
    va_end(args);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Syntax: %s <command>\n", argv[0]);
        return 1;
    }
    parse_command(argv[1]);
}
#endif

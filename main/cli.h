#pragma once

#include <stdbool.h>
#include <stdint.h>

// prompt
#ifndef CLI_PROMPT
#define CLI_PROMPT ">> "
#endif

// how many chars in the text edit buffer?
#ifndef CLI_BUFFER_SIZE
#define CLI_BUFFER_SIZE 128
#endif

// how many chars do we reserve for history?
#ifndef CLI_HISTORY_BUFFER_SIZE
#define CLI_HISTORY_BUFFER_SIZE 256
#endif

// pretty ansi colors
#ifndef CLI_USE_COLORS
#define CLI_USE_COLORS 1
#endif

// how much space to use for DMA in the UART
#ifndef CLI_RX_BUFFER_SIZE
#define CLI_RX_BUFFER_SIZE 256
#endif

// number of "events" to allow to be enqueued (doesn't matter)
#ifndef CLI_RX_QUEUE_SIZE
#define CLI_RX_QUEUE_SIZE 8
#endif

// stack used by the CLI task
#ifndef CLI_TASK_STACK_SIZE
#define CLI_TASK_STACK_SIZE 4096
#endif

// maximum slots for calls to `cli_register_commands` (each call uses one slot)
#ifndef CLI_MAX_COMMAND_SETS
#define CLI_MAX_COMMAND_SETS 16
#endif

// when displaying help, how many commands can we collect/sort in place?
// (it will allocate this many pointers on the stack, so 64 = 256 bytes on the stack)
#define CLI_MAX_COLLECT 64
// where should the help text be displayed, horizontally?
#define CLI_HELP_LEFT_PAD 40
// when parsing, args after this are just packed together into the final arg.
#define CLI_MAX_ARGS 10

/*
 * a command's callback is called with the `callback_arg` from its command
 * table entry, and an argc/argv that contain any arguments. argv[0] will
 * always be the final command entry that activated it, so for example in
 * "sys reboot", argv[0] will be "reboot".
 */
typedef void (*cli_callback_t)(const void *callback_arg, int argc, const char * const *argv);

/*
 * a single CLI command record. these should be in an array, terminated by
 * `CLI_LAST_COMMAND`, as static data.
 */
typedef struct cli_command {
    // the command word, like "sys", optionally followed by a space and a description of parameters
    const char *name;

    // an optional text to display in ?/help (may be NULL)
    const char *help;

    // function to call, and any state to pass in -- use this or `subcommands` but not both
    const cli_callback_t callback;
    const void *callback_arg;

    // nested array of commands
    const struct cli_command *subcommands;
} cli_command_t;

#define CLI_LAST_COMMAND { NULL, NULL, NULL, NULL, NULL }

/*
 * setup CLI and pass in an (optional) list of supported commands.
 * commands can be added later with `cli_register_commands`.
 */
void cli_init(uart_port_t uart, const cli_command_t *commands);

/*
 * append a set of commands to the list, if there's room.
 * modules can use this to add their own commands to the CLI as they init.
 */
void cli_register_commands(const cli_command_t *commands);

/*
 * return true if `s` is "1" or "true" or "on" or "yes".
 * just as a handy helper for parsing simple commands.
 */
bool cli_is_truthy(const char *s);

/*
 * display the help screen as if someone typed "?".
 */
void cli_display_help(void);

#include <string.h>
#include "driver/uart.h"

#include "cli.h"

#if CLI_USE_COLORS
#define ANSI_COLOR_RED "\x1b[31m"
#define ANSI_COLOR_GREEN "\x1b[32m"
#define ANSI_COLOR_OFF "\x1b[39m"
#define ANSI_BOLD "\x1b[1m"
#define ANSI_BOLD_OFF "\x1b[22m"
#define ANSI_UNDERLINE "\x1b[4m"
#define ANSI_UNDERLINE_OFF "\x1b[24m"
#else
#define ANSI_COLOR_RED ""
#define ANSI_COLOR_GREEN ""
#define ANSI_COLOR_OFF ""
#define ANSI_BOLD ""
#define ANSI_BOLD_OFF ""
#define ANSI_UNDERLINE ""
#define ANSI_UNDERLINE_OFF ""
#endif

#define ANSI_CLEAR_LINE "\x1b[128D\x1b[K"

#define outstr(_s) uart_write_bytes(s_cli_uart, _s, sizeof(_s))
#define display_prompt() outstr(ANSI_BOLD CLI_PROMPT ANSI_BOLD_OFF)
#define clear_line() outstr(ANSI_CLEAR_LINE)

static uart_port_t s_cli_uart;

const cli_command_t *s_command_sets[CLI_MAX_COMMAND_SETS] = { NULL, };
int s_command_sets_count = 0;

// current edit buffer
static uint8_t s_buffer[CLI_BUFFER_SIZE];
static uint8_t s_buffer_length = 0;
static uint8_t s_cursor = 0;

// history
static uint8_t s_history[CLI_HISTORY_BUFFER_SIZE];
static uint8_t s_history_index = 0;
static uint8_t s_history_length = 0;
static bool s_history_active = false;

// key (input) state
static uint8_t s_csi_state = 0;  // 1=\e, 2=\e[ (CSI)
static uint8_t s_csi_param = 0;  // digits following CSI


#define move_left(_n) csi_move(_n, 'D')
#define move_right(_n) csi_move(_n, 'C')

static void csi_move(int n, uint8_t command) {
    if (n == 0) return;
    char s[8];
    int len = snprintf(s, 8, "\x1b[%d%c", n, command);
    uart_write_bytes(s_cli_uart, s, len);
}


// ----- display help

static const char SPACES[33] = "                                ";
static void display_spaces(int count) {
    if (count <= 0) return;
    while (count > 32) {
        outstr(SPACES);
        count -= 32;
    }
    uart_write_bytes(s_cli_uart, SPACES, count);
}

// collect pointers to commands into a single array. returns how many slots were used.
static int collect_commands(const cli_command_t * const *command_sets, int command_sets_count, const cli_command_t **target, int slots) {
    int n = 0;
    for (int ci = 0; ci < command_sets_count; ci++) {
        const cli_command_t *commands = command_sets[ci];
        for (int c = 0; commands[c].name != NULL; c++) {
            target[n++] = &commands[c];
            if (n == slots) return n;
        }
    }
    return n;
}

// simple selection sort
static void sort_commands(const cli_command_t **commands, int command_count) {
    for (int i = 0; i < command_count; i++) {
        int target = i;
        for (int j = i + 1; j < command_count; j++) {
            if (strcmp(commands[j]->name, commands[target]->name) < 0) target = j;
        }
        if (target != i) {
            const cli_command_t *temp = commands[i];
            commands[i] = commands[target];
            commands[target] = temp;
        }
    }
}

static void display_help_span(const cli_command_t **commands, int command_count, int slots_remaining, int indent) {
    sort_commands(commands, command_count);

    for (int c = 0; c < command_count; c++) {
        display_spaces(indent);
        uart_write_bytes(s_cli_uart, commands[c]->name, strlen(commands[c]->name));
        int filled = indent + strlen(commands[c]->name);
        bool displayed_subcommands = false;

        // if all subcommands are help-free, just dump them in one line
        if (commands[c]->subcommands) {
            const cli_command_t *sub = commands[c]->subcommands;
            bool all_empty = true;
            for (int sc = 0; sub[sc].name != NULL; sc++) if (sub[sc].help) all_empty = false;
            if (all_empty) {
                outstr(" <");
                for (int sc = 0; sub[sc].name != NULL; sc++) {
                    if (sc != 0) outstr(" | ");
                    uart_write_bytes(s_cli_uart, sub[sc].name, strlen(sub[sc].name));
                    filled += (sc != 0 ? 3 : 0) + strlen(sub[sc].name);
                }
                outstr(">");
                filled += 3;
                displayed_subcommands = true;
            }
        }

        if (commands[c]->help != NULL) {
            if (filled >= CLI_HELP_LEFT_PAD) {
                outstr("\r\n");
                filled = 0;
            }
            display_spaces(CLI_HELP_LEFT_PAD - filled);
            uart_write_bytes(s_cli_uart, commands[c]->help, strlen(commands[c]->help));
        }
        outstr("\r\n");
        if (commands[c]->subcommands && !displayed_subcommands) {
            const cli_command_t **new_commands = &commands[command_count];
            int new_count = collect_commands(&commands[c]->subcommands, 1, new_commands, slots_remaining);
            display_help_span(new_commands, new_count, slots_remaining - new_count, indent + 4);
        }
    }
}

static void display_help(void) {
    const cli_command_t *commands[CLI_MAX_COLLECT];
    int command_count = collect_commands(s_command_sets, s_command_sets_count, commands, CLI_MAX_COLLECT);

    outstr(ANSI_UNDERLINE "Commands:" ANSI_UNDERLINE_OFF "\r\n");
    display_help_span(commands, command_count, CLI_MAX_COLLECT - command_count, 0);
}

static void display_error(int index) {
    outstr(ANSI_COLOR_RED);
    display_spaces(index + 4);
    outstr("^?" ANSI_COLOR_OFF "\r\n");
}


// ----- parser & executor

static int next_space(int index) {
    while (index < s_buffer_length && s_buffer[index] != ' ') index++;
    return index;
}

static int next_nonspace(int index) {
    while (index < s_buffer_length && s_buffer[index] == ' ') index++;
    return index;
}

/*
 * given a command set and an index into the command line, figure out if
 * there's a full or partial match with 1 or more of the commands.
 *
 * returns true if there's an exact match.
 * if there's an exact or partial match, `matched` will point to the first
 * or exact matching command entry, and `extent` is the number of chars past
 * `index` that matched.
 * otherwise, `matched` will be NULL.
 */
static bool match_command(
    const cli_command_t * const *command_sets,
    int command_sets_count,
    int index,
    const cli_command_t **matched,
    int *extent
) {
    *matched = NULL;
    *extent = 0;
    if (command_sets == NULL || command_sets_count == 0) return false;

    int end = next_space(index);
    for (int ci = 0; ci < command_sets_count; ci++) {
        const cli_command_t *commands = command_sets[ci];
        for (int c = 0; commands[c].name != NULL; c++) {
            uint16_t command_len = strlen(commands[c].name);

            if (strncmp((char *)(s_buffer + index), commands[c].name, end - index) != 0) continue;
            if (end - index == command_len || commands[c].name[end - index] == ' ') {
                // exact match
                *matched = &commands[c];
                *extent = end - index;
                return true;
            }

            if (*matched == NULL) {
                // first partial match: mark the entire command as the extent
                *matched = &commands[c];
                *extent = 0;
                while (commands[c].name[*extent] && commands[c].name[*extent] != ' ') (*extent)++;
            } else {
                // not the first partial match: reduce the extent to the common prefix
                int i = end - index;
                while (i < command_len && i < *extent && commands[c].name[i] == (*matched)->name[i]) i++;
                *extent = i;
            }
        }
    }

    if (*extent == 0) *matched = NULL;
    return false;
}

// modifies the string inline
static void parse(int index, int *argc, const char **argv) {
    *argc = 0;
    while (index < s_buffer_length && *argc <= CLI_MAX_ARGS) {
        argv[(*argc)++] = (char *)(s_buffer + index);
        index = next_space(index);
        s_buffer[index++] = 0;
        index = next_nonspace(index);
    }
}

// clean up the modified string
static void unparse(void) {
    for (int i = 0; i < s_buffer_length; i++) if (s_buffer[i] == 0) s_buffer[i] = ' ';
}

static void execute(void) {
    int index = next_nonspace(0);
    const cli_command_t * const *command_sets = s_command_sets;
    int command_sets_count = s_command_sets_count;
    const cli_command_t *match;
    int extent = 0;

    while (index < s_buffer_length && match_command(command_sets, command_sets_count, index, &match, &extent)) {
        if (match == NULL) {
            display_error(index);
            display_help();
            return;
        }

        if (match->subcommands == NULL) {
            const char *argv[CLI_MAX_ARGS] = { NULL, };
            int argc = 0;
            parse(index, &argc, argv);
            if (match->callback) {
                match->callback(match->callback_arg, argc, argv);
            } else {
                outstr(ANSI_COLOR_GREEN "*** ");
                for (int i = 0; i < argc; i++) {
                    uart_write_bytes(s_cli_uart, argv[i], strlen(argv[i]));
                    outstr(" ");
                }
                outstr(ANSI_COLOR_OFF "\r\n");
            }
            unparse();
            return;
        }

        index += extent;
        index = next_nonspace(index);
        command_sets = &match->subcommands;
        command_sets_count = 1;
    }

    display_error(index);
    display_help();
    return;
}


// ----- history

static int history_length(int index) {
    int end = index;
    while (s_history[end]) end++;
    return end - index;
}

static int history_next_from(int index) {
    while (index < s_history_length && s_history[index]) index++;
    if (index < s_history_length) index++;
    return index;
}

static int history_previous_from(int index) {
    if (index == 0) return index;
    index -= 2;
    while (index > 0 && s_history[index]) index--;
    if (index > 0) index++;
    return index;
}

static void history_drop(int index) {
    if (index >= s_history_length) return;
    int len = history_length(index);
    int end = index + len + 1;
    memmove(s_history + index, s_history + end, s_history_length - end);
    s_history_length -= len + 1;
    if (s_history_index >= index) s_history_index -= len + 1;
}

// save the current line buffer at the end of history, temporarily.
static void history_save_buffer(void) {
    // drop history to make room for the newbie:
    while (CLI_HISTORY_BUFFER_SIZE - s_history_length < s_buffer_length + 1) {
        // this can only happen if the current line length is bigger than the entire history buffer:
        if (s_history_length == 0) return;
        history_drop(0);
    }

    memmove(s_history + s_history_length, s_buffer, s_buffer_length);
    s_history[s_history_length + s_buffer_length] = 0;
}

// copy the saved line buffer back.
static void history_restore_buffer(void) {
    s_buffer_length = history_length(s_history_length);
    memmove(s_buffer, s_history + s_history_length, s_buffer_length);
}

static void history_add(void) {
    // remove any history item that's identical.
    int index = 0;
    while (index < s_history_length) {
        int len = history_length(index);
        if (len == s_cursor && memcmp(s_history + index, s_buffer, len) == 0) {
            history_drop(index);
        } else {
            index += len + 1;
        }
    }

    history_save_buffer();
    s_history_length += s_buffer_length + 1;
    s_history_index = s_history_length;
    s_history_active = false;
}

static void history_previous(void) {
    if (s_history_index == 0) return;
    if (!s_history_active) {
        history_save_buffer();
        s_history_active = true;
    }
    s_history_index = history_previous_from(s_history_index);
    s_buffer_length = history_length(s_history_index);
    memmove(s_buffer, s_history + s_history_index, s_buffer_length);
    s_cursor = s_buffer_length;
}

static void history_next(void) {
    if (!s_history_active) return;
    s_history_index = history_next_from(s_history_index);
    if (s_history_index == s_history_length) {
        history_restore_buffer();
        s_cursor = s_buffer_length;
        s_history_active = false;
        return;
    }
    s_buffer_length = history_length(s_history_index);
    memmove(s_buffer, s_history + s_history_index, s_buffer_length);
    s_cursor = s_buffer_length;
}

static void history_stop(void) {
    s_history_index = s_history_length;
}


// ----- key actions

static void insert(uint8_t c) {
    // leave a byte for zero-terminating it later:
    if (s_buffer_length < CLI_BUFFER_SIZE - 1) s_buffer_length++;
    for (int i = s_buffer_length - 1; i > s_cursor; i--) s_buffer[i] = s_buffer[i - 1];
    if (s_cursor < CLI_BUFFER_SIZE - 1) {
        s_buffer[s_cursor++] = c;
        uart_write_bytes(s_cli_uart, &c, 1);
    }
    if (s_cursor < s_buffer_length) uart_write_bytes(s_cli_uart, &s_buffer[s_cursor], s_buffer_length - s_cursor);
    move_left(s_buffer_length - s_cursor);
}

static void left(void) {
    if (s_cursor == 0) return;
    s_cursor--;
    csi_move(1, 'D');
}

static void right(void) {
    if (s_cursor == s_buffer_length) return;
    s_cursor++;
    csi_move(1, 'C');
}

static void home(void) {
    move_left(s_cursor);
    s_cursor = 0;
}

static void end(void) {
    move_right(s_buffer_length - s_cursor);
    s_cursor = s_buffer_length;
}

static void del(void) {
    if (s_buffer_length > s_cursor) s_buffer_length--;
    for (int i = s_cursor; i < s_buffer_length; i++) {
        s_buffer[i] = s_buffer[i + 1];
        uart_write_bytes(s_cli_uart, &s_buffer[i], 1);
    }
    outstr(" ");
    move_left(s_buffer_length - s_cursor + 1);
}

static void bs(void) {
    if (s_cursor == 0) return;
    s_cursor--;
    outstr("\b \b");
    if (s_cursor < s_buffer_length) {
        del();
    } else {
        s_buffer_length--;
    }
}

static void deleol(void) {
    for (int i = s_cursor; i < s_buffer_length; i++) outstr(" ");
    move_left(s_buffer_length - s_cursor);
    s_buffer_length = s_cursor;
}

static void delword(void) {
    while (s_cursor > 0 && s_buffer[s_cursor - 1] == ' ') bs();
    while (s_cursor > 0 && s_buffer[s_cursor - 1] != ' ') bs();
}

static void transpose(void) {
    if (s_cursor == 0 || s_cursor == s_buffer_length) return;
    uint8_t c = s_buffer[s_cursor - 1];
    s_buffer[s_cursor - 1] = s_buffer[s_cursor];
    s_buffer[s_cursor] = c;
    move_left(1);
    uart_write_bytes(s_cli_uart, &s_buffer[s_cursor - 1], 2);
    move_left(1);
}

static void reset(void) {
    clear_line();
    display_prompt();
    s_cursor = 0;
    s_buffer_length = 0;
    history_stop();
}

static void redraw(void) {
    clear_line();
    display_prompt();
    uart_write_bytes(s_cli_uart, s_buffer, s_buffer_length);
    move_left(s_buffer_length - s_cursor);
}

static void tab(void) {
    int index = next_nonspace(0);
    const cli_command_t * const *command_sets = s_command_sets;
    int command_sets_count = s_command_sets_count;

    while (index < s_cursor) {
        const cli_command_t *match;
        int extent = 0;
        if (match_command(command_sets, command_sets_count, index, &match, &extent)) {
            // exact match.
            index += extent;
            while (index < s_cursor && s_buffer[index] == ' ') index++;
            command_sets = &match->subcommands;
            command_sets_count = 1;
            if (index == s_cursor || command_sets == NULL) {
                if (s_buffer[index - 1] != ' ') insert(' ');
                return;
            }
            continue;
        }
        if (!match) return;

        int end = next_space(index);
        // if we found an autocomplete, but it's for some prior word, give up.
        if (end < s_cursor) return;
        // move the cursor along chars that are already matches.
        while (
            s_cursor < index + extent &&
            s_cursor < s_buffer_length &&
            s_buffer[s_cursor] == match->name[s_cursor - index]
        ) {
            s_cursor++;
            move_right(1);
        }
        while (s_cursor < index + extent) insert(match->name[s_cursor - index]);
        bool has_args = (match->name[s_cursor - index] == ' ');
        bool full_match = has_args || (match->name[s_cursor - index] == 0);
        if ((match->subcommands || has_args) && full_match && s_cursor == s_buffer_length) insert(' ');
        return;
    }
}

static void commit(void) {
    s_buffer[s_buffer_length] = 0;
    move_right(s_buffer_length - s_cursor);
    outstr("\r\n");
    if (
        (s_buffer_length >= 1 && s_buffer[0] == '?') ||
        (s_buffer_length == 4 && memcmp(s_buffer, "help", 4) == 0) ||
        (s_buffer_length == 4 && memcmp(s_buffer, "menu", 4) == 0)
    ) {
        display_help();
        history_add();
    } else if (s_buffer_length > 0) {
        execute();
        history_add();
    }

    reset();
}

static void process_key(uint8_t key) {
    switch (s_csi_state) {
        case 0:
            switch (key) {
                case 0x01:  // C-a
                    home();
                    break;
                case 0x02:  // C-b
                    left();
                    break;
                case 0x03:  // C-c
                    outstr(ANSI_BOLD "^C" ANSI_BOLD_OFF "\r\n");
                    reset();
                    break;
                case 0x04:  // C-d
                    del();
                    break;
                case 0x05:  // C-e
                    end();
                    break;
                case 0x06:  // C-f
                    right();
                    break;
                case 0x08:  // C-h (bs)
                    bs();
                    break;
                case 0x09:  // C-i (tab)
                    tab();
                    break;
                case 0x0b:  // C-k
                    deleol();
                    break;
                case 0x0c:  // C-l
                    redraw();
                    break;
                case 0x0d:  // C-m (enter)
                    commit();
                    break;
                case 0x12:  // C-r
                    redraw();
                    break;
                case 0x14:  // C-t
                    transpose();
                    break;
                case 0x15:  // C-u
                    reset();
                    break;
                case 0x17:  // C-w
                    delword();
                    break;
                case 0x1b:  // ESC
                    s_csi_state++;
                    break;
                case 0x7f:  // DEL
                    bs();
                    break;
                default:
                    if (key >= 0x20 && key <= 0x7e) {
                        insert(key);
                    }
            }
            break;
        case 1:
            switch (key) {
                case '[':
                    s_csi_state++;
                    s_csi_param = 0;
                    break;
                default:
                    // ignore ESC
                    s_csi_state = 0;
                    process_key(key);
            }
            break;
        case 2:
            switch (key) {
                case ';':
                    // too complex for us, ignore.
                    s_csi_param = 0;
                    break;
                case 'A':
                    history_previous();
                    redraw();
                    s_csi_state = 0;
                    break;
                case 'B':
                    history_next();
                    redraw();
                    s_csi_state = 0;
                    break;
                case 'C':
                    right();
                    s_csi_state = 0;
                    break;
                case 'D':
                    left();
                    s_csi_state = 0;
                    break;
                case 'F':
                    end();
                    s_csi_state = 0;
                    break;
                case 'H':
                    home();
                    s_csi_state = 0;
                    break;
                case '~':
                    switch (s_csi_param) {
                        case 1:
                            home();
                            break;
                        case 3:
                            del();
                            break;
                        case 4:
                            end();
                            break;
                    }
                    s_csi_state = 0;
                    break;
                default:
                    if (key >= '0' && key <= '9') {
                        s_csi_param = s_csi_param * 10 + (key - '0');
                    } else {
                        // ignore ESC
                        s_csi_state = 0;
                        process_key('[');
                        process_key(key);
                    }
            }
            break;
    }
}

static void cli_task(QueueHandle_t queue) {
    uart_event_t event;
    uint8_t ch;

    while (true) {
        if (xQueueReceive(queue, &event, pdMS_TO_TICKS(portMAX_DELAY))) {
            while (uart_read_bytes(s_cli_uart, &ch, 1, 0) > 0) process_key(ch);
        }
    }
}


// ----- API

void cli_init(uart_port_t uart, const cli_command_t *commands) {
    QueueHandle_t queue;
    ESP_ERROR_CHECK(uart_driver_install(uart, CLI_RX_BUFFER_SIZE, 0, CLI_RX_QUEUE_SIZE, &queue, 0));
    s_cli_uart = uart;

    TaskHandle_t task;
    xTaskCreate(cli_task, "CLI", CLI_TASK_STACK_SIZE / sizeof(portSTACK_TYPE), queue, tskIDLE_PRIORITY, &task);
    display_prompt();
    if (commands != NULL) cli_register_commands(commands);
}

void cli_register_commands(const cli_command_t *commands) {
    if (s_command_sets_count == CLI_MAX_COMMAND_SETS) {
        outstr("!!! Too many console command sets registered; increase CLI_MAX_COMMAND_SETS\r\n");
        return;
    }
    s_command_sets[s_command_sets_count++] = commands;
}

bool cli_is_truthy(const char *s) {
    if (s == NULL) return false;
    if (s[0] == '1') return true;
    if (strcmp(s, "on") == 0 || strcmp(s, "true") == 0 || strcmp(s, "yes") == 0) return true;
    return false;
}

void cli_display_help(void) {
    display_help();
}

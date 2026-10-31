#ifndef PT_CLI_H
#define PT_CLI_H
/* The window-free commands of cli.c, as AGENTS.md documents them; main.c and the window's argument handling (app.c)
 * call these. */
#include <stdio.h>
/* argv[1] is the command (or help, -h/--help, --version). Prints one JSON object on stdout and returns the exit code;
 * on an error it prints {"error": ...} and exits with 1 itself. */
int pt_cli_main(int argc, char **argv);
int pt_cli_is_command(const char *name);
const char *pt_cli_guide(void); /* AGENTS.md, compiled in: what `pdf-tools help` prints */
void pt_cli_usage(FILE *out);
#endif

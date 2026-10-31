/* <sys/wait.h> for MSVC, for the tests' pclose(): the C runtime's gives the exit code itself. */
#include "pt_compat.h"
#define WIFEXITED(status) ((status) != -1)
#define WEXITSTATUS(status) (status)

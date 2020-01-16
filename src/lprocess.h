#include "lua.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

typedef struct process
{
    int status;
#ifdef _WIN32
    HANDLE hProcess;
    DWORD dwProcessId;
#else
    pid_t pid;
#endif
} process;

#define PROCESS_METATABLE "ELI_PROCESS"

int process_create_meta(lua_State *L);

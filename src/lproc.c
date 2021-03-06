#include "lauxlib.h"
#include "lua.h"


#include "lprocess.h"
#include "lspawn.h"
#include "lutil.h"
#include "pipe.h"


#include <stdlib.h>
#include <string.h>


#ifdef _WIN32
#include <windows.h>

#define _lsleep Sleep
#define open _open
#define RDONLY_FLAG _O_RDONLY
#define WRONLY_FLAG _O_WRONLY | _O_TRUNC
#define SLEEP_MULTIPLIER 1e3
#else
#include <fcntl.h>
#include <unistd.h>


#define _lsleep usleep
#define RDONLY_FLAG O_RDONLY
#define WRONLY_FLAG O_WRONLY | O_TRUNC | O_CREAT
#define SLEEP_MULTIPLIER 1e6
#endif

static int lcheck_option_with_fallback(lua_State *L, int arg, const char *def,
                                       const char *fallback,
                                       const char *const lst[]) {
  const char *name =
      (def) ? luaL_optstring(L, arg, def) : luaL_checkstring(L, arg);
  int i;
  int j = -1;
  for (i = 0; lst[i]; i++) {
    if (strcmp(lst[i], name) == 0)
      return i;
    if (fallback && strcmp(lst[i], fallback) == 0)
      j = i;
  }
  if (j > -1)
    return j;

  return luaL_argerror(L, arg, lua_pushfstring(L, "invalid option '%s'", name));
}

static int get_redirect(lua_State *L, const char *stdname, int idx,
                        struct spawn_params *p) {
  stdioChannel *channel = malloc(sizeof(stdioChannel));
  channel->fdToClose = -1;
  channel->path = NULL;
  lua_getfield(L, idx, stdname);

  int stdioKind;

  switch (stdname[3]) {
  case 'i':
    stdioKind = STDIO_STDIN;
    break;
  case 'o':
    stdioKind = STDIO_STDOUT;
    break;
  case 'e':
    stdioKind = STDIO_STDERR;
    break;
  }

  int top = lua_gettop(L);
  switch (lua_type(L, -1)) {
  case LUA_TNIL: // fall through
  case LUA_TSTRING:;
    /** TODO
     * consider passing path
     * reuse code from luaL_checkoption to return one of options or fall back to
     * PATH mode
     */
    static const char *lst[] = {"ignore", "inherit", "pipe", "path", NULL};
    enum { IGNORE, INHERIT, PIPE, PATH };
    int kind = lcheck_option_with_fallback(
        L, -1, "pipe", "path", lst); // fallback to default pipe mode
    switch (kind) {
    case IGNORE:
      channel->kind = STDIO_CHANNEL_IGNORE_KIND;
      break;
    case INHERIT:
      channel->kind = STDIO_CHANNEL_INHERIT_KIND;
#ifdef _WIN32
      spawn_param_redirect(p, stdioKind, GetStdHandle(-10 + (-1 * stdioKind)));
#else
      spawn_param_redirect(p, stdioKind, stdioKind);
#endif
      break;
    case PATH:
      channel->kind = STDIO_CHANNEL_EXTERNAL_PATH_KIND;
      const char *path = luaL_checkstring(L, -1);
      channel->path = path;
      int fd;
      if (stdioKind == STDIO_STDIN) {
        if ((fd = open(path, RDONLY_FLAG)) == -1) {
          return push_error(L, "Failed to open stdin file!");
        }
      } else {
        if ((fd = open(path, WRONLY_FLAG)) == -1) {
          return push_error(L, "Failed to create stdout/stderr file!");
        }
      }
#ifdef _WIN32
      spawn_param_redirect(p, stdioKind, _get_osfhandle(fd));
#else
      spawn_param_redirect(p, stdioKind, fd);
#endif
      break;
    case PIPE:
      channel->kind = STDIO_CHANNEL_STREAM_KIND;
      PIPE_DESCRIPTORS descriptors;
      if (new_pipe(&descriptors) == -1) {
        return push_error(L, "Failed to create pipe!");
      };

      ELI_STREAM *stream = new_stream();
      stream->fd = descriptors.fd[stdioKind == STDIO_STDIN ? 1 : 0];
      channel->stream = stream;
#ifdef _WIN32
      spawn_param_redirect(
          p, stdioKind,
          _get_osfhandle(descriptors.fd[stdioKind == STDIO_STDIN ? 0 : 1]));
#else
      spawn_param_redirect(p, stdioKind,
                           descriptors.fd[stdioKind == STDIO_STDIN ? 0 : 1]);
#endif
      channel->fdToClose = descriptors.fd[stdioKind == STDIO_STDIN ? 0 : 1];
      break;
    default:
      luaL_error(L, "Invalid stdio type: %s!");
      return 1;
    }
    break;
  case LUA_TUSERDATA:
    lua_getmetatable(L, idx);
    luaL_getmetatable(L, LUA_FILEHANDLE);
    luaL_getmetatable(L, ELI_STREAM_RW_METATABLE);
    switch (stdioKind) {
    case STDIO_STDIN:
      luaL_getmetatable(L, ELI_STREAM_W_METATABLE);
      break;
    case STDIO_STDOUT:
    case STDIO_STDERR:
      luaL_getmetatable(L, ELI_STREAM_R_METATABLE);
      break;
    }

    if (lua_rawequal(L, -3, -4)) { // file
      lua_pop(L, lua_gettop(L) - top);
      luaL_Stream *fh = (luaL_Stream *)luaL_checkudata(L, -1, "FILE*");
      if (fh->closef == 0 || fh->f == NULL) {
        luaL_error(L, "%s: closed file");
        return 1;
      }

      channel->kind = STDIO_CHANNEL_EXTERNAL_FILE_KIND;
      channel->file = fh;
#ifdef _WIN32
      spawn_param_redirect(p, stdioKind, (HANDLE)_get_osfhandle(_fileno(fh)));
#else
      spawn_param_redirect(p, stdioKind, fileno(fh->f));
#endif
    } else if (lua_rawequal(L, -1, -4) || lua_rawequal(L, -2, -4)) { // eli pipe
      lua_pop(L, lua_gettop(L) - top);
      ELI_STREAM *stream = (ELI_STREAM *)lua_touserdata(L, -1);
      if (stream->closed) {
        luaL_error(L, "%s: closed pipe");
        return 1;
      }

      channel->kind = STDIO_CHANNEL_EXTERNAL_STREAM_KIND;
      channel->stream = stream;
#ifdef _WIN32
      spawn_param_redirect(p, stdioKind, (HANDLE)_get_osfhandle(stream->fd));
#else
      spawn_param_redirect(p, stdioKind, stream->fd);
#endif
    } else {
      lua_pop(L, lua_gettop(L) - top);
      luaL_typeerror(L, -1, "FILE*/ELI_STREAM");
      return 1;
    }
  }

  p->stdio[stdioKind] = channel;
  lua_pop(L, 1);
  return 0;
}

static int get_redirects(lua_State *L, int idx, struct spawn_params *p) {
  lua_getfield(L, idx, "stdio");

  // pipe, inherit, ignore are supported values
  switch (lua_type(L, -1)) {
  default:
    luaL_error(L, "bad args option (table expected, got %s)",
               luaL_typename(L, -1));
    return 1;
  case LUA_TNIL:
    // fall through
  case LUA_TSTRING:;
    static const char *lst[] = {"ignore", "inherit", "pipe", NULL};
    enum { IGNORE, INHERIT, PIPE };
    luaL_checkoption(L, -1, "pipe", lst); // force only limited set of strings
    const char *stdio = luaL_optstring(L, -1, "pipe");
    lua_pop(L, 1);
    // rebuild string to table
    lua_newtable(L);
    lua_pushstring(L, stdio);
    lua_setfield(L, -2, "stdin");
    lua_pushstring(L, stdio);
    lua_setfield(L, -2, "stdout");
    lua_pushstring(L, stdio);
    lua_setfield(L, -2, "stderr");
    break;
  case LUA_TTABLE:
    break;
  }
  int res;
  res = get_redirect(L, "stdin", -1, p);
  if (res)
    return res;
  res = get_redirect(L, "stdout", -1, p);
  if (res)
    return res;
  res = get_redirect(L, "stderr", -1, p);
  if (res)
    return res;
  lua_pop(L, 1);
  return 0;
}

/* filename [args-opts] -- proc/nil error */
/* args-opts -- proc/nil error */
static int eli_spawn(lua_State *L) {
  struct spawn_params *params;
  int have_options;
  switch (lua_type(L, 1)) {
  default:
    return luaL_typeerror(L, 1, "string or table");
  case LUA_TSTRING:
    switch (lua_type(L, 2)) {
    default:
      return luaL_typeerror(L, 2, "table");
    case LUA_TNONE:
      have_options = 0;
      break;
    case LUA_TTABLE:
      have_options = 1;
      break;
    }
    break;
  case LUA_TTABLE:
    have_options = 1;
    lua_getfield(L, 1, "command"); /* opts ... cmd */
    if (!lua_isnil(L, -1)) {
      /* convert {command=command,arg1,...} to command {arg1,...} */
      lua_insert(L, 1); /* cmd opts ... */
    } else {
      /* convert {arg0,arg1,...} to arg0 {arg1,...} */
      size_t i, n = lua_rawlen(L, 1);
      lua_rawgeti(L, 1, 1); /* opts ... nil cmd */
      lua_insert(L, 1);     /* cmd opts ... nil */
      for (i = 2; i <= n; i++) {
        lua_rawgeti(L, 2, i);     /* cmd opts ... nil argi */
        lua_rawseti(L, 2, i - 1); /* cmd opts ... nil */
      }
      lua_rawseti(L, 2, n); /* cmd opts ... */
    }
    if (lua_type(L, 1) != LUA_TSTRING)
      return luaL_error(L, "bad command option (string expected, got %s)",
                        luaL_typename(L, 1));
    break;
  }

  params = spawn_param_init(L);
  /* get filename to execute */
  spawn_param_filename(params, lua_tostring(L, 1));
  /* get arguments, environment, and redirections */
  if (have_options) {
    lua_getfield(L, 2, "args"); /* cmd opts ... argtab */
    switch (lua_type(L, -1)) {
    default:
      return luaL_error(L, "bad args option (table expected, got %s)",
                        luaL_typename(L, -1));
    case LUA_TNIL:
      lua_pop(L, 1);       /* cmd opts ... */
      lua_pushvalue(L, 2); /* cmd opts ... opts */
                           /*FALLTHRU*/
    case LUA_TTABLE:
      if (lua_rawlen(L, 2) > 0)
        return luaL_error(
            L, "cannot specify both the args option and array values");
      spawn_param_args(params); /* cmd opts ... */
      break;
    }
    lua_getfield(L, 2, "env"); /* cmd opts ... envtab */
    switch (lua_type(L, -1)) {
    default:
      return luaL_error(L, "bad env option (table expected, got %s)",
                        luaL_typename(L, -1));
    case LUA_TNIL:
      break;
    case LUA_TTABLE:
      spawn_param_env(params); /* cmd opts ... */
      break;
    }
  }
  int err_count = get_redirects(L, 2, params); /* cmd opts ... */
  if (err_count > 0) {
    return err_count;
  }
  return spawn_param_execute(params); /* proc/nil error */
}

static const struct luaL_Reg eliProcExtra[] = {
    {"spawn", eli_spawn},
    {NULL, NULL},
};

int luaopen_eli_proc_extra(lua_State *L) {
  process_create_meta(L);

  lua_newtable(L);
  luaL_setfuncs(L, eliProcExtra, 0);
  return 1;
}

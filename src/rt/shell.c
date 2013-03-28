//
//  Copyright (C) 2011-2013  Nick Gasson
//
//  This program is free software: you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation, either version 3 of the License, or
//  (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program.  If not, see <http://www.gnu.org/licenses/>.
//

#include "util.h"
#include "slave.h"
#include "rt.h"
#include "tree.h"
#include "common.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <inttypes.h>

#if defined HAVE_TCL_TCL_H
#include <tcl/tcl.h>
#elif defined HAVE_TCL_H
#include <tcl.h>
#endif

#ifdef HAVE_LIBREADLINE
#include <readline/readline.h>
#endif

#ifdef HAVE_READLINE_HISTORY
#include <readline/history.h>
#endif

static int shell_cmd_restart(ClientData cd, Tcl_Interp *interp,
                             int objc, Tcl_Obj *const objv[])
{
   slave_post_msg(SLAVE_RESTART, NULL, 0);
   return TCL_OK;
}

static int shell_cmd_run(ClientData cd, Tcl_Interp *interp,
                         int objc, Tcl_Obj *const objv[])
{
   uint64_t time = UINT64_MAX;
   if (objc == 3) {
      Tcl_WideInt base;
      int error = Tcl_GetWideIntFromObj(interp, objv[1], &base);
      if (error != TCL_OK || base <= 0) {
         fprintf(stderr, "invalid time\n");
         return TCL_ERROR;
      }
      const char *unit = Tcl_GetString(objv[2]);

      uint64_t mult;
      if      (strcmp(unit, "fs") == 0) mult = 1;
      else if (strcmp(unit, "ps") == 0) mult = 1000;
      else if (strcmp(unit, "ns") == 0) mult = 1000000;
      else if (strcmp(unit, "us") == 0) mult = 1000000000;
      else if (strcmp(unit, "ms") == 0) mult = 1000000000000;
      else {
         fprintf(stderr, "invalid time unit %s", unit);
         return TCL_ERROR;
      }

      time = base * mult;
   }
   else if (objc != 1) {
      fprintf(stderr, "usage: run [time units]\n");
      return TCL_ERROR;
   }

   slave_run_msg_t msg = { .time = time };
   slave_post_msg(SLAVE_RUN, &msg, sizeof(msg));

   slave_msg_t event;
   do {
      slave_get_msg(&event, NULL, NULL);

      switch (event) {
      case EVENT_STOP:
         break;
      default:
         fatal("unhandled slave event %d", event);
      }
   } while (event != EVENT_STOP);

   return TCL_OK;
}

static int shell_cmd_quit(ClientData cd, Tcl_Interp *interp,
                          int objc, Tcl_Obj *const objv[])
{
   slave_post_msg(SLAVE_QUIT, NULL, 0);
   bool *have_quit = (bool*)cd;
   *have_quit = true;
   return TCL_OK;
}

static int shell_cmd_show(ClientData cd, Tcl_Interp *interp,
                          int objc, Tcl_Obj *const objv[])
{
   tree_t top = cd;

   if (objc == 1) {
      fprintf(stderr, "try 'show -help' for usage\n");
      return TCL_ERROR;
   }

   const char *what = Tcl_GetString(objv[1]);
   if (strcmp(what, "-help") == 0) {
      printf("Usage: show [something]\n"
             "  -signal  - list all signals in design with current value\n"
             "  -process - list all processes in design\n"
             "  -alias   - list all aliases in design\n");
   }
   else if (strcmp(what, "-signal") == 0) {
      for (unsigned i = 0; i < tree_decls(top); i++) {
         tree_t d = tree_decl(top, i);
         if (tree_kind(d) != T_SIGNAL_DECL)
            continue;

         size_t len = 1;
         type_t type = tree_type(d);
         while (type_is_array(type)) {
            int64_t low = 0, high = 0;
            range_bounds(type_dim(type, 0), &low, &high);
            len *= (high - low + 1);

            type = type_elem(type);
         }

         slave_read_signal_msg_t msg = {
            .index = tree_index(d),
            .len   = len
         };
         slave_post_msg(SLAVE_READ_SIGNAL, &msg, sizeof(msg));

         const size_t rsz =
            sizeof(reply_read_signal_msg_t) + (msg.len * sizeof(uint64_t));
         reply_read_signal_msg_t *reply = xmalloc(rsz);
         slave_get_reply(REPLY_READ_SIGNAL, reply, rsz);

         const char *type_str = type_pp(tree_type(d));
         const char *short_name = strrchr(type_str, '.');

         printf("%-30s%-20s%s\n",
                istr(tree_ident(d)),
                (short_name != NULL ? short_name + 1 : type_str),
                pprint(d, reply->values, msg.len));

         free(reply);
      }
   }
   else if (strcmp(what, "-process") == 0) {
      for (unsigned i = 0; i < tree_stmts(top); i++) {
         tree_t p = tree_stmt(top, i);
         printf("%s\n", istr(tree_ident(p)));
      }
   }
   else if (strcmp(what, "-alias") == 0) {
      for (unsigned i = 0; i < tree_decls(top); i++) {
         tree_t a = tree_decl(top, i);
         if (tree_kind(a) != T_ALIAS)
            continue;

         printf("%s\n", istr(tree_ident(a)));
      }
   }
   else {
      fprintf(stderr, "cannot show '%s' - try 'show help' for usage\n", what);
      return TCL_ERROR;
   }

   return TCL_OK;
}

static int shell_cmd_help(ClientData cd, Tcl_Interp *interp,
                          int objc, Tcl_Obj *const objv[])
{
   printf(
      "NVC commands:\n"
      "  show\t\tDisplay simulation objects\n"
      "  restart\tRestart simulation\n"
      "  quit\t\tExit simulation\n"
      "Use -help on each command for detailed usage\n"
      "\n"
      "TCL commands:\n");

   return TCL_OK;
}

static int shell_cmd_copyright(ClientData cd, Tcl_Interp *interp,
                               int objc, Tcl_Obj *const objv[])
{
   extern char *copy_string;
   printf("%s\n", copy_string);
   return TCL_OK;
}

static char *shell_get_line(void)
{
   if (isatty(fileno(stdin))) {
#ifdef HAVE_LIBREADLINE
      char *buf = readline("% ");
      if ((buf != NULL) && (*buf != '\0'))
         add_history(buf);
      return buf;
#else   // HAVE_LIBREADLINE
      printf("%% ");
      fflush(stdout);
#endif  // HAVE_LIBREADLINE
   }

   size_t buflen = 256;
   char *buf = xmalloc(buflen);

   size_t off = 0;
   for (;;) {
      if (off == buflen) {
         buflen *= 2;
         buf = xrealloc(buf, buflen);
      }

      int ch = fgetc(stdin);
      switch (ch) {
      case EOF:
         buf[off] = '\0';
         return (off > 0 ? buf : NULL);

      case '\n':
         buf[off] = '\0';
         return buf;

      default:
         buf[off++] = ch;
      }
   }
}

static void shell_exit_handler(ClientData cd)
{
   bool *have_quit = cd;

   if (!*have_quit)
      slave_post_msg(SLAVE_QUIT, NULL, 0);

   slave_wait();
}

static void show_banner(void)
{
   extern const char *version_string;
   printf("%s\n", version_string);
   printf("Type \"help\" or \"copyright\" for more information.\n");
}

void shell_run(tree_t e)
{
   Tcl_Interp *interp = Tcl_CreateInterp();

   bool have_quit = false;

   Tcl_CreateExitHandler(shell_exit_handler, &have_quit);

   Tcl_CreateObjCommand(interp, "quit", shell_cmd_quit, &have_quit, NULL);
   Tcl_CreateObjCommand(interp, "run", shell_cmd_run, NULL, NULL);
   Tcl_CreateObjCommand(interp, "restart", shell_cmd_restart, NULL, NULL);
   Tcl_CreateObjCommand(interp, "show", shell_cmd_show, e, NULL);
   Tcl_CreateObjCommand(interp, "help", shell_cmd_help, e, NULL);
   Tcl_CreateObjCommand(interp, "copyright", shell_cmd_copyright, e, NULL);

   show_banner();

   slave_post_msg(SLAVE_RESTART, NULL, 0);

   char *line;
   while (!have_quit && (line = shell_get_line())) {
      switch (Tcl_Eval(interp, line)) {
      case TCL_OK:
         break;
      case TCL_ERROR:
         fprintf(stderr, "%s\n", Tcl_GetStringResult(interp));
         break;
      default:
         assert(false);
      }

      free(line);
   }

   printf("\nBye.\n");

   Tcl_Exit(EXIT_SUCCESS);
}

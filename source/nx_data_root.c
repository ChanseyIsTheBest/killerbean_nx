/* nx_data_root.c -- see nx_data_root.h.
 *
 * Resolution order, first hit wins. Every candidate is VALIDATED by stat()ing
 * libmain.so inside it, so a plausible-but-wrong path is rejected rather than
 * silently adopted:
 *
 *   1. argv[0]'s directory        -- hbloader passes the full .nro path, so
 *                                    this makes the folder name irrelevant.
 *   2. the compile-time default   -- sdmc:/switch/<GAME_FOLDER>
 *   3. the legacy name            -- sdmc:/switch/killerbean, which earlier
 *                                    revisions of this port expected
 *   4. a bounded scan of sdmc:/switch/ for any folder holding libmain.so
 *   5. give up -> the compile-time default, so the on-screen error names a
 *                 sensible path rather than an empty string
 *
 * Step 4 exists because the failure it prevents is genuinely hard to diagnose
 * from the console: no log, and an error naming a file the user can see.
 */
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include "config.h"
#include "nx_data_root.h"

char g_data_root[512];
char g_log_path[576];
char g_data_root_how[512];

#ifndef DATA_ROOT_DEFAULT
#define DATA_ROOT_DEFAULT "sdmc:/switch/" GAME_FOLDER
#endif
#define DATA_ROOT_LEGACY  "sdmc:/switch/killerbean"
#define SWITCH_DIR        "sdmc:/switch"

/* A directory counts as the game folder only if libmain.so is in it. Cheap,
 * and it is exactly the file whose absence started this. */
static int looks_like_root(const char *dir) {
  char p[640];
  struct stat st;
  if (!dir || !*dir) return 0;
  snprintf(p, sizeof p, "%s/libmain.so", dir);
  return stat(p, &st) == 0 && st.st_size > 0;
}

static void adopt(const char *dir, const char *how) {
  snprintf(g_data_root, sizeof g_data_root, "%s", dir);
  snprintf(g_log_path,  sizeof g_log_path,  "%s/debug.log", g_data_root);
  snprintf(g_data_root_how, sizeof g_data_root_how, "%s", how);
}

void nx_resolve_data_root(int argc, char *argv[]) {
  char cand[512];

  /* ---- 1. the directory the .nro was launched from ---------------------- */
  if (argc >= 1 && argv && argv[0] && argv[0][0]) {
    const char *a0 = argv[0];
    /* hbloader normally gives "sdmc:/switch/<dir>/<name>.nro". Some launchers
     * hand over a bare "/switch/..." with no device prefix; normalise that. */
    if (!strchr(a0, ':') && a0[0] == '/')
      snprintf(cand, sizeof cand, "sdmc:%s", a0);
    else
      snprintf(cand, sizeof cand, "%s", a0);

    char *slash = strrchr(cand, '/');
    if (slash && slash != cand) {
      *slash = '\0';                       /* strip "/<name>.nro"            */
      if (looks_like_root(cand)) {
        adopt(cand, "from argv[0] (.nro location)");
        return;
      }
    }
  }

  /* ---- 2/3. compile-time default, then the legacy name ------------------ */
  if (looks_like_root(DATA_ROOT_DEFAULT)) {
    adopt(DATA_ROOT_DEFAULT, "compile-time default");
    return;
  }
  if (looks_like_root(DATA_ROOT_LEGACY)) {
    adopt(DATA_ROOT_LEGACY, "legacy folder name");
    return;
  }

  /* ---- 4. bounded scan of sdmc:/switch/ --------------------------------- */
  {
    DIR *d = opendir(SWITCH_DIR);
    if (d) {
      struct dirent *de;
      int looked = 0;
      while ((de = readdir(d)) != NULL && looked < 256) {
        if (de->d_name[0] == '.') continue;
        looked++;
        snprintf(cand, sizeof cand, "%s/%s", SWITCH_DIR, de->d_name);
        if (looks_like_root(cand)) {
          char how[512];
          snprintf(how, sizeof how, "found by scanning %s (folder '%s')",
                   SWITCH_DIR, de->d_name);
          closedir(d);
          adopt(cand, how);
          return;
        }
      }
      closedir(d);
    }
  }

  /* ---- 5. nothing validated; keep the default so errors read sensibly --- */
  adopt(DATA_ROOT_DEFAULT, "NOT FOUND -- libmain.so is not in any candidate; "
                           "falling back to the compile-time default");
}

const char *nx_path(const char *sub) {
  static char buf[8][768];
  static unsigned n = 0;
  char *b = buf[n++ & 7];
  snprintf(b, sizeof buf[0], "%s%s", g_data_root, sub ? sub : "");
  return b;
}

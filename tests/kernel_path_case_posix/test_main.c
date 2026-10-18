/* The POSIX path backend's case-insensitive lookup: Xbox titles open
 * "d:\FRONT\MOVIES\MIDWAY.SFD" while an extracted disc holds
 * "front/movies/midway.sfd" (or a mix). Components that exist keep their
 * spelling on disk; a missing tail (a file about to be created) stays as the
 * title spelled it. Also checks the [PATH] hook fires as on Win32. */
#define _GNU_SOURCE
#include "kernel.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

void xbox_log(int level, const char *subsystem, const char *fmt, ...)
{ (void)level; (void)subsystem; (void)fmt; }

const char *xbox_LookupSymbolicLink(const char *link)
{ (void)link; return NULL; }

static int failures, hook_calls;
static void check(int ok, const char *name)
{
    printf("%s: %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}
static void hook(const char *p) { (void)p; hook_calls++; }

static void expect(const char *root, const char *guest, const char *rel, const char *name)
{
    char host[MAX_PATH], want[MAX_PATH];
    int ok = xbox_translate_path(guest, host, MAX_PATH);
    snprintf(want, sizeof want, "%s/%s", root, rel);
    if (!ok || strcmp(host, want) != 0)
        printf("  got %s\n  want %s\n", host, want);
    check(ok && strcmp(host, want) == 0, name);
}
static void touch(const char *root, const char *rel)
{
    char p[MAX_PATH]; FILE *f;
    snprintf(p, sizeof p, "%s/%s", root, rel);
    f = fopen(p, "wb"); if (f) fclose(f);
}
static void mk(const char *root, const char *rel)
{
    char p[MAX_PATH];
    snprintf(p, sizeof p, "%s/%s", root, rel);
    mkdir(p, 0700);
}
int main(void)
{
    char root[] = "/tmp/xbox-path-case-XXXXXX", cmd[600];
    if (!mkdtemp(root)) return 2;
    mk(root, "front"); mk(root, "front/movies"); touch(root, "front/movies/midway.sfd");
    touch(root, "GAMEDATA.WAD"); mk(root, "English"); touch(root, "English/Text.Bin");
    xbox_path_init(root, root);
    g_xbox_path_hook = hook;

    expect(root, "d:\\FRONT\\MOVIES\\MIDWAY.SFD", "front/movies/midway.sfd",
           "upper-case guest path finds the lower-case file");
    expect(root, "d:\\gamedata.wad", "GAMEDATA.WAD",
           "lower-case guest path finds the upper-case file");
    expect(root, "d:\\english\\TEXT.BIN", "English/Text.Bin", "mixed case on disk");
    expect(root, "d:\\front\\movies\\midway.sfd", "front/movies/midway.sfd",
           "exact spelling unchanged");
    expect(root, "d:\\FRONT\\NEW\\File.Dat", "front/NEW/File.Dat",
           "missing tail kept as spelled after the matched prefix");
    expect(root, "d:\\FRONT\\MOVIES\\MIDWAY.SFDX", "front/movies/MIDWAY.SFDX",
           "no prefix match on a longer name");
    check(hook_calls == 6, "path hook called once per translation");

    snprintf(cmd, sizeof cmd, "rm -rf '%s'", root);
    if (system(cmd) != 0) failures++;
    return failures ? 1 : 0;
}

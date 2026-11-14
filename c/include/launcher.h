/*
 * launcher.h — the Minecraft launcher side of the application.
 *
 * The launcher is deliberately split from the building generator: it only
 * knows about the .minecraft directory, Mojang's version metadata and starting
 * a Java process. Generation keeps using mb.h.
 */
#ifndef MB_LAUNCHER_H
#define MB_LAUNCHER_H

#include "mb.h"

#define MB_LAUNCHER_PATH 512
#define MB_LAUNCHER_MAX_VERSIONS 128

typedef struct {
    char      id[64];
    char      type[16];       /* release / snapshot / old_beta ... */
    bool      installed;      /* versions/<id>/<id>.json exists */
    long long releaseTime;    /* milliseconds since epoch, 0 if unknown */
} MBVersionEntry;

typedef struct {
    char gameDir[MB_LAUNCHER_PATH];
    char versionId[64];
    char playerName[64];
    int  memoryMb;            /* -Xmx, default 2048 */
    bool demo;
} MBLaunchOptions;

/* Called from the install worker. `stage` is a stable Chinese label, `item` is
 * the file being processed (may be NULL). The callback must not block long. */
typedef void (*MBLauncherProgress)(void* user, const char* stage,
                                   int done, int total, const char* item);

/* %APPDATA%\.minecraft, or an empty string when APPDATA is missing. */
const char* mb_launcher_default_game_dir(char* buf, size_t cap);
bool        mb_launcher_dir_exists(const char* gameDir);

/* Installed versions, newest first. Returns the number written (0 on error). */
int mb_launcher_scan_versions(const char* gameDir, MBVersionEntry* out, int cap);

/* Fetch the Mojang version manifest and merge remote entries with the local
 * install state. Network failure returns false and leaves `out` empty. */
bool mb_launcher_fetch_versions(Arena* a, const char* gameDir,
                                MBVersionEntry* out, int cap, int* count,
                                bool releasesOnly, char* err, size_t errCap);

/* Download one version (json, client jar, libraries, natives, asset index and
 * all asset objects) into gameDir. Existing files are skipped by size. */
bool mb_launcher_install(Arena* a, const char* gameDir, const char* versionId,
                         MBLauncherProgress progress, void* user,
                         char* err, size_t errCap);

/* Build and start javaw for an installed version. The access token is offline
 * ("0"); Microsoft accounts are a separate, later step. */
bool mb_launcher_launch(Arena* a, const MBLaunchOptions* opt,
                        char* err, size_t errCap);

/* Best Java for a required major version: bundled runtime first, then system. */
bool mb_launcher_java(Arena* a, const char* gameDir, int requiredMajor,
                      char* out, size_t cap);

#endif /* MB_LAUNCHER_H */

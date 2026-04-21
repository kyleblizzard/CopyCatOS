// CopyCatOS — by Kyle Blizzard at Blizzard.show

// hello.c — minimum MoonBase link test.
//
// Phase B verification: proves that every symbol the app touches
// resolves at link time against libmoonbase.so.1, and that the stub
// convention exits cleanly. This is the "runs, exits cleanly" check
// from tasks/moonbase-plumbing-tasks.md Phase B. Phase C rewires
// the same symbols to real IPC against MoonRock — this file does
// not change then.
//
// The init/quit return values are intentionally ignored here. Once
// moonbase_init and moonbase_quit have real bodies, a correct app
// checks the init return; the Phase B scaffold accepts MB_ENOSYS as
// a signal that nothing's wired yet without refusing to run.

#include <moonbase.h>

int main(void) {
    (void)moonbase_init(0, NULL);
    moonbase_quit(0);
    return 0;
}

/* <fcntl.h>'s open flags on the pair the program runs on: each value as the platform's own header spells it (pinned
 * per target in expected.json) and what the kernel DOES with it. Linux arm64 overrides O_DIRECTORY, O_NOFOLLOW (and
 * O_TMPFILE, which contains O_DIRECTORY) relative to x86_64; given x86_64's values, an arm64 O_DIRECTORY is the
 * kernel's O_DIRECT and an arm64 O_NOFOLLOW is a bit the kernel does not read as NOFOLLOW, so the file cell opens a
 * regular file as a "directory" and the link cell silently FOLLOWS a symlink. Exit 42 iff both refusals happen. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

int main(void) {
    char dir[64], file[64], symlinkPath[64];
    int pid = getpid();
    snprintf(dir, sizeof dir, "fcntl_flags_%d_dir", pid);
    snprintf(file, sizeof file, "fcntl_flags_%d_file", pid);
    snprintf(symlinkPath, sizeof symlinkPath, "fcntl_flags_%d_link", pid);

    printf("O_DIRECTORY=%#x O_NOFOLLOW=%#x", (unsigned)O_DIRECTORY, (unsigned)O_NOFOLLOW);
#ifdef O_TMPFILE
    printf(" O_TMPFILE=%#x", (unsigned)O_TMPFILE);
#endif
    printf("\n");

    if (mkdir(dir, 0700) != 0) return 2;
    int created = open(file, O_CREAT | O_WRONLY | O_TRUNC, 0600);
    if (created < 0) return 3;
    close(created);
    if (symlink(file, symlinkPath) != 0) return 4;

    int onDir = open(dir, O_RDONLY | O_DIRECTORY);
    int dirOk = onDir >= 0;
    if (onDir >= 0) close(onDir);

    errno = 0;
    int onFile = open(file, O_RDONLY | O_DIRECTORY);
    int fileErrno = onFile < 0 ? errno : 0;
    if (onFile >= 0) close(onFile);

    errno = 0;
    int onLink = open(symlinkPath, O_RDONLY | O_NOFOLLOW);
    int linkErrno = onLink < 0 ? errno : 0;
    if (onLink >= 0) close(onLink);

    unlink(symlinkPath);
    unlink(file);
    rmdir(dir);

    printf("dir=%s file_errno=%d link_errno=%d\n", dirOk ? "opened" : "refused", fileErrno, linkErrno);
    return (dirOk && onFile < 0 && onLink < 0) ? 42 : 1;
}

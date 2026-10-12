/* atlas-ctl — talk to a running atlas over /tmp/atlas.sock */
#include <sys/socket.h>
#include <sys/un.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: atlas-ctl <command> [args...]\n"
                        "  status | list | goto X Y | zoom Z\n"
                        "  locate PATTERN | focus PATTERN\n"
                        "  spawn CMD... | card | quit\n");
        return 2;
    }
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    snprintf(a.sun_path, sizeof a.sun_path, "/tmp/atlas.sock");
    if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0) {
        fprintf(stderr, "atlas-ctl: cannot reach atlas (is it running?)\n");
        return 1;
    }
    char buf[2048]; int off = 0;
    for (int i = 1; i < argc; i++) {
        int w = snprintf(buf + off, sizeof buf - off, "%s%s",
                         i == 1 ? "" : " ", argv[i]);
        if (w < 0 || off + w >= (int)sizeof buf) break;
        off += w;
    }
    buf[off++] = '\n';
    write(fd, buf, off);

    char resp[8192]; ssize_t n;
    while ((n = read(fd, resp, sizeof resp)) > 0)
        fwrite(resp, 1, n, stdout);
    close(fd);
    return 0;
}

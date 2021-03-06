#include <stdlib.h>
#include <string.h>

#include <uv.h>

#include "../include/raft.h"

#include "io_uv_ip.h"

int raft__io_uv_ip_parse(const char *address, struct sockaddr_in *addr)
{
    char buf[256];
    char *host;
    char *port;
    char *colon = ":";
    int rv;

    /* TODO: turn this poor man parsing into proper one */
    strcpy(buf, address);
    host = strtok(buf, colon);
    port = strtok(NULL, ":");
    if (port == NULL) {
        port = "8080";
    }

    rv = uv_ip4_addr(host, atoi(port), addr);
    if (rv != 0) {
        return RAFT_ERR_IO_CONNECT;
    }

    return 0;
}

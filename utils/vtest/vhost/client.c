/*
 * client.c
 *
 * Copyright (c) 2015 Juniper Networks, Inc. All rights reserved.
*/

#include <errno.h>
#include <linux/vhost.h>
#include <limits.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>


#include "client.h"

int
client_init_path(Client *client, const char *path) {

    if (!client || !path || strlen(path) == 0) {
        return E_CLIENT_ERR_FARG;
    }

   strncpy(client->socket_path, path, PATH_MAX);

   return E_CLIENT_OK;
}

int
clinet_init_socket(Client *client) {

    if (!client) {
        return E_CLIENT_ERR_FARG;
    }

    client->socket = socket(AF_UNIX, SOCK_STREAM, 0);
    if (client->socket == -1) {

        return E_CLIENT_ERR_SOCK;
    }
    return E_CLIENT_OK;

}

int
client_connect_socket(Client *client) {

    struct sockaddr_un unix_socket = {0, .sun_path = {0}};
    size_t addrlen = 0;

    if (!client->socket || strlen(client->socket_path) == 0) {
        return E_CLIENT_ERR_FARG;
    }

    unix_socket.sun_family = AF_UNIX;
    strncpy(unix_socket.sun_path, client->socket_path, PATH_MAX);
    addrlen = strlen(unix_socket.sun_path) + sizeof(AF_UNIX);

    if (connect(client->socket, (struct sockaddr *)&unix_socket, addrlen)  == -1) {

        return E_CLIENT_ERR_CONN;
    }

    return E_CLIENT_OK;
}

int
client_disconnect_socket(Client *client) {

    if (!client) {
        return E_CLIENT_ERR_FARG;
    }

    close(client->socket);

    return E_CLIENT_OK;
}

int
client_vhost_ioctl(Client *client, VhostUserRequest request, void *req_ptr) {

    Client *const cl = client;
    int fds[VHOST_MEMORY_MAX_NREGIONS] = {-2};
    VhostUserMsg message = {0, .flags=0};
    CLIENT_H_RET_VAL ret_val = E_CLIENT_OK;
    CLIENT_H_RET_VAL ret_set_val = E_CLIENT_VIOCTL_REPLY;
    size_t fd_num = 0;

    if (!client) {
        return E_CLIENT_ERR_FARG;
    }

    /* Function argument pointer (req_ptr) for following messages
     * SHOULD not be NULL. */
    switch (request) {
        case VHOST_USER_SET_MEM_TABLE:
        case VHOST_USER_SET_LOG_BASE:
        case VHOST_USER_SET_LOG_FD:
        case VHOST_USER_SET_VRING_KICK:
        case VHOST_USER_SET_VRING_CALL:
        case VHOST_USER_SET_VRING_ERR:
            if (!req_ptr) {
                return E_CLIENT_ERR_FARG;
            }
            break;

        default:
            break;
    }

    message.request = request;
    message.flags &= ~VHOST_USER_VERSION_MASK;
    message.flags |= QEMU_PROT_VERSION;

    /* Set message structure for sending data. */
    ret_set_val = client_vhost_ioctl_set_send_msg(cl, request, req_ptr, &message, fds, &fd_num);

    if (!(ret_set_val == E_CLIENT_OK || ret_set_val == E_CLIENT_VIOCTL_REPLY)) {
        return E_CLIENT_ERR_VIOCTL;
    }

    ret_val = client_vhost_ioctl_send_fds(&message, cl->socket, fds, fd_num );
    if (ret_val != E_CLIENT_OK) {
        return ret_val;
    }

    if (ret_set_val == E_CLIENT_VIOCTL_REPLY) {

        /* Set message structure after receive data */
        ret_val = client_vhost_ioctl_set_recv_msg(request, req_ptr, &message);
        if (!(ret_val == E_CLIENT_OK)) {
            return E_CLIENT_ERR_VIOCTL;
        }

        ret_val = client_vhost_ioctl_recv_fds(cl->socket, &message, fds, &fd_num);

        if (ret_val != E_CLIENT_OK) {
            return ret_val;
        }
    }

    return E_CLIENT_OK;
}

int
client_vhost_ioctl_set_send_msg(Client *client, VhostUserRequest request, void *req_ptr,
                     VhostUserMsg *msg, int *fds, size_t *fd_num ) {

    VhostUserMsg *const message = msg;
    bool msg_has_reply = false;

    size_t *const l_fd_num = fd_num;
    struct vring_file {unsigned int index; int fd;} *file;

    if (!client || !msg || !fds || !fd_num) {

        return E_CLIENT_ERR_FARG;
    }

    switch (request) {

        case VHOST_USER_NONE:
            break;

        case VHOST_USER_GET_FEATURES:
        case VHOST_USER_GET_VRING_BASE:
            msg_has_reply = true;
            break;

        case VHOST_USER_SET_FEATURES:
        case VHOST_USER_SET_LOG_BASE:
            message->u64 = *((uint64_t *) req_ptr);
            message->size = sizeof(((VhostUserMsg*)0)->u64);
            /* if VHOST_USER_PROTOCOL_F_LOG_SHMFD
            msg_has_reply = true;
            */
            break;

        case VHOST_USER_SET_OWNER:
        case VHOST_USER_RESET_OWNER:
            break;

        case VHOST_USER_SET_MEM_TABLE:
            memcpy(&message->memory, req_ptr, sizeof(VhostUserMemory));
            message->size = sizeof(((VhostUserMemory*)0)->padding);
            message->size += sizeof(((VhostUserMemory*)0)->nregions);

            for (*l_fd_num = 0; *l_fd_num < message->memory.nregions; fd_num++) {
                fds[*l_fd_num] = client->sh_mem_fds[*l_fd_num];
                message->size = sizeof(VhostUserMemoryRegion);
            }
            break;

        case VHOST_USER_SET_LOG_FD:
            fds[++(*l_fd_num)] = *((int *) req_ptr);
            break;

        case VHOST_USER_SET_VRING_NUM:
        case VHOST_USER_SET_VRING_BASE:
            memcpy(&message->state, req_ptr, sizeof(((VhostUserMsg*)0)->state));
            message->size = sizeof(((VhostUserMsg*)0)->state);
            break;

        case VHOST_USER_SET_VRING_ADDR:
            memcpy(&message->addr, req_ptr, sizeof(((VhostUserMsg*)0)->addr));
            message->size = sizeof(((VhostUserMsg*)0)->addr);
            break;

        case VHOST_USER_SET_VRING_KICK:
        case VHOST_USER_SET_VRING_CALL:
        case VHOST_USER_SET_VRING_ERR:
            file = req_ptr;
            message->u64 = file->index;
            message->size = sizeof(((VhostUserMsg*)0)->u64);
            if (file->fd > 0 ) {
                client->sh_mem_fds[(*l_fd_num)++] = file->fd;
            }
            break;

        default:
            return E_CLIENT_ERR_IOCTL_SEND;

    }
    if (msg_has_reply) {
       return E_CLIENT_VIOCTL_REPLY;
    }

    return E_CLIENT_OK;
}

int
client_vhost_ioctl_set_recv_msg(VhostUserRequest request, void *req_ptr, VhostUserMsg *msg) {

    VhostUserMsg *const message = msg;

    if (!msg) {

        return E_CLIENT_ERR_FARG;
    }

    switch (request) {
        case VHOST_USER_GET_FEATURES:
            *((uint64_t *) req_ptr) = message->u64;
            break;
        case VHOST_USER_GET_VRING_BASE:
            memcpy(req_ptr, &message->state, sizeof(struct vhost_vring_state));

        default:
            return E_CLIENT_ERR_IOCTL_REPLY;
    }

    return E_CLIENT_OK;
}

int
client_vhost_ioctl_send_fds(VhostUserMsg *msg, int fd, int *fds, size_t fd_num) {

    struct iovec iov;
    struct msghdr msgh;
    struct cmsghdr *cmsgh = NULL;
    char controlbuf[CMSG_SPACE(fd_num * sizeof(int))];

    size_t vhost_user_msg_member_size = ((sizeof(((VhostUserMsg*)0)->request)) +
    (sizeof(((VhostUserMsg*)0)->flags)) + (sizeof(((VhostUserMsg*)0)->size)));

    const VhostUserMsg *const message = msg;
    int ret = 0;

    if (!msg || !fds) {
        return E_CLIENT_ERR_FARG;
    }

    memset(controlbuf, 0, sizeof(controlbuf));
    memset(&msgh, 0, sizeof(struct msghdr));
    memset(&iov, 0, sizeof(struct iovec));

    iov.iov_base = (void *) message;
    iov.iov_len = vhost_user_msg_member_size + message->size;

    msgh.msg_iov = &iov;
    msgh.msg_iovlen = 1;

    if (fd_num) {
        msgh.msg_name = NULL;
        msgh.msg_namelen = 0;
        msgh.msg_control = controlbuf;

        cmsgh = CMSG_FIRSTHDR(&msgh);
        cmsgh->cmsg_len = CMSG_LEN(sizeof(int) * fd_num);
        cmsgh->cmsg_level = SOL_SOCKET;
        cmsgh->cmsg_type = SCM_RIGHTS;

        msgh.msg_controllen = cmsgh->cmsg_len;

        memcpy(CMSG_DATA(cmsgh), fds, sizeof(int) * fd_num);

    } else {
        msgh.msg_control = NULL;
        msgh.msg_controllen = 0;
    }

    do {
        ret = sendmsg(fd, &msgh, 0);

    } while (ret < 0 && errno == EINTR);

    if (ret < 0) {
        return E_CLIENT_ERR_IOCTL_SEND;
    }

    return E_CLIENT_OK;
}

int
client_vhost_ioctl_recv_fds(int fd, VhostUserMsg *msg, int *fds, size_t *fd_num) {

    struct  msghdr msgh;
    struct iovec iov;
    struct cmsghdr *cmsgh = NULL;
    char controlbuf[CMSG_SPACE(sizeof(int) * (*fd_num))];
    VhostUserMsg *const message = msg;
    int *const l_fds = fds;
    size_t *const l_fd_num = fd_num;
    int ret = 0;

    if (!msg || !fds || !fd_num) {

        return E_CLIENT_ERR_FARG;
    }

    memset(controlbuf, 0, sizeof(controlbuf));
    memset(&msgh, 0, sizeof(struct msghdr));
    memset(&iov, 0, sizeof(struct iovec));

    msgh.msg_name = NULL;
    msgh.msg_namelen = 0;

    msgh.msg_iov = &iov;
    msgh.msg_iovlen = 1;
    msgh.msg_control = controlbuf;
    msgh.msg_controllen = sizeof(controlbuf);

    iov.iov_base = (void *) message;
    iov.iov_len = VHOST_USER_HDR_SIZE;

    ret = recvmsg(fd, &msgh, 0);

    if ((ret > 0 && msgh.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) || ret < 0 ) {
       return E_CLIENT_ERR_IOCTL_REPLY;
    }

    cmsgh = CMSG_FIRSTHDR(&msgh);
    if (cmsgh && cmsgh->cmsg_len > 0 && cmsgh->cmsg_level == SOL_SOCKET &&
            cmsgh->cmsg_type == SCM_RIGHTS) {

        client_vhost_ioctl_recv_fds_handler(cmsgh, l_fds, l_fd_num);

    }

    read(fd, ((char*)msg) + ret, message->size);
    return E_CLIENT_OK;
}

int
client_vhost_ioctl_recv_fds_handler(struct cmsghdr *cmsgh, int *fds, size_t *fd_num) {

    struct cmsghdr *const l_cmsgh = cmsgh;
    int *const l_fds = fds;
    size_t *const l_fd_num = fd_num;
    size_t fd_size = 0;

    if (!cmsgh || !fds || !fd_num) {
        return E_CLIENT_ERR_FARG;
    }

    if (*fd_num * sizeof(int) >= l_cmsgh->cmsg_len - CMSG_LEN(0)) {
       fd_size = l_cmsgh->cmsg_len - CMSG_LEN(0);
       *l_fd_num = fd_size / sizeof(int);
       memcpy(l_fds, CMSG_DATA(l_cmsgh), fd_size);
    }

    return E_CLIENT_OK;
}




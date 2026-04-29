#ifndef FD_POLL_H_
#define FD_POLL_H_

typedef struct fd_poller fd_poller;
fd_poller *fd_poller_new(int sdl_event_type);
void fd_poller_free(fd_poller *poller);
void fd_poller_watch(fd_poller *poller, int fd);

#endif // FD_POLL_H_

/*
 * MIT License
 *
 * Copyright (c) 2023 Frédéric Bour <frederic.bour@lakaban.net>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */


#include <errno.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "fd_poll.h"

#define MAX_WATCHED_FDS 8

// Messages sent on the control pipe: a fd to watch, or QUIT
#define QUIT (-1)

struct txp_fd_poller_s
{
  SDL_Thread *thread;
  int pipe[2];
  Uint32 event_type;
  // Owned by the thread
  int fds[MAX_WATCHED_FDS];
  int fd_count;
};

static void push_event(txp_fd_poller *poller, int fd)
{
  SDL_Event event;
  SDL_zero(event);
  event.type = poller->event_type;
  event.user.code = fd;
  if (SDL_PushEvent(&event) < 0)
  {
    fprintf(stderr, "[fd_poll] cannot push event: %s\n", SDL_GetError());
    abort();
  }
}

static void add_fd(txp_fd_poller *poller, int fd)
{
  for (int i = 0; i < poller->fd_count; i++)
    if (poller->fds[i] == fd)
      return;
  if (poller->fd_count == MAX_WATCHED_FDS)
  {
    fprintf(stderr, "[fd_poll] too many watched fds, ignoring %d\n", fd);
    return;
  }
  poller->fds[poller->fd_count++] = fd;
}

// Read a control message, false when the thread should stop
static bool read_control(txp_fd_poller *poller)
{
  int msg;
  ssize_t n;
  do n = read(poller->pipe[0], &msg, sizeof(msg));
  while (n == -1 && errno == EINTR);

  if (n != sizeof(msg))
  {
    if (n == -1)
      perror("[fd_poll] read");
    else if (n > 0)
      fprintf(stderr, "[fd_poll] partial read on control pipe\n");
    return false;
  }
  if (msg == QUIT)
    return false;
  add_fd(poller, msg);
  return true;
}

static int SDLCALL poll_thread_main(void *data)
{
  txp_fd_poller *poller = data;

  while (1)
  {
    // The control pipe comes first, then the watched fds
    struct pollfd fds[MAX_WATCHED_FDS + 1];
    int count = poller->fd_count;
    fds[0] = (struct pollfd){.fd = poller->pipe[0], .events = POLLRDNORM};
    for (int i = 0; i < count; i++)
      fds[i + 1] = (struct pollfd){.fd = poller->fds[i], .events = POLLRDNORM};

    if (poll(fds, count + 1, -1) == -1)
    {
      if (errno == EINTR)
        continue;
      perror("[fd_poll] poll");
      return 1;
    }

    // Notify and forget the ready fds. A closed pipe reports POLLHUP without
    // POLLRDNORM (Linux), and a closed fd POLLNVAL: these are ready too
    // (the reader will see the end of file or the error), otherwise the
    // fd would stay watched and poll would return immediately forever.
    poller->fd_count = 0;
    for (int i = 1; i <= count; i++)
    {
      if (fds[i].revents & (POLLRDNORM | POLLHUP | POLLERR | POLLNVAL))
        push_event(poller, fds[i].fd);
      else
        poller->fds[poller->fd_count++] = fds[i].fd;
    }

    if ((fds[0].revents & (POLLRDNORM | POLLHUP | POLLERR | POLLNVAL)) &&
        !read_control(poller))
      return 0;
  }
}

static void write_control(txp_fd_poller *poller, int msg)
{
  while (1)
  {
    ssize_t n = write(poller->pipe[1], &msg, sizeof(msg));
    if (n == sizeof(msg))
      return;
    if (n == -1 && errno == EINTR)
      continue;
    perror("[fd_poll] write");
    abort();
  }
}

txp_fd_poller *txp_fd_poller_new(Uint32 event_type)
{
  txp_fd_poller *poller = calloc(1, sizeof(txp_fd_poller));
  if (!poller)
    return NULL;
  poller->event_type = event_type;

  if (pipe(poller->pipe) == -1)
  {
    perror("[fd_poll] pipe");
    free(poller);
    return NULL;
  }

  poller->thread = SDL_CreateThread(poll_thread_main, "fd_poller", poller);
  if (!poller->thread)
  {
    fprintf(stderr, "[fd_poll] cannot create thread: %s\n", SDL_GetError());
    close(poller->pipe[0]);
    close(poller->pipe[1]);
    free(poller);
    return NULL;
  }

  return poller;
}

void txp_fd_poller_watch(txp_fd_poller *poller, int fd)
{
  if (poller && fd >= 0)
    write_control(poller, fd);
}

void txp_fd_poller_free(txp_fd_poller *poller)
{
  if (!poller)
    return;
  write_control(poller, QUIT);
  SDL_WaitThread(poller->thread, NULL);
  close(poller->pipe[0]);
  close(poller->pipe[1]);
  free(poller);
}

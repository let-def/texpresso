#include "fd_poll.h"

#include <SDL2/SDL.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/poll.h>
#include <unistd.h>

/* Configuration */
#define MAX_WATCHED_FDS 8

#define DEBUG 0

#define dprintf(...)                \
  do                                \
  {                                 \
    if (DEBUG)                      \
      fprintf(stderr, __VA_ARGS__); \
  } while (0)

/* Internal State Structure */
struct fd_poller
{
  SDL_Thread *thread;
  int pipes[2];
  char _pad[64];  // Avoid false sharing
  int fds[MAX_WATCHED_FDS];
  int fd_count;
  int event_type;
};

/* Wo#include <sys/poll.h>
rker Thread Main Function */
static int SDLCALL poll_thread_main(void *data)
{
  struct fd_poller *poller = (struct fd_poller *)data;
  int control_fd = poller->pipes[0];
  int int_buf;
  ssize_t n;
  bool is_running = true;

  while (is_running)
  {
    /*
       1. Prepare Poll Array
       Index 0: Control Pipe
       Index 1..N: Watched FDs
    */
    struct pollfd fds[MAX_WATCHED_FDS + 1];

    int fd_count = poller->fd_count;

    /* Add Control Pipe */
    fds[0].fd = control_fd;
    fds[0].events = POLLRDNORM;
    fds[0].revents = 0;

    /* Add Watched FDs */
    dprintf("FD_POLL: poll(");
    for (int i = 0; i < poller->fd_count; i++)
    {
      dprintf("%s%d", i > 0 ? ", " : "", poller->fds[i]);
      fds[i + 1].fd = poller->fds[i];
      fds[i + 1].events = POLLRDNORM;
      fds[i + 1].revents = 0;
    }
    dprintf(")\n");

    /*
       2. Poll
       We use -1 timeout (infinite).
    */
    int ret = poll(fds, fd_count + 1, -1);
    if (ret == -1)
    {
      if (errno == EINTR)
        continue;
      perror("poll");
      goto cleanup;
    }

    /*
       3. Process Watched FDs (Indices 1 to total_fds)
       Emit events and remove FDs from the list in one go.
    */
    poller->fd_count = 0;
    for (int i = 1; i <= fd_count; i++)
    {
      if (fds[i].revents & POLLRDNORM)
      {
        /* Generate SDL Event */
        SDL_Event event;
        SDL_zero(event);
        event.type = poller->event_type;
        event.user.code = fds[i].fd; /* Store FD in code */
        event.user.data1 = NULL;
        event.user.data2 = NULL;

        dprintf("FD_POLL: fd %d is ready\n", int_buf);

        if (SDL_PushEvent(&event) < 0)
        {
          fprintf(stderr, "Error: Failed to push SDL event\n");
          abort();
        }
      }
      else
      {
        /* Add FD back to list */
        poller->fds[poller->fd_count++] = fds[i].fd;
      }
    }

    /*
       4. Process Control FD (Index 0)
       Read one integer, act on it, and loop again.
    */
    if (fds[0].revents & POLLRDNORM)
    {
      n = read(control_fd, &int_buf, sizeof(int));
      if (n == -1)
      {
        if (errno == EINTR)
          continue;
        /* Pipe error or closed */
        goto cleanup;
      }
      if (n == 0)
      {
        /* Pipe closed */
        goto cleanup;
      }
      if (n != sizeof(int))
      {
        fprintf(stderr, "Error: Partial read on control pipe\n");
        goto cleanup;
      }

      if (int_buf < 0)
      {
        /* Quit signal */
        is_running = false;
      }
      else
      {
        /* Add FD to watch list */
        for (int i = 0; i < poller->fd_count; i++)
        {
          if (poller->fds[i] == int_buf)
          {
            int_buf = -1;
            break;
          }
        }

        if (int_buf == -1)
        {
          dprintf("FD_POLL: ignoring %d, already watching\n", int_buf);
        }
        else if (poller->fd_count < MAX_WATCHED_FDS)
        {
          poller->fds[poller->fd_count++] = int_buf;
          dprintf("FD_POLL: watching fd %d\n", int_buf);
        }
        else
        {
          fprintf(stderr,
                  "Warning: Max FD limit (%d) reached, ignoring FD %d\n",
                  MAX_WATCHED_FDS, int_buf);
        }
      }
    }
    else
    {
      dprintf("FD_POLL: nothing on control fd\n");
    }

    /* If we processed control messages or events, we loop back to
       rebuild the pollfd array and poll again. */
  }

cleanup:
  return is_running ? 1 : 0;
}

/* Public API */

/*
   Creates a new poller instance.
   event_type: The SDL_Event type to use when an FD becomes readable.
*/
fd_poller *fd_poller_new(int event_type)
{
  struct fd_poller *poller =
      (struct fd_poller *)calloc(1, sizeof(struct fd_poller));
  if (!poller)
    return NULL;

  poller->event_type = event_type;
  poller->fd_count = 0;

  if (pipe(poller->pipes) == -1)
  {
    free(poller);
    return NULL;
  }

  poller->thread =
      SDL_CreateThread(poll_thread_main, "fd_poller_thread", poller);

  if (!poller->thread)
  {
    close(poller->pipes[0]);
    close(poller->pipes[1]);
    free(poller);
    return NULL;
  }

  return poller;
}

static void write_int(fd_poller *poller, int msg, const char *caller)
{
  while (1)
  {
    ssize_t n = write(poller->pipes[1], &msg, sizeof(int));
    if (n == sizeof(int))
      break;
    if (n == -1 && errno == EINTR)
      continue;
    perror(caller);
    abort();
  }
}

/*
   Adds an FD to the watch list.
   Sends an integer to the thread via the pipe.
*/
void fd_poller_watch(fd_poller *poller, int fd)
{
  if (!poller || fd < 0)
    return;

  dprintf("FD_POLL: watch %d\n", fd);
  write_int(poller, fd, __func__);
}

/*
   Frees the poller and stops the thread.
*/
void fd_poller_free(fd_poller *poller)
{
  if (!poller)
    return;

  /* Signal quit */
  write_int(poller, -1, __func__);

  /* Wait for thread to finish */
  SDL_WaitThread(poller->thread, NULL);

  /* Cleanup resources */
  close(poller->pipes[0]);
  close(poller->pipes[1]);
  free(poller);
}

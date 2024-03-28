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

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>
#include <unistd.h>

#ifdef __APPLE__
# include <sys/syslimits.h>
#else
# include <linux/limits.h>
#endif

static int usage(void)
{
  fprintf(stderr, "Usage:\n  proxy [directory with texpresso.{stdin,stdout,stderr}]\n");
  return 1;
}

static int transfer(int fd_src, int fd_dst)
{
  char buffer[1024];
  int n;

  do
  {
    n = read(fd_src, buffer, 1024);
    if (n == -1)
    {
      if (errno == EINTR)
        continue;
      perror("transfer read");
      return 0;
    }
  } while (0);

  char *ptr = buffer;
  while (n > 0)
  {
    int m = write(fd_dst, ptr, n);
    if (m == -1)
    {
      if (errno == EINTR)
        continue;
      perror("transfer write");
      return 0;
    }
    n -= m;
    ptr += m;
  }
  return 1;
}

int main(int argc, char **argv)
{
  char
    path_stdin[PATH_MAX],
    path_stdout[PATH_MAX],
    path_stderr[PATH_MAX];

  const char *tmpdir = getenv("TMPDIR");
  stpcpy(stpcpy(path_stdin, tmpdir), "/texpresso.stdin");
  stpcpy(stpcpy(path_stdout, tmpdir), "/texpresso.stdout");
  stpcpy(stpcpy(path_stderr, tmpdir), "/texpresso.stderr");

  int fd_stdin = open(path_stdin, O_WRONLY, 0);
  if (fd_stdin == -1)
  {
    perror(path_stdin);
    return 1;
  }
  int fd_stdout = open(path_stdout, O_RDONLY, 0);
  if (fd_stdout == -1)
  {
    perror(path_stdout);
    return 1;
  }
  int fd_stderr = open(path_stderr, O_RDONLY, 0);
  if (fd_stderr == -1)
  {
    perror(path_stderr);
    return 1;
  }

  while (1)
  {
    struct pollfd fds[3];
    fds[0].fd = 0;
    fds[0].events = POLLIN;
    fds[0].revents = 0;
    fds[1].fd = fd_stdout;
    fds[1].events = POLLIN;
    fds[1].revents = 0;
    fds[2].fd = fd_stderr;
    fds[2].events = POLLIN;
    fds[2].revents = 0;

    int n = poll(fds, 3, -1);
    if (n == -1)
    {
      if (errno == EINTR) continue;
      perror("proxy poll");
      break;
    }

    if (fds[0].revents & POLLNVAL ||
        fds[1].revents & POLLNVAL ||
        fds[2].revents & POLLNVAL)
    {
      fprintf(stderr, "proxy: stream closed\n");
      break;
    }

    if (fds[0].revents & POLLIN)
      if (!transfer(fds[0].fd, fd_stdin))
        return 1;

    if (fds[1].revents & POLLIN)
      if (!transfer(fds[1].fd, STDOUT_FILENO))
        return 1;

    if (fds[2].revents & POLLIN)
      if (!transfer(fds[2].fd, STDERR_FILENO))
        return 1;
  }

  if (close(fd_stdin) == -1)
    perror("close stdin");
  if (close(fd_stdout) == -1)
    perror("close stdout");
  if (close(fd_stderr) == -1)
    perror("close stderr");

  return 0;
}

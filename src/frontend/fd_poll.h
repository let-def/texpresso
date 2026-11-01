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

#ifndef FD_POLL_H_
#define FD_POLL_H_

#include <SDL2/SDL.h>

// Watch file descriptors from a background thread, turning readiness into
// SDL events so that the main loop only has to wait on SDL_WaitEvent.
//
// A watched fd generates a single event (type event_type, user.code = fd)
// when it becomes readable, hangs up or fails, and is then forgotten: the
// main loop reads what is available and watches the fd again to be notified
// of more input. An fd that reached end of file should not be watched again.

typedef struct txp_fd_poller_s txp_fd_poller;

txp_fd_poller *txp_fd_poller_new(Uint32 event_type);

// Stop the thread and release the poller (NULL is accepted)
void txp_fd_poller_free(txp_fd_poller *poller);

// Generate an event when fd becomes ready (no-op if it is already watched)
void txp_fd_poller_watch(txp_fd_poller *poller, int fd);

#endif // FD_POLL_H_

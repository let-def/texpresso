/*
 * Preserved fence / rollback logic from the retired fork engine (engine_tex.c).
 *
 * This file is NOT compiled. It is kept verbatim as the reference algorithm for
 * porting the fork engine's fine-grained incremental replay into the in-process
 * wasm TeX engine (engine_tex.c, wasm-backed). The wasm engine currently uses a
 * simpler uniform-stride fence placement; this is the exponential-density,
 * exact-offset, multi-file version to port to.
 *
 * Primitive mapping (fork -> wasm), so only the checkpoint primitive changes:
 *   process_t (a forked child + its VFS log mark)   -> a COW fence layer
 *                                                       (wasm_engine_snapshot_push
 *                                                        + wasm_fence.mark)
 *   fork() / A_FORK reply / Q_CHLD                   -> wasm_engine_snapshot_push()
 *   pop_process() (kill the child)                   -> wasm_engine_restore_to()
 *   Q_SEEN report of the consumed position           -> read-offset tracked in
 *                                                       wio_read (see Option A:
 *                                                       short reads make the read
 *                                                       offset == consumption)
 *   Q_READ clip at fences[fence_pos].position        -> clip the return of
 *                                                       wio_read at the fence
 *   the trace (fed by Q_SEEN)                        -> a trace fed by wio_read
 *   compute_fences / possible_fence / revert_trace   -> ported as-is (below)
 *
 * The trace records, per read, the file entry + consumed position + time. On an
 * edit compute_fences() walks it backward from the edit with exponentially
 * growing gaps (dense near the edit, sparse far back). rollback discards forks
 * past the edit, reverts each reverted trace entry's `seen`, and rebuilds the
 * incremental DVI/synctex from the truncated output buffer.
 */

#if 0 /* reference only — not built */

typedef struct { fileentry_t *entry; int position; } fence_t;
typedef struct { fileentry_t *entry; int seen, time; } trace_entry_t;
typedef struct { int pid, fd; int trace_len; mark_t snap; } process_t;
/* engine holds: trace_entry_t *trace; int trace_cap; fence_t fences[16];
 *               int fence_pos; process_t processes[MAX_PROCESS]; int process_count; */

/* ---- trace: record how far a file has been consumed (fed by Q_SEEN) ---- */
static void record_seen(struct tex_engine *self, fileentry_t *entry, int seen, int time)
{
  process_t *p = get_process(self);

  if (p->trace_len > 0 && self->trace[p->trace_len-1].entry == entry &&
      (self->process_count <= 1 ||
      self->processes[self->process_count - 2].trace_len != p->trace_len))
  {
    self->trace[p->trace_len-1].time = time;
    entry->seen = seen;
    return;
  }

  if (p->trace_len == self->trace_cap)
  {
    int new_cap = self->trace_cap == 0 ? 8 : self->trace_cap * 2;
    trace_entry_t *newtr = calloc(sizeof(trace_entry_t), new_cap);
    if (newtr == NULL) abort();
    if (self->trace)
    {
      memcpy(newtr, self->trace, self->trace_cap * sizeof(trace_entry_t));
      free(self->trace);
    }
    self->trace = newtr;
    self->trace_cap = new_cap;
  }

  self->trace[p->trace_len] = (trace_entry_t){
    .entry = entry,
    .seen = entry->seen,
    .time = time,
  };
  entry->seen = seen;
  p->trace_len += 1;
}

static void revert_trace(trace_entry_t *te)
{
  te->entry->seen = te->seen;
}

/* ---- rollback: discard forks past the edit, revert the trace, resync DVI ---- */
static void rollback_processes(fz_context *ctx, struct tex_engine *self, int reverted, int trace)
{
  while (self->process_count > 0 && get_process(self)->trace_len > trace)
    pop_process(ctx, self);              /* -> wasm: restore_to(fence before edit) */

  int trace_len = self->process_count == 0 ? 0 : get_process(self)->trace_len;
  while (reverted > trace_len)
  {
    reverted--;
    revert_trace(&self->trace[reverted]);
  }

  if (self->st.document.entry)
    incdvi_update(ctx, self->dvi, self->st.document.entry->saved.data);
  else
    incdvi_reset(self->dvi);
  if (self->st.synctex.entry)
    synctex_update(ctx, self->stex, self->st.synctex.entry->saved.data);
  else
    synctex_rollback(ctx, self->stex, 0);
  editor_truncate(BUF_OUT, output_data(self->st.stdout.entry));
  editor_truncate(BUF_LOG, output_data(self->st.log.entry));
}

/* ---- fence placement ---- */
static bool possible_fence(trace_entry_t *te)
{
  if (te->seen == INT_MAX || te->seen == -1)
    return 0;
  if (te->entry->saved.level > FILE_READ)
    return 0;
  return 1;
}

static int compute_fences(fz_context *ctx, struct tex_engine *self, int trace, int offset)
{
  self->fence_pos = -1;

  if (trace <= 0)
    return trace;

  if (get_process(self)->trace_len <= trace)
    mabort();

  self->fence_pos = 0;

  offset = (offset - 64) & ~(64 - 1);
  if (offset < self->trace[trace].seen)
    offset = self->trace[trace].seen;
  if (offset == -1)
    offset = 0;

  self->fences[0].entry = self->trace[trace].entry;
  self->fences[0].position = offset;

  int delta = 50;
  int time = self->trace[trace].time - 10;

  int target_process = self->process_count - 1;
  while (target_process >= 0 && self->processes[target_process].trace_len > trace)
    target_process -= 1;
  int target_trace = target_process >= 0 ? self->processes[target_process].trace_len : -1;
  while (trace > target_trace && self->fence_pos < 15)
  {
    if (self->trace[trace].time <= time && possible_fence(&self->trace[trace]))
    {
      self->fence_pos += 1;
      self->fences[self->fence_pos].entry = self->trace[trace].entry;
      self->fences[self->fence_pos].position = self->trace[trace].seen;
      if (self->fences[self->fence_pos].position == -1)
        self->fences[self->fence_pos].position = 0;
      time -= delta;
      delta *= 2;
    }
    trace -= 1;
  }

  return trace;
}

/* ---- read clipping: fork exactly at the next fence position (Q_READ) ---- */
/*   if (fence_pos >= 0 && fences[fence_pos].entry == e &&
 *       fences[fence_pos].position < read.pos + n) {
 *     n = fences[fence_pos].position - read.pos;   // clip
 *     if (n == 0) { answer A_FORK; fence_pos--; }  // -> wasm: snapshot_push()
 *   }                                                                        */

#endif /* reference only */

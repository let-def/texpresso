#ifndef EDITOR_H_
#define EDITOR_H_

#include "driver.h"
#include "vstack.h"

void editor_set_protocol(enum editor_protocol protocol);

// Receiving commands

enum EDITOR_COMMAND
{
  EDIT_OPEN,
  EDIT_CLOSE,
  EDIT_CHANGE,
  EDIT_THEME,
  EDIT_PREVIOUS_PAGE,
  EDIT_NEXT_PAGE,
  EDIT_MOVE_WINDOW,
  EDIT_RESCAN,
  EDIT_STAY_ON_TOP,
  EDIT_SYNCTEX_FORWARD,
};

struct editor_command {
  enum EDITOR_COMMAND tag;
  union {
    struct {
      const char *path;
      const char *data;
      int length;
    } open;

    struct {
      const char *path;
    } close;

    struct {
      const char *path, *data;
      int offset, remove_length, insert_length;
    } change;

    struct {
      float bg[3], fg[3];
    } theme;

    struct {
    } previous_page;

    struct {
    } next_page;

    struct {
      float x, y, w, h;
    } move_window;

    struct {
    } rescan;

    struct {
      bool status;
    } stay_on_top;

    struct {
      const char *path;
      int line;
    } synctex_forward;
  };
};

bool editor_parse(fz_context *ctx,
                  vstack *stack,
                  val command,
                  struct editor_command *out);

// Sending message

enum EDITOR_INFO_BUFFER
{
  BUF_OUT, // TeX process stdout
  BUF_LOG, // TeX output log file
};

void editor_append(enum EDITOR_INFO_BUFFER name,
                   int pos,
                   const char *data,
                   int data_len);

void editor_truncate(enum EDITOR_INFO_BUFFER name, int length);
void editor_flush(void);
void editor_synctex(const char *path, int path_len, int line, int column);
void editor_reset_sync(void);

#endif  // EDITOR_H_

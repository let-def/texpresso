#ifndef VIEWER_H
#define VIEWER_H

#include <SDL2/SDL.h>
#include <mupdf/fitz.h>
#include <stdbool.h>
#include "pagecollection.h"

/**
 * @brief Range of pages and lines visible in the viewport.
 *
 * Used to determine which pages need to be rendered and cached.
 */
typedef struct {
  int first_page, last_page;  ///< Range of pages currently visible
  int first_line, last_line;  ///< First and last visible scanlines
} VisibleRange;

/**
 * @brief Scrollbar UI state.
 *
 * Manages the appearance and interaction with the vertical scrollbar thumb.
 * Alpha fades scrollbar in/out based on user interaction.
 */
typedef struct {
  bool dragging;            ///< Currently dragging the thumb
  uint8_t target_alpha;     ///< Target opacity (0-255)
  float drag_offset;        ///< Mouse click offset from thumb top
  float alpha;              ///< Current opacity (0-255)

  int x, y;                 ///< Scrollbar position (x, y of track)
  int w, h;                 ///< Scrollbar track width and height
  int thumb_h;              ///< Thumb height in pixels
  int thumb_y;              ///< Thumb top position in pixels
} ViewerScroll;

/**
 * @brief Viewer state and physics parameters.
 *
 * Manages zoom, panning, scrolling, and animation state.
 * Also handles scrollbar UI and viewport state tracking for efficient re-rendering.
 *
 * Physics model:
 * - Vertical position uses spring-damper system with border constraints
 * - Horizontal scrolling is independent (per-page) with friction
 * - Zoom changes smoothly via interpolation or instant
 * - Panning provides momentum-based scrolling
 */
typedef struct {
  // Zoom and position
  float zoom, target_zoom;          ///< Current and target zoom factor
  float offset_y, target_offset_y;  ///< Current and target vertical offset
  float offset_x, target_offset_x;  ///< Current and target horizontal offset
  float velocity_y;                 ///< Current vertical velocity

  // Interaction flags
  bool panning, panning_x, animating, crop;  ///< Interaction state

  // Physics parameters
  float friction;        ///< Velocity decay per frame
  float spring_k;        ///< Spring constant for centering
  float border_friction; ///< Friction when hitting document borders
  float zoom_speed;      ///< Animation speed for zoom changes

  // Layout parameters
  int margin;         ///< Page margin in world units
  int screen_margin;  ///< Margin from screen edge

  // Viewport
  int win_w, win_h;   ///< Window dimensions in pixels
  float ez;           ///< Effective zoom = zoom * (win_w - 2*margin) / ref_width

  // UI
  ViewerScroll scroll;    ///< Scrollbar state
  VisibleRange vr;        ///< Current visible range

  // Render tracking (for frame skipping)
  struct {
    int win_w, win_h;
    VisibleRange vr;
    ViewerScroll scroll;
    float offset_x;
  } last_render;
} Viewer;

/**
 * @brief Document coordinate result from hit testing.
 */
typedef struct {
  int page_index;  ///< Page index containing the point (-1 if none)
  float x, y;      ///< Local coordinates within the page
} DocCoord;

/**
 * @brief Initialize a Viewer structure.
 *
 * Sets default physics parameters and initial state:
 * - zoom = 1.0
 * - No panning or animating
 * - Default friction/spring constants
 * - Scrollbar hidden
 */
void viewer_init(Viewer *vwr);

/**
 * @brief Cleanup a Viewer structure.
 *
 * Current implementation does nothing (no dynamic allocation).
 * Reserved for future cleanup needs.
 */
void viewer_finalize(Viewer *vwr);

/**
 * @brief Update viewer state for one frame.
 *
 * Implements physics simulation:
 * - Spring-damper system for vertical position
 * - Border constraints (document boundaries)
 * - Momentum-based scrolling
 * - Smooth zoom interpolation
 *
 * @param vwr Viewer state to update.
 * @param pcoll Page collection (for document dimensions).
 * @param dt Time since last frame in seconds.
 * @param win_w, win_h Current window dimensions.
 */
void viewer_update(Viewer *vwr, PageCollection *pcoll, float dt, int win_w, int win_h);

/**
 * @brief Process SDL events for navigation.
 *
 * Handles:
 * - Scrollbar interaction (dragging, clicking)
 * - Panning with mouse
 * - Double-click zoom (toggle crop mode)
 * - Keyboard navigation (UP/DOWN)
 * - Mouse wheel scrolling and zoom (with SHIFT)
 *
 * @param ctx MuPDF context (for page queries during events).
 * @param vwr Viewer state to update.
 * @param pcoll Page collection (for coordinate calculations).
 * @param ev SDL event to process.
 * @param win SDL window (for mouse grab control).
 */
void viewer_handle_event(fz_context *ctx, Viewer *vwr, PageCollection *pcoll, SDL_Event *ev, SDL_Window *win);

/**
 * @brief Check if viewer is idle (no animation or movement).
 *
 * Used to optimize event waiting - can block until next event
 * when nothing is animating or moving.
 *
 * @param vwr Viewer state to check.
 * @return true if all animations complete and scrollbar settled.
 */
bool viewer_is_idle(Viewer *vwr);

/**
 * @brief Determine if a full re-render is needed.
 *
 * Compares current state to last rendered state to detect:
 * - Viewport changes
 * - Scrollbar state changes
 * - Page range changes
 * - Offset changes
 *
 * @param ctx MuPDF context (for visible range calculation).
 * @param vwr Viewer state to check.
 * @param pcoll Page collection.
 * @return true if re-render needed.
 */
bool viewer_need_rerender(fz_context *ctx, Viewer *vwr, PageCollection *pcoll);

/**
 * @brief Get effective scale factor for rendering.
 *
 * This is the factor used to map document units to window pixel units.
 *
 * @param vwr Viewer state.
 * @return Scale factor in pixels per world unit.
 */
float viewer_get_scale_factor(Viewer *vwr);

/**
 * @brief Get the range of pages currently visible in the viewport.
 *
 * @param ctx MuPDF context (for page queries).
 * @param vwr Viewer state.
 * @param pcoll Page collection.
 * @return VisibleRange struct with page and line range.
 */
VisibleRange viewer_get_visible_range(fz_context *ctx, Viewer *vwr, PageCollection *pcoll);

/**
 * @brief Get screen rectangle for a specific page.
 *
 * @param ctx MuPDF context (for page bounds).
 * @param vwr Viewer state.
 * @param pcoll Page collection.
 * @param i Page index.
 * @return Screen-space rectangle for the page.
 */
fz_irect viewer_get_page_screen_rect(fz_context *ctx, Viewer *vwr, PageCollection *pcoll, int i);

/**
 * @brief Get buffer-space rectangle for a specific page.
 *
 * This is the absolute position where the page should be cached
 * in the texture buffer (before texture wrapping).
 *
 * @param ctx MuPDF context (for page bounds).
 * @param vwr Viewer state.
 * @param pcoll Page collection.
 * @param page_idx Page index.
 * @return Buffer-space rectangle for the page.
 */
fz_irect viewer_get_page_buffer_rect(fz_context *ctx, Viewer *vwr, PageCollection *pcoll, int page_idx);

/**
 * @brief Convert screen coordinates to document coordinates.
 *
 * Used for hit testing (clicks, double-clicks, etc.).
 * Returns page index and local coordinates within that page.
 *
 * @param ctx MuPDF context (for page bounds).
 * @param vwr Viewer state.
 * @param pcoll Page collection.
 * @param sx, sy Screen coordinates.
 * @return DocCoord with page index and local position.
 */
DocCoord viewer_screen_to_doc(fz_context *ctx, Viewer *vwr, PageCollection *pcoll, int sx, int sy);

/**
 * @brief Draw scrollbar UI.
 *
 * @param vwr Viewer state (contains scrollbar data).
 * @param ren SDL renderer for drawing.
 */
void viewer_draw_scrollbar(Viewer *vwr, SDL_Renderer *ren);

/**
 * @brief Clip absolute rectangle to canvas and convert to local coordinates.
 *
 * @param absolute Rectangle in absolute coordinates.
 * @param canvas Reference rectangle to clip against.
 * @return Relative rectangle within canvas.
 */
fz_irect fz_relative_clipped_area(fz_irect absolute, fz_irect canvas);

/**
 * @brief Scroll to a document coordinate, with optional horizontal/vertical hysteresis.
 *
 * @param ctx MuPDF context (for page metrics).
 * @param vwr Viewer state to update.
 * @param pcoll Page collection (for page dimensions).
 * @param coord Target DocCoord.
 * @param center_tolerance [0, 0.5] Fraction of viewport *height* to consider "centered" vertically.
 * @param h_center_tolerance [0, 0.5] Fraction of viewport *width* to consider "centered" horizontally.
 *        (Set to 0.0 to disable horizontal hysteresis.)
 * @note Sets `vwr->animating = true` if *either* axis scrolls.
 */
void viewer_scroll_to_doc_coord(fz_context *ctx,
                                Viewer *vwr,
                                PageCollection *pcoll,
                                DocCoord coord,
                                float center_tolerance,
                                float h_center_tolerance);

#endif

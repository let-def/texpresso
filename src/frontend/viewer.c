#include "viewer.h"
#include "pagecollection.h"

#include <SDL2/SDL.h>
#include <stdlib.h>

/**
 * @brief Calculate maximum horizontal scroll range for apage.
 *
 * When a page is narrower than the window, it can be centered horizontally.
 * This function computes how far left/right the page can be scrolled.
 *
 * Formula:
 * - If page fits in window: scroll limit = 0 (fixed)
 * - If page exceeds window: limit = (page_width - win_width) / (2 * ez) + screen_margin / ez
 *
 * @param page_w Page width in pixels (at 100% zoom).
 * @param win_w Window width in pixels.
 * @param ez Effective zoom (screen pixels per world unit).
 * @param screen_margin Margin from window edge.
 * @return Maximum horizontal offset (positive right, negative left).
 */
static float get_page_scroll_limit(float page_w,
                                   int win_w,
                                   float ez,
                                   int screen_margin)
{
  float vis_world_w = (float)win_w / ez;
  if (page_w <= vis_world_w)
    return 0.0f;
  return (page_w - vis_world_w) / 2.0f + ((float)screen_margin / ez);
}

/**
 * @brief Calculate screen X position for a page's left edge.
 *
 * Converts world coordinates to screen coordinates with optional horizontal
 * centering and clipping.
 *
 * Algorithm:
 * 1. Compute world X where page is centered in window
 * 2. Clamp offset_x to stay within scroll limits
 * 3. Convert to screen coordinates
 *
 * @param page_w Page width in pixels (at 100% zoom).
 * @param win_w Window width in pixels.
 * @param offset_x Current horizontal scroll offset (world units).
 * @param ez Effective zoom.
 * @param screen_margin Margin from window edge.
 * @return Screen X coordinate of page's left edge.
 */
static float get_page_screen_x(int page_w, int win_w, float offset_x,
                               float ez, int screen_margin)
{
  float vis_world_w = (float)win_w / ez;
  float page_world_w = (float)page_w;
  float world_x_base = (vis_world_w / 2.0f) - (page_world_w / 2.0f);

  float limit = get_page_scroll_limit(page_w, win_w, ez, screen_margin);
  float clamped_offset = offset_x;
  if (clamped_offset < -limit) clamped_offset = -limit;
  if (clamped_offset > limit) clamped_offset = limit;

  return (world_x_base - clamped_offset) * ez;
}

/**
 * @brief Calculate scrollbar state for current viewport.
 *
 * Updates scrollbar thumb size and position based on:
 * - Total document height vs visible height
 * - Current vertical offset
 *
 * Thumb size = visible_height / total_height
 * Thumb position normalized from (offset - min_off) / range
 *
 * Also handles fade in/out interpolation:
 * - Direct mouse interaction: alpha = 255
 * - Hovering near scrollbar: alpha = 128
 * - Not hovering: alpha = 0 (fade out)
 */
static void calc_scrollbar(Viewer *vwr, PageCollection *pcoll, float dt)
{
  float total_h = pagecollection_total_height(pcoll, vwr->margin);
  float vis_h = (float)vwr->win_h / vwr->ez;
  ViewerScroll *sb = &vwr->scroll;

  // Track dimensions (right side of screen)
  sb->w = 18;
  sb->x = vwr->win_w - sb->w;
  sb->y = 0;
  sb->h = vwr->win_h;

  if (total_h <= vis_h)
  {
    // No scroll needed: document fits in viewport
    sb->thumb_h = 0;
    sb->thumb_y = 0;
    sb->target_alpha = 0;
  }
  else
  {
    // Calculate thumb height ratio
    float ratio = vis_h / total_h;
    if (ratio > 1.0f)
      ratio = 1.0f;

    sb->thumb_h = (int)(sb->h * ratio);
    if (sb->thumb_h < 20)
      sb->thumb_h = 20; // Min thumb size

    // Calculate thumb position
    // min_off is negative (scroll above top), max_off is positive (scroll below bottom)
    float min_off = -(float)vwr->screen_margin / vwr->ez;
    float max_off = total_h - vis_h + ((float)vwr->screen_margin / vwr->ez);

    // Normalize offset to [0, 1]
    float range = max_off - min_off;
    float norm_y = 0.0f;
    if (range > 0)
      norm_y = (vwr->offset_y - min_off) / range;

    // Clamp
    if (norm_y < 0)
      norm_y = 0;
    if (norm_y > 1)
      norm_y = 1;

    // Map to track height (minus thumb height)
    float track_avail = sb->h - sb->thumb_h;
    sb->thumb_y = (int)(sb->y + norm_y * track_avail);
  }

  // Fade interpolation: smooth alpha transitions
  if (sb->alpha != sb->target_alpha)
  {
    float step = dt / 0.2 * 256;  // Time constant: 0.2 seconds

    // Simple lerp
    if (sb->alpha < sb->target_alpha)
    {
      sb->alpha += step;
      if (sb->alpha > sb->target_alpha)
        sb->alpha = sb->target_alpha;
    }
    else
    {
      sb->alpha -= step;
      if (sb->alpha < sb->target_alpha)
        sb->alpha = sb->target_alpha;
    }
  }
}

/**
 * @brief Update scrollbar visibility based on mouse position.
 *
 * Determines if mouse is hovering over or dragging the scrollbar,
 * and sets target alpha accordingly.
 *
 * Hit detection expands scrollbar hit area by 10px for easier interaction.
 */
static void hover_scrollbar(ViewerScroll *sb, int mouse_x, int mouse_y)
{
  int hit_w = sb->w + 20;
  int hit_x = sb->x - 10;
  bool hovering = (mouse_x >= hit_x && mouse_x <= hit_x + hit_w &&
                   mouse_y >= sb->y && mouse_y <= sb->y + sb->h);
  if (hovering || sb->dragging)
    sb->target_alpha = 255; // Fully visible
  else if (mouse_x < hit_x - 100)
    sb->target_alpha = 0;   // Not hovering at all
  else
    sb->target_alpha = 128; // Faintly visible (hovered recently)
}

/**
 * @brief Draw scrollbar to renderer.
 *
 * Draws track (background) and thumb (handle) with current alpha.
 * Thumb colors: dragging = 200, idle = 180 (grayscale).
 */
static void draw_scrollbar(SDL_Renderer *ren, ViewerScroll *sb)
{
  if (sb->thumb_h <= 0)
    return;

  SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);

  // Draw Track
  SDL_SetRenderDrawColor(ren, 50, 50, 50, sb->alpha);
  SDL_Rect track = {sb->x, sb->y, sb->w, sb->h};
  SDL_RenderFillRect(ren, &track);

  // Draw Thumb
  int thumb_col = sb->dragging ? 200 : 180;
  SDL_SetRenderDrawColor(ren, thumb_col, thumb_col, thumb_col, sb->alpha);
  SDL_Rect thumb = {sb->x, sb->thumb_y, sb->w, sb->thumb_h};
  SDL_RenderFillRect(ren, &thumb);

  SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_NONE);
}

/**
 * @brief Check if point is inside scrollbar area (with expanded hit box).
 */
static bool is_in_scrollbar(ViewerScroll *sb, int x, int y) {
    return (x >= sb->x - 10 && x < sb->x + sb->w + 10 &&
            y >= sb->y - 10 && y < sb->y + sb->h + 10);
}

/**
 * @brief Check if point is inside scrollbar thumb.
 */
static bool is_in_thumb(ViewerScroll *sb, int x, int y) {
    return (x >= sb->x && x < sb->x + sb->w &&
            y >= sb->thumb_y && y < sb->thumb_y + sb->thumb_h);
}

/**
 * @brief Draw scrollbar to renderer.
 *
 * Exposed API function that wraps internal draw_scrollbar.
 */
void viewer_draw_scrollbar(Viewer *vwr, SDL_Renderer *ren)
{
  draw_scrollbar(ren, &vwr->scroll);
}

/**
 * @brief Initialize Viewer with default physics parameters.
 */
void viewer_init(Viewer *vwr)
{
  vwr->zoom = vwr->target_zoom = 1.0f;
  vwr->offset_y = vwr->target_offset_y = 0.0f;
  vwr->offset_x = vwr->target_offset_x = 0.0f;
  vwr->velocity_y = 0.0f;
  vwr->panning = vwr->panning_x = vwr->animating = vwr->crop = false;
  vwr->click_pending = false;
  vwr->click_timestamp = 0;
  vwr->click_x = 0;
  vwr->click_y = 0;
  vwr->friction = 0.95f;        // Velocity decay per frame
  vwr->spring_k = 0.15f;        // Spring constant for centering
  vwr->border_friction = 0.20f; // Friction at document boundaries
  vwr->zoom_speed = 0.20f;      // Zoom interpolation speed
  vwr->setting_margin = vwr->margin = 10;
  vwr->screen_margin = 15;
  vwr->scroll = (ViewerScroll){0,};
  vwr->win_w = vwr->win_h = 1;
}

/**
 * @brief Cleanup Viewer (no-op, reserved for future use).
 */
void viewer_finalize(Viewer *vwr)
{
  (void)vwr;
}

/**
 * @brief Recompute effective zoom factor.
 *
 * ez = zoom * (window_width - 2 * screen_margin) / reference_width
 *
 * This is the scaling factor that converts world units to screen pixels,
 * accounting for zoom level and window sizing.
 */
static void viewer_update_effective_zoom(Viewer *vwr, PageCollection *pcoll)
{
  vwr->ez = vwr->zoom * (float)(vwr->win_w - vwr->screen_margin * 2) /
          pagecollection_reference_width(pcoll);
  vwr->margin = fz_min(vwr->setting_margin, vwr->screen_margin / vwr->ez);
}

/**
 * @brief Update viewer state for one frame.
 *
 * Implements physics simulation for vertical scrolling:
 *
 * When animating (zoom change):
 * - Linear interpolation toward target values
 * - Stops when close enough (thresholds)
 *
 * When not animating:
 * - If panning: exponential velocity decay (friction)
 * - If not panning:
 *   - Document smaller than viewport: spring toward center
 *   - Document larger: velocity decay with friction
 * - Integration: offset += velocity
 * - Clamp to document boundaries with border_friction
 *
 * After physics calculation, scrollbar state is updated.
 *
 * @param vwr Viewer state to update.
 * @param pcoll Page collection (for document dimensions).
 * @param dt Time since last frame in seconds.
 * @param win_w, win_h Current window dimensions.
 */
void viewer_update(Viewer *vwr, PageCollection *pcoll, float dt, int win_w, int win_h)
{
  vwr->win_w = win_w;
  vwr->win_h = win_h;
  viewer_update_effective_zoom(vwr, pcoll);

  if (dt >= 0.0001f)
  {
    float vis_h = (float)win_h / vwr->ez;
    float min_off = -(float)vwr->screen_margin / vwr->ez;
    float total_h = pagecollection_total_height(pcoll, vwr->margin);
    float max_off = total_h - vis_h + ((float)vwr->screen_margin / vwr->ez);
    float ts = dt * 60.0f;  // Time step normalized to 60 FPS

    if (vwr->animating) {
      // Smooth interpolation toward target values
      float lerp = 1.0f - SDL_powf(1.0f - vwr->zoom_speed, ts);
      vwr->zoom += (vwr->target_zoom - vwr->zoom) * lerp;
      vwr->offset_y += (vwr->target_offset_y - vwr->offset_y) * lerp;
      vwr->offset_x += (vwr->target_offset_x - vwr->offset_x) * lerp;

      if (fabsf(vwr->zoom - vwr->target_zoom) < 0.001f &&
          fabsf(vwr->offset_y - vwr->target_offset_y) < 0.01f) {
        vwr->zoom = vwr->target_zoom;
        vwr->offset_y = vwr->target_offset_y;
        vwr->animating = false;
      }
    } else {
      // Physics-based scrolling
      if (vwr->panning) {
        // Momentum-based scrolling: velocity decays exponentially
        vwr->velocity_y *= SDL_powf(0.50f, ts);
      } else {
        //Spring-damper toward document center if small
        if (total_h < vis_h) {
          float target_center = (total_h - vis_h) / 2.0f;
          vwr->velocity_y += (target_center - vwr->offset_y) * vwr->spring_k * ts;
          vwr->velocity_y *= SDL_powf(vwr->border_friction, ts);
        } else {
          vwr->velocity_y *= SDL_powf(vwr->friction, ts);
        }
      }
      vwr->offset_y += vwr->velocity_y * ts;
    }

    // Clamp to document boundaries
    if (vwr->offset_y < min_off) {
      vwr->offset_y = min_off;
      vwr->velocity_y = 0;
    } else if (vwr->offset_y > max_off) {
      vwr->offset_y = max_off;
      vwr->velocity_y = 0;
    }

    // Stop tiny velocities (avoid micro-jitter)
    if (fabsf(vwr->velocity_y) < 0.01f)
      vwr->velocity_y = 0;

    viewer_update_effective_zoom(vwr, pcoll);
  }

  calc_scrollbar(vwr, pcoll, dt);
}

/**
 * @brief Process SDL events for navigation.
 *
 * Handles:
 * - Scrollbar interaction (track clicks, thumb dragging)
 * - Panning with mouse
 * - Double-click zoom (with crop toggle)
 * - Keyboard navigation (UP/DOWN)
 * - Mouse wheel scrolling and zoom (with SHIFT)
 */
DocCoord viewer_handle_event(fz_context *ctx, Viewer *vwr, PageCollection *pcoll, SDL_Event *ev, SDL_Window *win)
{
  DocCoord dc = {-1, 0, 0};
  if (ev->type == SDL_MOUSEBUTTONDOWN ||
      ev->type == SDL_MOUSEBUTTONUP)
    hover_scrollbar(&vwr->scroll, ev->button.x, ev->button.y);

  if (ev->type == SDL_MOUSEMOTION)
    hover_scrollbar(&vwr->scroll, ev->motion.x, ev->motion.y);

  // Mouse click on scrollbar or document
  if (ev->type == SDL_MOUSEBUTTONDOWN && ev->button.button == SDL_BUTTON_LEFT)
  {
    // Store click coordinates for double-click detection
    vwr->click_timestamp = ev->button.timestamp;
    vwr->click_x = ev->button.x;
    vwr->click_y = ev->button.y;
    vwr->click_pending = true;

    // Scrollbar interaction
    if (is_in_scrollbar(&vwr->scroll, ev->button.x, ev->button.y))
    {
      vwr->scroll.dragging = true;
      if (is_in_thumb(&vwr->scroll, ev->button.x, ev->button.y))
      {
        vwr->scroll.drag_offset = ev->button.y - vwr->scroll.thumb_y;
      } else
      {
        vwr->scroll.drag_offset = vwr->scroll.thumb_h / 2.0f;

        float min_off = -vwr->screen_margin / vwr->ez;
        float total_h = pagecollection_total_height(pcoll, vwr->margin);
        float vis_h = vwr->win_h / vwr->ez;
        float max_off = total_h - vis_h + vwr->screen_margin / vwr->ez;

        float range = max_off - min_off;
        if (range > 0)
        {
          float track_avail = (float)vwr->scroll.h - vwr->scroll.thumb_h;
          if (track_avail <= 0)
            track_avail = 1;

          float new_thumb_y = ev->motion.y - vwr->scroll.drag_offset;
          if (new_thumb_y < vwr->scroll.y)
            new_thumb_y = vwr->scroll.y;
          if (new_thumb_y > vwr->scroll.y + track_avail)
            new_thumb_y = vwr->scroll.y + track_avail;

          float norm_y = (new_thumb_y - vwr->scroll.y) / track_avail;
          vwr->target_offset_y = vwr->offset_y = min_off + norm_y * range;
          vwr->velocity_y = 0;
          vwr->panning = false;
        }
      }
    }

    // Mouse click on document
    DocCoord dc = viewer_screen_to_doc(ctx, vwr, pcoll, ev->button.x, ev->button.y);

    // --- RE-ANCHOR LOGIC ---
    // When clicking on a page, determine if horizontal panning is possible
    if (dc.page_index != -1)
    {
      fz_display_list *dl = pagecollection_get(pcoll, dc.page_index);
      fz_rect bounds = fz_bound_display_list(ctx, dl);
      float doc_w = bounds.x1 - bounds.x0;
      float limit = get_page_scroll_limit(doc_w, vwr->win_w, vwr->ez, vwr->screen_margin);

      vwr->panning_x = limit > 0.01f;

      if (vwr->panning_x)
      {
        // Clamp to horizontal scroll limits
        if (vwr->offset_x < -limit) vwr->offset_x = -limit;
        if (vwr->offset_x > limit) vwr->offset_x = limit;
      }
    }

    if (ev->button.clicks >= 2 && dc.page_index != -1)
    {
      // Double-click: toggle crop mode and zoom to page
      if (ev->button.clicks > 2)
        vwr->crop = !vwr->crop;
      vwr->animating = true;

      fz_display_list *dl = pagecollection_get(pcoll, dc.page_index);
      float doc_w;
      fz_rect bounds = fz_bound_display_list(ctx, dl);
      if (vwr->crop)
      {
        // Crop mode: zoom to content bounds only
        fz_rect cropped = fz_empty_rect;
        fz_device *dev = fz_new_bbox_device(ctx, &cropped);
        fz_run_display_list(ctx, dl, dev, fz_identity, bounds, NULL);
        fz_close_device(ctx, dev);
        fz_drop_device(ctx, dev);
        float mid = (bounds.x0 + bounds.x1) / 2;
        cropped = fz_intersect_rect(bounds, cropped);
        doc_w = fz_max(fz_abs(mid - cropped.x0), fz_abs(mid - cropped.x1)) * 2.0;
      }
      else
        doc_w = bounds.x1 - bounds.x0;

      float ref_width = pagecollection_reference_width(pcoll);
      vwr->target_zoom = ref_width / doc_w;

      float n_ez = vwr->target_zoom * ((float)(vwr->win_w - 20) / ref_width);
      float world_y = dc.y + pagecollection_page_offset(pcoll, dc.page_index, vwr->margin);
      vwr->target_offset_y = world_y - (vwr->win_h / 2.0f) / n_ez;
      vwr->target_offset_x = 0;
    } else
    {
   // Single click: start panning
    vwr->panning = true;
    vwr->animating = false;
    vwr->velocity_y = 0;
  }
  } else if (ev->type == SDL_MOUSEBUTTONUP)
  {
    vwr->scroll.dragging = false;
    vwr->panning = false;
    SDL_SetRelativeMouseMode(SDL_FALSE);

    // Check if this was a short-click (single click, not drag) to determine DocCoord
    if (vwr->click_pending && SDL_GetTicks() - vwr->click_timestamp < 100 &&
        ev->button.button == SDL_BUTTON_LEFT &&
        ev->button.clicks == 1 &&
        abs(ev->button.x - vwr->click_x) < 10 &&
        abs(ev->button.y - vwr->click_y) < 10)
      dc = viewer_screen_to_doc(ctx, vwr, pcoll, ev->button.x, ev->button.y);

    // Reset click tracking
    vwr->click_pending = false;

  } else if (ev->type == SDL_MOUSEMOTION)
  {
    // Reset click tracking if mouse moves significantly
    if (vwr->click_pending)
    {
      vwr->click_pending = false;
      vwr->click_timestamp = 0;
    }

    if (vwr->scroll.dragging)
    {
      // Thumb dragging: update position directly
      float min_off = -vwr->screen_margin / vwr->ez;
      float total_h = pagecollection_total_height(pcoll, vwr->margin);
      float vis_h = vwr->win_h / vwr->ez;
      float max_off = total_h - vis_h + vwr->screen_margin / vwr->ez;

      float range = max_off - min_off;
      if (range > 0)
      {
        float track_avail = (float)vwr->scroll.h - vwr->scroll.thumb_h;
        if (track_avail <= 0)
          track_avail = 1;

        float new_thumb_y = ev->motion.y - vwr->scroll.drag_offset;
        if (new_thumb_y < vwr->scroll.y)
          new_thumb_y = vwr->scroll.y;
        if (new_thumb_y > vwr->scroll.y + track_avail)
          new_thumb_y = vwr->scroll.y + track_avail;

        float norm_y = (new_thumb_y - vwr->scroll.y) / track_avail;
        vwr->target_offset_y = vwr->offset_y = min_off + norm_y * range;
        vwr->velocity_y = 0;
        vwr->panning = false;
      }
    } else if (vwr->panning) {
      // Mouse dragging: accumulate offset and velocity
      float dy = ev->motion.yrel / vwr->ez;
      float dx = ev->motion.xrel / vwr->ez;
      vwr->offset_y -= dy;
      if (vwr->panning_x) vwr->offset_x -= dx;
      vwr->velocity_y = (vwr->velocity_y * 0.6f) - (dy * 0.4f);

      // Check if pointer hit window edge: switch to relative mode
      if ((ev->motion.y + ev->motion.yrel <= 0 ||
           ev->motion.y + ev->motion.yrel >= vwr->win_h) ||
           (vwr->panning_x && (ev->motion.x + ev->motion.xrel <= 0 ||
                             ev->motion.x + ev->motion.xrel >= vwr->win_w)))
        SDL_SetRelativeMouseMode(SDL_TRUE);
    }
  } else if (ev->type == SDL_MOUSEWHEEL)
  {
    vwr->animating = false;
    vwr->panning = false;
    if (SDL_GetModState() & KMOD_SHIFT)
    {
      // SHIFT + wheel = zoom
      vwr->zoom += ev->wheel.preciseY * 0.1f;
      if (vwr->zoom < 0.1f) vwr->zoom = 0.1f;
      vwr->target_zoom = vwr->zoom;
    }
    else
    {
      // Normal wheel = vertical scrolling
      vwr->velocity_y -= (ev->wheel.preciseY * 10) / vwr->ez;
    }
  } else if (ev->type == SDL_KEYDOWN)
  {
    if (ev->key.keysym.sym == SDLK_UP)
    {
      // UP key: scroll up by one screenful
      float screenful = vwr->win_h / vwr->ez;
      DocCoord hit = viewer_screen_to_doc(ctx, vwr, pcoll, vwr->win_w / 2, vwr->win_h / 2);
      fz_display_list *dl = pagecollection_get(pcoll, hit.page_index);
      if (dl)
      {
        fz_rect bounds = fz_bound_display_list(ctx, dl);
        if (bounds.y1 - bounds.y0 < screenful)
        {
          // Page is smaller than screen: jump to previous page
          dl = pagecollection_get(pcoll, hit.page_index - 1);
          if (dl)
          {
            bounds = fz_bound_display_list(ctx, dl);
            vwr->animating = true;
            vwr->target_offset_y =
                pagecollection_page_offset(pcoll, hit.page_index - 1, vwr->margin) -
                (bounds.y1 - bounds.y0 + screenful) / 2;
          }
        }
        else
        {
          // Page is larger than screen: scroll within page
          float page_top = pagecollection_page_offset(pcoll, hit.page_index, vwr->margin);
          vwr->animating = true;

          if (page_top < vwr->offset_y - screenful)
            vwr->target_offset_y = vwr->offset_y - screenful + vwr->margin;
          else if (page_top < vwr->offset_y)
            vwr->target_offset_y = page_top - vwr->margin;
          else
            vwr->target_offset_y = page_top - screenful;
        }
      }
    }
    else if (ev->key.keysym.sym == SDLK_DOWN)
    {
      // DOWN key: scroll down by one screenful
      float screenful = vwr->win_h / vwr->ez;
      DocCoord hit = viewer_screen_to_doc(ctx, vwr, pcoll, vwr->win_w / 2, vwr->win_h / 2);
      fz_display_list *dl = pagecollection_get(pcoll, hit.page_index);
      if (dl)
      {
        fz_rect bounds = fz_bound_display_list(ctx, dl);
        if (bounds.y1 - bounds.y0 < screenful)
        {
          // Page is smaller than screen: jump to next page
          dl = pagecollection_get(pcoll, hit.page_index + 1);
          if (dl)
          {
            bounds = fz_bound_display_list(ctx, dl);
            vwr->animating = true;
            vwr->target_offset_y =
                pagecollection_page_offset(pcoll, hit.page_index + 1, vwr->margin) +
                (bounds.y1 - bounds.y0 - screenful) / 2 - vwr->margin;
          }
        }
        else
        {
          // Page is larger than screen: scroll within page
          float page_bot =
              pagecollection_page_offset(pcoll, hit.page_index, vwr->margin) +
              bounds.y1 - bounds.y0;
          vwr->animating = true;

          if (page_bot > vwr->offset_y + screenful * 2)
            vwr->target_offset_y = vwr->offset_y + screenful - vwr->margin;
          else if (page_bot > vwr->offset_y + screenful)
            vwr->target_offset_y = page_bot - screenful + vwr->margin;
          else
            vwr->target_offset_y = page_bot;
        }
      }
    }
  }
  return dc;
}

/**
 * @brief Check if viewer is idle (no animation or movement).
 *
 * Used to optimize event waiting - can block until next event
 * when nothing is animating or moving.
 *
 * @param vwr Viewer state to check.
 * @return true if all animations complete and scrollbar settled.
 */
bool viewer_is_idle(Viewer*vwr)
{
  return (!vwr->animating && vwr->velocity_y == 0.0f) &&
         ((uint8_t)vwr->scroll.alpha == vwr->scroll.target_alpha);
}

/**
 * @brief Determine if a full re-render is needed.
 *
 * Compares current state to last rendered state to detect:
 * - Viewport changes
 * - Scrollbar state changes
 * - Page range changes
 * - Offset changes (horizontal)
 *
 * If any change exceeds thresholds, returns true.
 *
 * @param ctx MuPDF context (for visible range calculation).
 * @param vwr Viewer state to check.
 * @param pcoll Page collection.
 * @return true if re-render needed.
 */
bool viewer_need_rerender(fz_context *ctx, Viewer*vwr, PageCollection *pcoll)
{
  VisibleRange vr = viewer_get_visible_range(ctx, vwr, pcoll);
  bool same =
      (vwr->last_render.vr.first_line == vr.first_line) &&
      (vwr->last_render.vr.first_page == vr.first_page) &&
      (vwr->last_render.vr.last_line == vr.last_line) &&
      (vwr->last_render.vr.last_page == vr.last_page) &&
      (vwr->last_render.scroll.x == vwr->scroll.x) &&
      (vwr->last_render.scroll.y == vwr->scroll.y) &&
      (vwr->last_render.scroll.w == vwr->scroll.w) &&
      (vwr->last_render.scroll.h == vwr->scroll.h) &&
      (vwr->last_render.scroll.thumb_y == vwr->scroll.thumb_y) &&
      (vwr->last_render.scroll.thumb_h == vwr->scroll.thumb_h) &&
      ((uint8_t)vwr->last_render.scroll.alpha == (uint8_t)vwr->scroll.alpha) &&
      (vwr->last_render.win_w == vwr->win_w) &&
      (vwr->last_render.win_h == vwr->win_h) &&
      (fz_abs(vwr->offset_x - vwr->last_render.offset_x) < 0.001f);

  // Update last render state
  vwr->last_render.vr = vr;
  vwr->last_render.scroll = vwr->scroll;
  vwr->last_render.win_w = vwr->win_w;
  vwr->last_render.win_h = vwr->win_h;
  vwr->last_render.offset_x = vwr->offset_x;

  return !same;
}

/**
 * @brief Get current effective scale factor.
 *
 * @param vwr Viewer state.
 * @return Scale factor in pixels per world unit.
 */
float viewer_get_scale_factor(Viewer*vwr)
{
  return vwr->ez;
}

/**
 * @brief Get the range of pages currently visible in the viewport.
 *
 * Computes:
 * - first_page: page containing top of viewport
 * - last_page: page containing bottom of viewport
 * - first_line, last_line: vertical range in buffer coordinates
 *
 * @param ctx MuPDF context (for page queries).
 * @param vwr Viewer state.
 * @param pcoll Page collection.
 * @return VisibleRange struct with page and line range.
 */
VisibleRange viewer_get_visible_range(fz_context *ctx, Viewer *vwr, PageCollection *pcoll)
{
  VisibleRange vr;
  vr.first_page = pagecollection_page_below(pcoll, vwr->offset_y, vwr->margin);
  vr.last_page = pagecollection_page_above(pcoll, vwr->offset_y + vwr->win_h / vwr->ez, vwr->margin);

  fz_irect window = fz_make_irect(0, 0, vwr->win_w, vwr->win_h);
  {
    fz_irect screen_rect =
      viewer_get_page_screen_rect(ctx, vwr, pcoll, vr.first_page);
    fz_irect visible_rect =
        fz_relative_clipped_area(screen_rect, window);
    fz_irect buffer_rect =
      viewer_get_page_buffer_rect(ctx, vwr, pcoll, vr.first_page);
    vr.first_line = buffer_rect.y0 + visible_rect.y0;
  }
  {
    fz_irect screen_rect =
      viewer_get_page_screen_rect(ctx, vwr, pcoll, vr.last_page);
    fz_irect visible_rect =
        fz_relative_clipped_area(screen_rect, window);
    fz_irect buffer_rect =
      viewer_get_page_buffer_rect(ctx, vwr, pcoll, vr.last_page);
    vr.last_line = buffer_rect.y0 + visible_rect.y1;
  }

  return vr;
}

/**
 * @brief Get screen rectangle for a specific page.
 *
 * Converts page position (in world coordinates) to screen Coordinates.
 * Accounts for:
 * - Current zoom (vwr->ez)
 * - Horizontal offset (vwr->offset_x)
 * - Page-specific horizontal centering
 *
 * @param ctx MuPDF context (for page bounds).
 * @param vwr Viewer state.
 * @param pcoll Page collection.
 * @param i Page index.
 * @return Screen-space rectangle for the page.
 */
fz_irect viewer_get_page_screen_rect(fz_context *ctx, Viewer *vwr, PageCollection *pcoll, int i)
{
  float w = 0, h = 0;
  fz_display_list *dl = pagecollection_get(pcoll, i);
  if (dl) {
    fz_rect bounds = fz_bound_display_list(ctx, dl);
    w = bounds.x1 - bounds.x0;
    h = bounds.y1 - bounds.y0;
  }
  fz_irect r;
  r.x0 = get_page_screen_x((int)w, vwr->win_w, vwr->offset_x, vwr->ez, vwr->screen_margin);
  r.y0 = (pagecollection_page_offset(pcoll, i, vwr->margin) - vwr->offset_y) * vwr->ez;
  r.x1 = r.x0 + w * vwr->ez;
  r.y1 = r.y0 + h * vwr->ez;
  return r;
}

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
fz_irect viewer_get_page_buffer_rect(fz_context *ctx, Viewer *vwr, PageCollection *pcoll, int i) {
  fz_irect r;
  float offset = pagecollection_page_offset(pcoll, i, 0);
  float w = 0, h = 0;
  fz_display_list *dl = pagecollection_get(pcoll, i);
  if (dl) {
    fz_rect bounds = fz_bound_display_list(ctx, dl);
    w = bounds.x1 - bounds.x0;
    h = bounds.y1 - bounds.y0;
  }
  r.x0 = 0;
  r.y0 = offset * vwr->ez;
  r.x1 = r.x0 + w * vwr->ez;
  r.y1 = r.y0 + h * vwr->ez;
  return r;
}

/**
 * @brief Convert screen coordinates to document coordinates.
 *
 * Used for hit testing (clicks, double-clicks, etc.).
 * Iterates through pages to find which one contains the point,
 * then computes local coordinates.
 *
 * @param ctx MuPDF context (for page bounds).
 * @param vwr Viewer state.
 * @param pcoll Page collection.
 * @param sx, sy Screen coordinates.
 * @return DocCoord with page index and local position (-1 if none).
 */
DocCoord viewer_screen_to_doc(fz_context *ctx, Viewer *vwr, PageCollection *pcoll, int sx, int sy) {
  DocCoord dc = {-1, 0, 0};
  float world_y = (sy / vwr->ez) + vwr->offset_y;
  ssize_t page = pagecollection_page_below(pcoll, world_y, vwr->margin);
  float top = pagecollection_page_offset(pcoll, page, vwr->margin);
  fz_display_list *dl = pagecollection_get(pcoll, page);
  if (dl)
  {
    fz_rect bounds = fz_bound_display_list(ctx, dl);
    float w = bounds.x1 - bounds.x0;
    float h = bounds.y1 - bounds.y0;
    if (world_y >= top && world_y <= top + h)
    {
      float page_x = get_page_screen_x(w, vwr->win_w, vwr->offset_x, vwr->ez,
                                       vwr->screen_margin);
      float local_x = (sx - page_x) / vwr->ez;
      if (local_x >= 0 && local_x <= w)
      {
        dc.page_index = page;
        dc.x = local_x;
        dc.y = world_y - top;
      }
    }
  }
  return dc;
}

/**
 * @brief Clip absolute rectangle to canvas and convert to local coordinates.
 *
 * First intersects the absolute rectangle with the canvas, then converts
 * to local coordinates relative to the canvas's top-left corner.
 *
 * @param absolute Rectangle in absolute coordinates.
 * @param canvas Reference rectangle to clip against.
 * @return Relative rectangle within canvas.
 */
fz_irect fz_relative_clipped_area(fz_irect absolute, fz_irect canvas)
{
  fz_irect inter = fz_intersect_irect(absolute, canvas);
  inter.x0 -= absolute.x0;
  inter.y0 -= absolute.y0;
  inter.x1 -= absolute.x0;
  inter.y1 -= absolute.y0;
  return inter;
}

void viewer_scroll_to_doc_coord(fz_context *ctx,
                                Viewer *vwr,
                                PageCollection *pcoll,
                                DocCoord coord,
                                float center_tolerance,
                                float h_center_tolerance)
{
  // Get viewport dimensions in world units
  float scale = vwr->ez;
  float win_w_world = (vwr->win_w - 2 * vwr->screen_margin) / scale;
  float win_h_world = (vwr->win_h - 2 * vwr->screen_margin) / scale;

  // Current viewport bounds (in world coordinates)
  float view_top    = vwr->offset_y;
  float view_bottom = vwr->offset_y + win_h_world;

  // Vertical: scroll if |rel_y - 0.5| > center_tolerance
  float target_y = coord.y + pagecollection_page_offset(pcoll, coord.page_index,
                                                        vwr->margin);
  float rel_y = (target_y - view_top) / win_h_world;  // ∈ [0, 1]
  bool scroll_y = fabsf(rel_y - 0.5f) > center_tolerance;

  // Horizontal: scroll if page does not fit and
  //             |rel_x - 0.5| > h_center_tolerance
  float target_x = 0;
  bool scroll_x = false;
  if (h_center_tolerance < INFINITY)
  {
    fz_display_list *dl = pagecollection_get(pcoll, coord.page_index);
    if (dl)
    {
      fz_rect bounds = fz_bound_display_list(ctx, dl);
      float w = bounds.x1 - bounds.x0;
      if (w > win_w_world)
      {
        target_x = coord.x - w / 2.0;
        float rel_x = (target_x - vwr->offset_x) / win_w_world;
        scroll_x = fabs(rel_x) > h_center_tolerance;
      }
    }
  }

  if (!scroll_y && !scroll_x) return; // Hysteresis: no scroll

  // Compute new offsets to center the target (if scrolling)
  if (scroll_y)
    vwr->target_offset_y = target_y - win_h_world * 0.25f;

  if (scroll_x)
    vwr->target_offset_x = target_x;

  vwr->velocity_y = 0.0;
  vwr->animating = true;
}

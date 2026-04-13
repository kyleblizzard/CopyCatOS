// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.
//
// AuraOS Window Manager — XComposite-based compositing for window shadows
//
// This module adds realistic Mac OS X Snow Leopard drop shadows to window
// frames. It uses the XComposite extension to redirect windows to off-screen
// pixmaps, then renders Gaussian-blurred shadows around each frame using Cairo.
//
// The approach:
//   - Each frame window gets an ARGB visual (32-bit with alpha channel),
//     which allows the shadow region to be transparent/semi-transparent.
//   - Frame windows are sized LARGER than the client to make room for
//     the shadow on all four sides.
//   - decor_paint() calls compositor_paint_shadow() first, then draws
//     the title bar and borders on top.

#ifndef AURA_COMPOSITOR_H
#define AURA_COMPOSITOR_H

#include "wm.h"
#include <cairo/cairo.h>
#include <X11/Xlib.h>

// ── Shadow extent constants ──
// These define how many extra pixels the frame window extends beyond the
// actual window chrome to make room for the shadow.
//
// SHADOW_RADIUS is the blur spread in pixels.
// SHADOW_Y_OFFSET shifts the shadow downward (simulating light from above).
// The top extent is smaller because the shadow is pushed down; the bottom
// extent is larger for the same reason.

#define SHADOW_RADIUS        22
#define SHADOW_Y_OFFSET       4

// How far the shadow extends beyond the window chrome on each side.
// Top is reduced by the Y-offset (shadow shifts down, so less above).
// Bottom is increased by the Y-offset (shadow shifts down, so more below).
#define SHADOW_TOP           (SHADOW_RADIUS - SHADOW_Y_OFFSET)   // 18
#define SHADOW_BOTTOM        (SHADOW_RADIUS + SHADOW_Y_OFFSET)   // 26
#define SHADOW_LEFT           SHADOW_RADIUS                       // 22
#define SHADOW_RIGHT          SHADOW_RADIUS                       // 22

// Peak alpha for the shadow blur (0.0 = invisible, 1.0 = fully opaque).
// Active (focused) windows get a stronger shadow; inactive get a softer one.
#define SHADOW_ALPHA_ACTIVE   0.40
#define SHADOW_ALPHA_INACTIVE 0.25

// Reduced blur radius for inactive windows (softer, less prominent)
#define SHADOW_RADIUS_INACTIVE 15

// ── Public API ──

// Initialize the compositor: check for required X extensions, set up
// XComposite redirection, find an ARGB visual, and pre-compute the
// shadow blur kernel. Call this once at WM startup after wm_init().
// Returns true on success, false if a required extension is missing.
bool compositor_init(AuraWM *wm);

// Find and return a 32-bit ARGB visual suitable for frame windows.
// Also creates a Colormap for that visual. The caller uses these when
// creating frame windows so they support per-pixel transparency (needed
// for the semi-transparent shadow regions).
//
// out_visual: receives the 32-bit Visual pointer
// out_colormap: receives a Colormap associated with that visual
// Returns true if an ARGB visual was found, false otherwise.
bool compositor_create_argb_visual(AuraWM *wm, Visual **out_visual,
                                   Colormap *out_colormap);

// Paint the drop shadow around a client's frame. Call this at the start
// of decor_paint(), BEFORE drawing the title bar and borders.
//
// The shadow is painted into the transparent region that surrounds the
// actual window chrome. The frame window must have been created with an
// ARGB visual and sized to include SHADOW_* padding.
//
// wm: the window manager state
// c:  the client whose shadow to paint
// cr: a Cairo context targeting the frame window's surface
void compositor_paint_shadow(AuraWM *wm, Client *c, cairo_t *cr);

// Set the X input shape on a frame window so that mouse clicks in the
// shadow region pass through to whatever is behind (the shadow is visual
// only — it should not intercept clicks).
//
// The clickable area is set to the actual frame rectangle, excluding the
// shadow padding. Call this after framing a window.
void compositor_set_input_shape(AuraWM *wm, Client *c);

// Handle an XDamage event. When a window's contents change, X sends a
// DamageNotify. We acknowledge it and trigger a repaint of the affected
// client's decorations.
void compositor_damage_notify(AuraWM *wm, XEvent *e);

// Shut down the compositor: undo XComposite redirection and free any
// cached surfaces. Call at WM shutdown.
void compositor_shutdown(AuraWM *wm);

#endif // AURA_COMPOSITOR_H

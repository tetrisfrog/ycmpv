/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <limits.h>

#include "video/out/geometry.h"
#include "options/m_option.h"

// xpos,ypos: position of the left upper corner
// widw,widh: width and height of the window
// scrw,scrh: width and height of the current screen
// The input parameters should be set to a centered window (default fallbacks).
static void m_geometry_apply(int *xpos, int *ypos, int *widw, int *widh,
                             int scrw, int scrh, struct m_geometry *gm)
{
    if (gm->wh_valid) {
        int prew = *widw, preh = *widh;
        if (gm->w > 0)
            *widw = gm->w_per ? scrw * (gm->w / 100.0) : gm->w;
        if (gm->h > 0)
            *widh = gm->h_per ? scrh * (gm->h / 100.0) : gm->h;
        // keep aspect if the other value is not set
        double asp = (double)prew / preh;
        if (gm->w > 0 && !(gm->h > 0)) {
            *widh = *widw / asp;
        } else if (!(gm->w > 0) && gm->h > 0) {
            *widw = *widh * asp;
        }
    }

    if (gm->xy_valid) {
        if (gm->x != INT_MIN) {
            *xpos = gm->x;
            if (gm->x_per)
                *xpos = (scrw - *widw) * (*xpos / 100.0);
            if (gm->x_sign)
                *xpos = scrw - *widw - *xpos;
        }
        if (gm->y != INT_MIN) {
            *ypos = gm->y;
            if (gm->y_per)
                *ypos = (scrh - *widh) * (*ypos / 100.0);
            if (gm->y_sign)
                *ypos = scrh - *widh - *ypos;
        }
    }
}

// Fit *w/*h into the size specified by geo.
static void apply_autofit(int *w, int *h, int scr_w, int scr_h,
                          struct m_geometry *geo, bool allow_upscale)
{
    if (!geo->wh_valid)
        return;

    int dummy;
    int n_w = *w, n_h = *h;
    m_geometry_apply(&dummy, &dummy, &n_w, &n_h, scr_w, scr_h, geo);

    if (!allow_upscale && *w <= n_w && *h <= n_h)
        return;

    // If aspect mismatches, always make the window smaller than the fit box
    double asp = (double)*w / *h;
    double n_asp = (double)n_w / n_h;
    if (n_asp <= asp) {
        *w = n_w;
        *h = n_w / asp;
    } else {
        *w = n_h * asp;
        *h = n_h;
    }
}

static void calc_monitor_aspect(struct mp_screen_info info,
                                int scr_w, int scr_h,
                                float *pixelaspect, int *w, int *h)
{
    *pixelaspect = 1.0 / info.opts.monitor_pixel_aspect;

    if (scr_w > 0 && scr_h > 0 && info.opts.force_monitor_aspect)
        *pixelaspect = info.opts.force_monitor_aspect * scr_h / scr_w;

    if (*pixelaspect < 1) {
        *h /= *pixelaspect;
    } else {
        *w *= *pixelaspect;
    }
}

struct mp_geometry mp_calc_window_geometry(struct mp_screen_info info,
                                           int d_w, int d_h)
{
    struct mp_screen_info_opts opts = info.opts;
    int x0, y0;

    struct mp_geometry result;
    struct mp_rect scr = info.scr;
    int scr_w = mp_rect_width(scr);
    int scr_h = mp_rect_height(scr);

    calc_monitor_aspect(info, scr_w, scr_h, &result.monitor_par, &d_w, &d_h);

    apply_autofit(&d_w, &d_h, scr_w, scr_h, &opts.autofit, true);
    apply_autofit(&d_w, &d_h, scr_w, scr_h, &opts.autofit_larger, false);

    x0 = scr.x0 + (int)(scr_w - d_w) / 2;
    y0 = scr.y0 + (int)(scr_h - d_h) / 2;

    m_geometry_apply(&x0, &y0, &d_w, &d_h, scr_w, scr_h, &opts.geometry);

    result.window_rect = (struct mp_rect) { x0, y0, x0 + d_w, y0 + d_h };
    return result;
}

void vo_copy_opts_to_screen_info(struct vo *vo, struct mp_screen_info *info)
{
    info->opts = (struct mp_screen_info_opts) {
        .geometry             = vo->opts->geometry,
        .autofit              = vo->opts->autofit,
        .autofit_larger       = vo->opts->autofit_larger,
        .force_monitor_aspect = vo->opts->force_monitor_aspect,
        .monitor_pixel_aspect = vo->opts->monitor_pixel_aspect,
    };
}


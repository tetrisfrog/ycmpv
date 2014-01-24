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

#include "video/out/vo.h"

struct mp_screen_info {
    struct mp_rect scr;
    bool is_constrained;
    struct mp_rect constrained_scr;

    struct mp_screen_info_opts {
        struct m_geometry geometry;
        struct m_geometry autofit;
        struct m_geometry autofit_larger;

        float force_monitor_aspect;
        float monitor_pixel_aspect;
    } opts;
};

struct mp_geometry {
    struct mp_rect window_rect;
    float monitor_par;
};

struct mp_geometry mp_calc_window_geometry(struct mp_screen_info info,
                                          int d_w, int d_h);
void vo_copy_opts_to_screen_info(struct vo *vo, struct mp_screen_info *info);



/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) 2020 Blender Foundation
 * All rights reserved.
 */

/** \file
 * \ingroup bgpencil
 */
#include <algorithm>
#include <string>

#include "BKE_context.h"
#include "BKE_gpencil.h"
#include "BKE_gpencil_geom.h"
#include "BKE_layer.h"
#include "BKE_main.h"
#include "BKE_material.h"

#include "BLI_blenlib.h"
#include "BLI_math.h"

#include "DNA_gpencil_types.h"
#include "DNA_layer_types.h"
#include "DNA_material_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_view3d_types.h"

#include "UI_view2d.h"

#include "ED_view3d.h"

#include "DEG_depsgraph.h"
#include "DEG_depsgraph_query.h"

#include "gpencil_io_base.h"

namespace blender::io::gpencil {

/* Constructor. */
GpencilIO::GpencilIO(const struct GpencilIOParams *iparams)
{
  params_.frame_start = iparams->frame_start;
  params_.frame_end = iparams->frame_end;
  params_.frame_cur = iparams->frame_cur;
  params_.ob = iparams->ob;
  params_.region = iparams->region;
  params_.v3d = iparams->v3d;
  params_.C = iparams->C;
  params_.mode = iparams->mode;
  params_.flag = iparams->flag;
  params_.select_mode = iparams->select_mode;
  params_.frame_mode = iparams->frame_mode;
  params_.stroke_sample = iparams->stroke_sample;
  params_.resolution = iparams->resolution;
  params_.scale = iparams->scale;

  /* Easy access data. */
  bmain_ = CTX_data_main(params_.C);
  depsgraph_ = CTX_data_depsgraph_pointer(params_.C);
  scene_ = CTX_data_scene(params_.C);
  rv3d_ = (RegionView3D *)params_.region->regiondata;
  gpd_ = (params_.ob != NULL) ? (bGPdata *)params_.ob->data : nullptr;
  cfra_ = iparams->frame_cur;

  /* Load list of selected objects. */
  create_object_list();
  object_created_ = false;

  winx_ = params_.region->winx;
  winy_ = params_.region->winy;

  invert_axis_[0] = false;
  invert_axis_[1] = true;

  /* Camera rectangle. */
  if (rv3d_->persp == RV3D_CAMOB) {
    render_x_ = (scene_->r.xsch * scene_->r.size) / 100;
    render_y_ = (scene_->r.ysch * scene_->r.size) / 100;

    ED_view3d_calc_camera_border(CTX_data_scene(params_.C),
                                 depsgraph_,
                                 params_.region,
                                 params_.v3d,
                                 rv3d_,
                                 &camera_rect_,
                                 true);
    is_camera_ = true;
    camera_ratio_ = render_x_ / (camera_rect_.xmax - camera_rect_.xmin);
    offset_[0] = camera_rect_.xmin;
    offset_[1] = camera_rect_.ymin;
  }
  else {
    is_camera_ = false;
    /* Calc selected object boundbox. Need set initial value to some variables. */
    camera_ratio_ = 1.0f;
    offset_[0] = 0.0f;
    offset_[1] = 0.0f;

    selected_objects_boundbox_set();
    rctf boundbox;
    selected_objects_boundbox_get(&boundbox);

    render_x_ = boundbox.xmax - boundbox.xmin;
    render_y_ = boundbox.ymax - boundbox.ymin;
    offset_[0] = boundbox.xmin;
    offset_[1] = boundbox.ymin;
  }

  gpl_cur_ = nullptr;
  gpf_cur_ = nullptr;
  gps_cur_ = nullptr;
}

/** Create a list of selected objects sorted from back to front */
void GpencilIO::create_object_list(void)
{
  ViewLayer *view_layer = CTX_data_view_layer(params_.C);

  float camera_z_axis[3];
  copy_v3_v3(camera_z_axis, rv3d_->viewinv[2]);
  ob_list_.clear();

  LISTBASE_FOREACH (Base *, base, &view_layer->object_bases) {
    Object *object = base->object;

    if (object->type != OB_GPENCIL) {
      continue;
    }
    if ((params_.select_mode == GP_EXPORT_ACTIVE) && (params_.ob != object)) {
      continue;
    }

    if ((params_.select_mode == GP_EXPORT_SELECTED) && ((base->flag & BASE_SELECTED) == 0)) {
      continue;
    }

    /* Save z-depth from view to sort from back to front. */
    if (is_camera_) {
      float camera_z = dot_v3v3(camera_z_axis, object->obmat[3]);
      ObjectZ obz = {camera_z, object};
      ob_list_.push_back(obz);
    }
    else {
      float zdepth = 0;
      if (rv3d_) {
        if (rv3d_->is_persp) {
          zdepth = ED_view3d_calc_zfac(rv3d_, object->obmat[3], nullptr);
        }
        else {
          zdepth = -dot_v3v3(rv3d_->viewinv[2], object->obmat[3]);
        }
        ObjectZ obz = {zdepth * -1.0f, object};
        ob_list_.push_back(obz);
      }
    }

    /* Sort list of objects from point of view. */
    ob_list_.sort(
        [](const ObjectZ &obz1, const ObjectZ &obz2) { return obz1.zdepth < obz2.zdepth; });
  }
}

/**
 * Set file input_text full path.
 * \param C: Context.
 * \param filename: Path of the file provided by save dialog.
 */
void GpencilIO::filename_set(const char *filename)
{
  BLI_strncpy(filename_, filename, FILE_MAX);
  BLI_path_abs(filename_, BKE_main_blendfile_path(bmain_));
}

/**
 * Convert to screenspace
 * \param co: 3D position
 * \param r_co: 2D position
 * \return False if error
 */
bool GpencilIO::gpencil_3d_point_to_screen_space(const float co[3], float r_co[2])
{
  float parent_co[3];
  mul_v3_m4v3(parent_co, diff_mat_, co);
  float screen_co[2];
  eV3DProjTest test = (eV3DProjTest)(V3D_PROJ_RET_OK);
  if (ED_view3d_project_float_global(params_.region, parent_co, screen_co, test) ==
      V3D_PROJ_RET_OK) {
    if (!ELEM(V2D_IS_CLIPPED, screen_co[0], screen_co[1])) {
      copy_v2_v2(r_co, screen_co);
      /* Invert X axis. */
      if (invert_axis_[0]) {
        r_co[0] = winx_ - r_co[0];
      }
      /* Invert Y axis. */
      if (invert_axis_[1]) {
        r_co[1] = winy_ - r_co[1];
      }
      /* Apply offset and scale. */
      sub_v2_v2(r_co, offset_);
      mul_v2_fl(r_co, camera_ratio_);

      return true;
    }
  }
  r_co[0] = V2D_IS_CLIPPED;
  r_co[1] = V2D_IS_CLIPPED;

  /* Invert X axis. */
  if (invert_axis_[0]) {
    r_co[0] = winx_ - r_co[0];
  }
  /* Invert Y axis. */
  if (invert_axis_[1]) {
    r_co[1] = winy_ - r_co[1];
  }

  return false;
}

/**
 * Convert to project space
 * \param co: 3D position
 * \param r_co: 2D position
 */
void GpencilIO::gpencil_3d_point_to_project_space(const float mat[4][4],
                                                  const float co[3],
                                                  float r_co[2])
{
  float parent_co[3];
  mul_v3_m4v3(parent_co, diff_mat_, co);

  float tmp[4];
  copy_v3_v3(tmp, parent_co);
  tmp[3] = 1.0f;
  mul_m4_v4(mat, tmp);

  copy_v2_v2(r_co, tmp);

  /* Invert X axis. */
  if (invert_axis_[0]) {
    r_co[0] = winx_ - r_co[0];
  }
  /* Invert Y axis. */
  if (invert_axis_[1]) {
    r_co[1] = winy_ - r_co[1];
  }
}

/**
 * Convert to 2D
 * \param co: 3D position
 * \param r_co: 2D position
 */
void GpencilIO::gpencil_3d_point_to_2D(const float co[3], float r_co[2])
{
  const bool is_camera = (bool)(rv3d_->persp == RV3D_CAMOB);
  if (is_camera) {
    gpencil_3d_point_to_project_space(rv3d_->viewmat, co, r_co);
  }
  else {
    gpencil_3d_point_to_screen_space(co, r_co);
  }
}

/**
 * Get average pressure
 * \param gps: Pointer to stroke
 * \retun value
 */
float GpencilIO::stroke_average_pressure_get(struct bGPDstroke *gps)
{
  bGPDspoint *pt = nullptr;

  if (gps->totpoints == 1) {
    pt = &gps->points[0];
    return pt->pressure;
  }

  float tot = 0.0f;
  for (int32_t i = 0; i < gps->totpoints; i++) {
    pt = &gps->points[i];
    tot += pt->pressure;
  }

  return tot / (float)gps->totpoints;
}

/**
 * Check if the thickness of the stroke is constant
 * \param gps: Pointer to stroke
 * \retun true if all points thickness are equal.
 */
bool GpencilIO::is_stroke_thickness_constant(struct bGPDstroke *gps)
{
  if (gps->totpoints == 1) {
    return true;
  }

  bGPDspoint *pt = &gps->points[0];
  float prv_pressure = pt->pressure;

  for (int32_t i = 0; i < gps->totpoints; i++) {
    pt = &gps->points[i];
    if (pt->pressure != prv_pressure) {
      return false;
    }
  }

  return true;
}

/**
 * Get radius of point
 * \param gps: Stroke
 * \return Radius in pixels
 */
float GpencilIO::stroke_point_radius_get(struct bGPDstroke *gps)
{
  const bGPDlayer *gpl = gpl_current_get();
  bGPDspoint *pt = nullptr;
  float v1[2], screen_co[2], screen_ex[2];

  pt = &gps->points[0];
  gpencil_3d_point_to_2D(&pt->x, screen_co);

  /* Radius. */
  bGPDstroke *gps_perimeter = BKE_gpencil_stroke_perimeter_from_view(
      rv3d_, gpd_, gpl, gps, 3, diff_mat_);

  pt = &gps_perimeter->points[0];
  gpencil_3d_point_to_2D(&pt->x, screen_ex);

  sub_v2_v2v2(v1, screen_co, screen_ex);
  float radius = len_v2(v1);
  BKE_gpencil_free_stroke(gps_perimeter);

  return MAX2(radius, 1.0f);
}

/**
 * Convert a color to Hex value (#FFFFFF)
 * \param color: Original RGB color
 * \return String with the conversion
 */
std::string GpencilIO::rgb_to_hexstr(float color[3])
{
  uint8_t r = color[0] * 255.0f;
  uint8_t g = color[1] * 255.0f;
  uint8_t b = color[2] * 255.0f;
  char hex_string[20];
  sprintf(hex_string, "#%02X%02X%02X", r, g, b);

  std::string hexstr = hex_string;

  return hexstr;
}

/**
 * Convert a color to gray scale.
 * \param color: Color to convert
 */
void GpencilIO::rgb_to_grayscale(float color[3])
{
  float grayscale = ((0.3f * color[0]) + (0.59f * color[1]) + (0.11f * color[2]));
  color[0] = grayscale;
  color[1] = grayscale;
  color[2] = grayscale;
}

/**
 * Convert a full string to lowercase
 * \param input_text: Input input_text
 * \return Lower case string
 */
std::string GpencilIO::to_lower_string(char *input_text)
{
  std::string text = input_text;
  /* First remove any point of the string. */
  size_t found = text.find_first_of(".");
  while (found != std::string::npos) {
    text[found] = '_';
    found = text.find_first_of(".", found + 1);
  }

  std::transform(
      text.begin(), text.end(), text.begin(), [](unsigned char c) { return std::tolower(c); });

  return text;
}

struct bGPDlayer *GpencilIO::gpl_current_get(void)
{
  return gpl_cur_;
}

void GpencilIO::gpl_current_set(struct bGPDlayer *gpl)
{
  gpl_cur_ = gpl;
  BKE_gpencil_layer_transform_matrix_get(depsgraph_, params_.ob, gpl, diff_mat_);
  mul_m4_m4m4(diff_mat_, diff_mat_, gpl->layer_invmat);
}

struct bGPDframe *GpencilIO::gpf_current_get(void)
{
  return gpf_cur_;
}

void GpencilIO::gpf_current_set(struct bGPDframe *gpf)
{
  gpf_cur_ = gpf;
}
struct bGPDstroke *GpencilIO::gps_current_get(void)
{
  return gps_cur_;
}

void GpencilIO::gps_current_set(struct Object *ob, struct bGPDstroke *gps, const bool set_colors)
{
  gps_cur_ = gps;
  if (set_colors) {
    gp_style_ = BKE_gpencil_material_settings(ob, gps->mat_nr + 1);

    is_stroke_ = ((gp_style_->flag & GP_MATERIAL_STROKE_SHOW) &&
                  (gp_style_->stroke_rgba[3] > GPENCIL_ALPHA_OPACITY_THRESH));
    is_fill_ = ((gp_style_->flag & GP_MATERIAL_FILL_SHOW) &&
                (gp_style_->fill_rgba[3] > GPENCIL_ALPHA_OPACITY_THRESH));

    /* Stroke color. */
    copy_v4_v4(stroke_color_, gp_style_->stroke_rgba);
    avg_opacity_ = 0;
    /* Get average vertex color and apply. */
    float avg_color[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    for (int32_t i = 0; i < gps->totpoints; i++) {
      bGPDspoint *pt = &gps->points[i];
      add_v4_v4(avg_color, pt->vert_color);
      avg_opacity_ += pt->strength;
    }

    mul_v4_v4fl(avg_color, avg_color, 1.0f / (float)gps->totpoints);
    interp_v3_v3v3(stroke_color_, stroke_color_, avg_color, avg_color[3]);
    avg_opacity_ /= (float)gps->totpoints;

    /* Fill color. */
    copy_v4_v4(fill_color_, gp_style_->fill_rgba);
    /* Apply vertex color for fill. */
    interp_v3_v3v3(fill_color_, fill_color_, gps->vert_color_fill, gps->vert_color_fill[3]);
  }
}

struct MaterialGPencilStyle *GpencilIO::gp_style_current_get(void)
{
  return gp_style_;
}

bool GpencilIO::material_is_stroke(void)
{
  return is_stroke_;
}

bool GpencilIO::material_is_fill(void)
{
  return is_fill_;
}

float GpencilIO::stroke_average_opacity_get(void)
{
  return avg_opacity_;
}

bool GpencilIO::is_camera_mode(void)
{
  return is_camera_;
}

/* Calc selected strokes boundbox. */
void GpencilIO::selected_objects_boundbox_set(void)
{
  const float gap = 10.0f;
  const bGPDspoint *pt;
  int32_t i;

  float screen_co[2];
  float r_min[2], r_max[2];
  INIT_MINMAX2(r_min, r_max);

  for (ObjectZ &obz : ob_list_) {
    Object *ob = obz.ob;
    /* Use evaluated version to get strokes with modifiers. */
    Object *ob_eval = (Object *)DEG_get_evaluated_id(depsgraph_, &ob->id);
    bGPdata *gpd_eval = (bGPdata *)ob_eval->data;

    LISTBASE_FOREACH (bGPDlayer *, gpl, &gpd_eval->layers) {
      if (gpl->flag & GP_LAYER_HIDE) {
        continue;
      }
      BKE_gpencil_layer_transform_matrix_get(depsgraph_, ob_eval, gpl, diff_mat_);

      bGPDframe *gpf = gpl->actframe;
      if (gpf == nullptr) {
        continue;
      }

      LISTBASE_FOREACH (bGPDstroke *, gps, &gpf->strokes) {
        if (gps->totpoints == 0) {
          continue;
        }
        for (i = 0, pt = gps->points; i < gps->totpoints; i++, pt++) {
          /* Convert to 2D. */
          gpencil_3d_point_to_2D(&pt->x, screen_co);
          minmax_v2v2_v2(r_min, r_max, screen_co);
        }
      }
    }
  }
  /* Add small gap. */
  add_v2_fl(r_min, gap * -1.0f);
  add_v2_fl(r_max, gap);

  select_boundbox_.xmin = r_min[0];
  select_boundbox_.ymin = r_min[1];
  select_boundbox_.xmax = r_max[0];
  select_boundbox_.ymax = r_max[1];
}

void GpencilIO::selected_objects_boundbox_get(rctf *boundbox)
{
  boundbox->xmin = select_boundbox_.xmin;
  boundbox->xmax = select_boundbox_.xmax;
  boundbox->ymin = select_boundbox_.ymin;
  boundbox->ymax = select_boundbox_.ymax;
}

void GpencilIO::frame_number_set(const int value)
{
  cfra_ = value;
}

}  // namespace blender::io::gpencil
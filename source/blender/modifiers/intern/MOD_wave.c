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
 * along with this program; if not, write to the Free Software  Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) 2005 by the Blender Foundation.
 * All rights reserved.
 */

/** \file
 * \ingroup modifiers
 */

#include "BLI_utildefines.h"

#include "BLI_math.h"

#include "BLT_translation.h"

#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"

#include "BKE_context.h"
#include "BKE_deform.h"
#include "BKE_editmesh.h"
#include "BKE_lib_id.h"
#include "BKE_lib_query.h"
#include "BKE_mesh.h"
#include "BKE_scene.h"
#include "BKE_screen.h"
#include "BKE_texture.h"

#include "UI_interface.h"
#include "UI_resources.h"

#include "RNA_access.h"

#include "MEM_guardedalloc.h"
#include "RE_shader_ext.h"

#include "MOD_modifiertypes.h"
#include "MOD_ui_common.h"
#include "MOD_util.h"

#include "DEG_depsgraph.h"
#include "DEG_depsgraph_query.h"

static void initData(ModifierData *md)
{
  WaveModifierData *wmd = (WaveModifierData *)md;  // whadya know, moved here from Iraq

  wmd->flag |= (MOD_WAVE_X | MOD_WAVE_Y | MOD_WAVE_CYCL | MOD_WAVE_NORM_X | MOD_WAVE_NORM_Y |
                MOD_WAVE_NORM_Z);

  wmd->objectcenter = NULL;
  wmd->texture = NULL;
  wmd->map_object = NULL;
  wmd->height = 0.5f;
  wmd->width = 1.5f;
  wmd->speed = 0.25f;
  wmd->narrow = 1.5f;
  wmd->lifetime = 0.0f;
  wmd->damp = 10.0f;
  wmd->falloff = 0.0f;
  wmd->texmapping = MOD_DISP_MAP_LOCAL;
  wmd->defgrp_name[0] = 0;
}

static bool dependsOnTime(ModifierData *UNUSED(md))
{
  return true;
}

static void foreachObjectLink(ModifierData *md, Object *ob, ObjectWalkFunc walk, void *userData)
{
  WaveModifierData *wmd = (WaveModifierData *)md;

  walk(userData, ob, &wmd->objectcenter, IDWALK_CB_NOP);
  walk(userData, ob, &wmd->map_object, IDWALK_CB_NOP);
}

static void foreachIDLink(ModifierData *md, Object *ob, IDWalkFunc walk, void *userData)
{
  WaveModifierData *wmd = (WaveModifierData *)md;

  walk(userData, ob, (ID **)&wmd->texture, IDWALK_CB_USER);

  foreachObjectLink(md, ob, (ObjectWalkFunc)walk, userData);
}

static void foreachTexLink(ModifierData *md, Object *ob, TexWalkFunc walk, void *userData)
{
  walk(userData, ob, md, "texture");
}

static void updateDepsgraph(ModifierData *md, const ModifierUpdateDepsgraphContext *ctx)
{
  WaveModifierData *wmd = (WaveModifierData *)md;
  if (wmd->objectcenter != NULL) {
    DEG_add_object_relation(ctx->node, wmd->objectcenter, DEG_OB_COMP_TRANSFORM, "Wave Modifier");
  }
  if (wmd->map_object != NULL) {
    DEG_add_object_relation(ctx->node, wmd->map_object, DEG_OB_COMP_TRANSFORM, "Wave Modifier");
  }
  if (wmd->objectcenter != NULL || wmd->map_object != NULL) {
    DEG_add_modifier_to_transform_relation(ctx->node, "Wave Modifier");
  }
  if (wmd->texture != NULL) {
    DEG_add_generic_id_relation(ctx->node, &wmd->texture->id, "Wave Modifier");
  }
}

static void requiredDataMask(Object *UNUSED(ob),
                             ModifierData *md,
                             CustomData_MeshMasks *r_cddata_masks)
{
  WaveModifierData *wmd = (WaveModifierData *)md;

  /* ask for UV coordinates if we need them */
  if (wmd->texture && wmd->texmapping == MOD_DISP_MAP_UV) {
    r_cddata_masks->fmask |= CD_MASK_MTFACE;
  }

  /* ask for vertexgroups if we need them */
  if (wmd->defgrp_name[0] != '\0') {
    r_cddata_masks->vmask |= CD_MASK_MDEFORMVERT;
  }
}

static bool dependsOnNormals(ModifierData *md)
{
  WaveModifierData *wmd = (WaveModifierData *)md;

  return (wmd->flag & MOD_WAVE_NORM) != 0;
}

static void waveModifier_do(WaveModifierData *md,
                            const ModifierEvalContext *ctx,
                            Object *ob,
                            Mesh *mesh,
                            float (*vertexCos)[3],
                            int numVerts)
{
  WaveModifierData *wmd = (WaveModifierData *)md;
  MVert *mvert = NULL;
  MDeformVert *dvert;
  int defgrp_index;
  float ctime = DEG_get_ctime(ctx->depsgraph);
  float minfac = (float)(1.0 / exp(wmd->width * wmd->narrow * wmd->width * wmd->narrow));
  float lifefac = wmd->height;
  float(*tex_co)[3] = NULL;
  const int wmd_axis = wmd->flag & (MOD_WAVE_X | MOD_WAVE_Y);
  const float falloff = wmd->falloff;
  float falloff_fac = 1.0f; /* when falloff == 0.0f this stays at 1.0f */
  const bool invert_group = (wmd->flag & MOD_WAVE_INVERT_VGROUP) != 0;

  if ((wmd->flag & MOD_WAVE_NORM) && (mesh != NULL)) {
    mvert = mesh->mvert;
  }

  if (wmd->objectcenter != NULL) {
    float mat[4][4];
    /* get the control object's location in local coordinates */
    invert_m4_m4(ob->imat, ob->obmat);
    mul_m4_m4m4(mat, ob->imat, wmd->objectcenter->obmat);

    wmd->startx = mat[3][0];
    wmd->starty = mat[3][1];
  }

  /* get the index of the deform group */
  MOD_get_vgroup(ob, mesh, wmd->defgrp_name, &dvert, &defgrp_index);

  if (wmd->damp == 0.0f) {
    wmd->damp = 10.0f;
  }

  if (wmd->lifetime != 0.0f) {
    float x = ctime - wmd->timeoffs;

    if (x > wmd->lifetime) {
      lifefac = x - wmd->lifetime;

      if (lifefac > wmd->damp) {
        lifefac = 0.0;
      }
      else {
        lifefac = (float)(wmd->height * (1.0f - sqrtf(lifefac / wmd->damp)));
      }
    }
  }

  Tex *tex_target = wmd->texture;
  if (mesh != NULL && tex_target != NULL) {
    tex_co = MEM_malloc_arrayN(numVerts, sizeof(*tex_co), "waveModifier_do tex_co");
    MOD_get_texture_coords((MappingInfoModifierData *)wmd, ctx, ob, mesh, vertexCos, tex_co);

    MOD_init_texture((MappingInfoModifierData *)wmd, ctx);
  }

  if (lifefac != 0.0f) {
    /* avoid divide by zero checks within the loop */
    float falloff_inv = falloff != 0.0f ? 1.0f / falloff : 1.0f;
    int i;

    for (i = 0; i < numVerts; i++) {
      float *co = vertexCos[i];
      float x = co[0] - wmd->startx;
      float y = co[1] - wmd->starty;
      float amplit = 0.0f;
      float def_weight = 1.0f;

      /* get weights */
      if (dvert) {
        def_weight = invert_group ? 1.0f - BKE_defvert_find_weight(&dvert[i], defgrp_index) :
                                    BKE_defvert_find_weight(&dvert[i], defgrp_index);

        /* if this vert isn't in the vgroup, don't deform it */
        if (def_weight == 0.0f) {
          continue;
        }
      }

      switch (wmd_axis) {
        case MOD_WAVE_X | MOD_WAVE_Y:
          amplit = sqrtf(x * x + y * y);
          break;
        case MOD_WAVE_X:
          amplit = x;
          break;
        case MOD_WAVE_Y:
          amplit = y;
          break;
      }

      /* this way it makes nice circles */
      amplit -= (ctime - wmd->timeoffs) * wmd->speed;

      if (wmd->flag & MOD_WAVE_CYCL) {
        amplit = (float)fmodf(amplit - wmd->width, 2.0f * wmd->width) + wmd->width;
      }

      if (falloff != 0.0f) {
        float dist = 0.0f;

        switch (wmd_axis) {
          case MOD_WAVE_X | MOD_WAVE_Y:
            dist = sqrtf(x * x + y * y);
            break;
          case MOD_WAVE_X:
            dist = fabsf(x);
            break;
          case MOD_WAVE_Y:
            dist = fabsf(y);
            break;
        }

        falloff_fac = (1.0f - (dist * falloff_inv));
        CLAMP(falloff_fac, 0.0f, 1.0f);
      }

      /* GAUSSIAN */
      if ((falloff_fac != 0.0f) && (amplit > -wmd->width) && (amplit < wmd->width)) {
        amplit = amplit * wmd->narrow;
        amplit = (float)(1.0f / expf(amplit * amplit) - minfac);

        /*apply texture*/
        if (tex_co) {
          Scene *scene = DEG_get_evaluated_scene(ctx->depsgraph);
          TexResult texres;
          texres.nor = NULL;
          BKE_texture_get_value(scene, tex_target, tex_co[i], &texres, false);
          amplit *= texres.tin;
        }

        /*apply weight & falloff */
        amplit *= def_weight * falloff_fac;

        if (mvert) {
          /* move along normals */
          if (wmd->flag & MOD_WAVE_NORM_X) {
            co[0] += (lifefac * amplit) * mvert[i].no[0] / 32767.0f;
          }
          if (wmd->flag & MOD_WAVE_NORM_Y) {
            co[1] += (lifefac * amplit) * mvert[i].no[1] / 32767.0f;
          }
          if (wmd->flag & MOD_WAVE_NORM_Z) {
            co[2] += (lifefac * amplit) * mvert[i].no[2] / 32767.0f;
          }
        }
        else {
          /* move along local z axis */
          co[2] += lifefac * amplit;
        }
      }
    }
  }

  MEM_SAFE_FREE(tex_co);
}

static void deformVerts(ModifierData *md,
                        const ModifierEvalContext *ctx,
                        Mesh *mesh,
                        float (*vertexCos)[3],
                        int numVerts)
{
  WaveModifierData *wmd = (WaveModifierData *)md;
  Mesh *mesh_src = NULL;

  if (wmd->flag & MOD_WAVE_NORM) {
    mesh_src = MOD_deform_mesh_eval_get(ctx->object, NULL, mesh, vertexCos, numVerts, true, false);
  }
  else if (wmd->texture != NULL || wmd->defgrp_name[0] != '\0') {
    mesh_src = MOD_deform_mesh_eval_get(ctx->object, NULL, mesh, NULL, numVerts, false, false);
  }

  waveModifier_do(wmd, ctx, ctx->object, mesh_src, vertexCos, numVerts);

  if (!ELEM(mesh_src, NULL, mesh)) {
    BKE_id_free(NULL, mesh_src);
  }
}

static void deformVertsEM(ModifierData *md,
                          const ModifierEvalContext *ctx,
                          struct BMEditMesh *editData,
                          Mesh *mesh,
                          float (*vertexCos)[3],
                          int numVerts)
{
  WaveModifierData *wmd = (WaveModifierData *)md;
  Mesh *mesh_src = NULL;

  if (wmd->flag & MOD_WAVE_NORM) {
    mesh_src = MOD_deform_mesh_eval_get(
        ctx->object, editData, mesh, vertexCos, numVerts, true, false);
  }
  else if (wmd->texture != NULL || wmd->defgrp_name[0] != '\0') {
    mesh_src = MOD_deform_mesh_eval_get(ctx->object, editData, mesh, NULL, numVerts, false, false);
  }

  waveModifier_do(wmd, ctx, ctx->object, mesh_src, vertexCos, numVerts);

  if (!ELEM(mesh_src, NULL, mesh)) {
    BKE_id_free(NULL, mesh_src);
  }
}

static void panel_draw(const bContext *C, Panel *panel)
{
  uiLayout *sub, *row, *col, *split;
  uiLayout *layout = panel->layout;

  PointerRNA ptr;
  PointerRNA ob_ptr;
  modifier_panel_get_property_pointers(C, panel, &ob_ptr, &ptr);

  bool has_vertex_group = RNA_string_length(&ptr, "vertex_group") != 0;

  split = uiLayoutSplit(layout, 0.5f, false);
  col = uiLayoutColumn(split, true);
  uiItemL(col, IFACE_("Motion:"), ICON_NONE);
  uiItemR(col, &ptr, "use_x", 0, NULL, ICON_NONE);
  uiItemR(col, &ptr, "use_y", 0, NULL, ICON_NONE);
  uiItemR(col, &ptr, "use_cyclic", 0, NULL, ICON_NONE);

  col = uiLayoutColumn(split, false);
  uiItemR(col, &ptr, "use_normal", 0, NULL, ICON_NONE);
  sub = uiLayoutColumn(col, true);
  uiLayoutSetActive(sub, RNA_boolean_get(&ptr, "use_normal"));
  uiItemR(sub, &ptr, "use_normal_x", 0, "X", ICON_NONE);
  uiItemR(sub, &ptr, "use_normal_y", 0, "Y", ICON_NONE);
  uiItemR(sub, &ptr, "use_normal_z", 0, "Z", ICON_NONE);

  uiItemR(layout, &ptr, "falloff_radius", 0, "Falloff", ICON_NONE);
  uiItemR(layout, &ptr, "height", UI_ITEM_R_SLIDER, NULL, ICON_NONE);
  uiItemR(layout, &ptr, "width", UI_ITEM_R_SLIDER, NULL, ICON_NONE);
  uiItemR(layout, &ptr, "narrowness", UI_ITEM_R_SLIDER, NULL, ICON_NONE);

  row = uiLayoutRow(layout, true);
  uiItemPointerR(row, &ptr, "vertex_group", &ob_ptr, "vertex_groups", "", ICON_NONE);
  sub = uiLayoutRow(row, true);
  uiLayoutSetActive(sub, has_vertex_group);
  uiItemR(sub, &ptr, "invert_vertex_group", 0, "", ICON_ARROW_LEFTRIGHT);

  modifier_panel_end(layout, &ptr);
}

static void position_panel_draw(const bContext *C, Panel *panel)
{
  uiLayout *col;
  uiLayout *layout = panel->layout;

  PointerRNA ptr;
  modifier_panel_get_property_pointers(C, panel, NULL, &ptr);

  uiItemR(layout, &ptr, "start_position_object", 0, "", ICON_NONE);
  col = uiLayoutColumn(layout, true);
  uiItemR(col, &ptr, "start_position_x", 0, "X", ICON_NONE);
  uiItemR(col, &ptr, "start_position_y", 0, "Y", ICON_NONE);
}

static void time_panel_draw(const bContext *C, Panel *panel)
{
  uiLayout *layout = panel->layout;

  PointerRNA ptr;
  modifier_panel_get_property_pointers(C, panel, NULL, &ptr);

  uiItemR(layout, &ptr, "time_offset", 0, "Offset", ICON_NONE);
  uiItemR(layout, &ptr, "lifetime", 0, "Life", ICON_NONE);
  uiItemR(layout, &ptr, "damping_time", 0, "Damping", ICON_NONE);
  uiItemR(layout, &ptr, "speed", UI_ITEM_R_SLIDER, NULL, ICON_NONE);
}

static void texture_panel_draw(const bContext *C, Panel *panel)
{
  uiLayout *col;
  uiLayout *layout = panel->layout;

  PointerRNA ptr;
  PointerRNA ob_ptr;
  modifier_panel_get_property_pointers(C, panel, &ob_ptr, &ptr);

  int texture_coords = RNA_enum_get(&ptr, "texture_coords");

  col = uiLayoutColumn(layout, false);
  uiTemplateID(col, C, &ptr, "texture", "texture.new", NULL, NULL, 0, ICON_NONE, NULL);

  uiItemR(layout, &ptr, "texture_coords", 0, NULL, ICON_NONE);
  if (texture_coords == MOD_DISP_MAP_OBJECT) {
    uiItemR(layout, &ptr, "texture_coords_object", 0, "", ICON_NONE);
  }
  else if (texture_coords == MOD_DISP_MAP_UV && RNA_enum_get(&ob_ptr, "type") == OB_MESH) {
    PointerRNA obj_data_ptr = RNA_pointer_get(&ob_ptr, "data");
    uiItemPointerR(col, &ptr, "uv_layer", &obj_data_ptr, "uv_layers", "", ICON_NONE);
  }
}

static void panelRegister(ARegionType *region_type)
{
  PanelType *panel_type = modifier_panel_register(region_type, "Wave", panel_draw);
  modifier_subpanel_register(
      region_type, "wave_position", "Start Position", NULL, position_panel_draw, true, panel_type);
  modifier_subpanel_register(
      region_type, "wave_time", "Time", NULL, time_panel_draw, true, panel_type);
  modifier_subpanel_register(
      region_type, "texture_position", "Texture", NULL, texture_panel_draw, false, panel_type);
}

ModifierTypeInfo modifierType_Wave = {
    /* name */ "Wave",
    /* structName */ "WaveModifierData",
    /* structSize */ sizeof(WaveModifierData),
    /* type */ eModifierTypeType_OnlyDeform,
    /* flags */ eModifierTypeFlag_AcceptsCVs | eModifierTypeFlag_AcceptsLattice |
        eModifierTypeFlag_SupportsEditmode,

    /* copyData */ modifier_copyData_generic,

    /* deformVerts */ deformVerts,
    /* deformMatrices */ NULL,
    /* deformVertsEM */ deformVertsEM,
    /* deformMatricesEM */ NULL,
    /* applyModifier */ NULL,

    /* initData */ initData,
    /* requiredDataMask */ requiredDataMask,
    /* freeData */ NULL,
    /* isDisabled */ NULL,
    /* updateDepsgraph */ updateDepsgraph,
    /* dependsOnTime */ dependsOnTime,
    /* dependsOnNormals */ dependsOnNormals,
    /* foreachObjectLink */ foreachObjectLink,
    /* foreachIDLink */ foreachIDLink,
    /* foreachTexLink */ foreachTexLink,
    /* freeRuntimeData */ NULL,
    /* panelRegister */ panelRegister,
};

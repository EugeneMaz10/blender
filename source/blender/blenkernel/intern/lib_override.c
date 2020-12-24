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
 * The Original Code is Copyright (C) 2016 by Blender Foundation.
 * All rights reserved.
 */

/** \file
 * \ingroup bke
 */

#include <stdlib.h>
#include <string.h>

#include "MEM_guardedalloc.h"

#include "DNA_ID.h"
#include "DNA_collection_types.h"
#include "DNA_key_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "DEG_depsgraph.h"
#include "DEG_depsgraph_build.h"

#include "BKE_armature.h"
#include "BKE_collection.h"
#include "BKE_global.h"
#include "BKE_idtype.h"
#include "BKE_key.h"
#include "BKE_layer.h"
#include "BKE_lib_id.h"
#include "BKE_lib_override.h"
#include "BKE_lib_query.h"
#include "BKE_lib_remap.h"
#include "BKE_main.h"
#include "BKE_scene.h"

#include "BLI_ghash.h"
#include "BLI_listbase.h"
#include "BLI_string.h"
#include "BLI_task.h"
#include "BLI_utildefines.h"

#include "RNA_access.h"
#include "RNA_types.h"

#define OVERRIDE_AUTO_CHECK_DELAY 0.2 /* 200ms between auto-override checks. */
//#define DEBUG_OVERRIDE_TIMEIT

#ifdef DEBUG_OVERRIDE_TIMEIT
#  include "PIL_time_utildefines.h"
#endif

static void lib_override_library_property_copy(IDOverrideLibraryProperty *op_dst,
                                               IDOverrideLibraryProperty *op_src);
static void lib_override_library_property_operation_copy(
    IDOverrideLibraryPropertyOperation *opop_dst, IDOverrideLibraryPropertyOperation *opop_src);

static void lib_override_library_property_clear(IDOverrideLibraryProperty *op);
static void lib_override_library_property_operation_clear(
    IDOverrideLibraryPropertyOperation *opop);

/** Initialize empty overriding of \a reference_id by \a local_id. */
IDOverrideLibrary *BKE_lib_override_library_init(ID *local_id, ID *reference_id)
{
  /* If reference_id is NULL, we are creating an override template for purely local data.
   * Else, reference *must* be linked data. */
  BLI_assert(reference_id == NULL || reference_id->lib != NULL);
  BLI_assert(local_id->override_library == NULL);

  ID *ancestor_id;
  for (ancestor_id = reference_id; ancestor_id != NULL && ancestor_id->override_library != NULL &&
                                   ancestor_id->override_library->reference != NULL;
       ancestor_id = ancestor_id->override_library->reference) {
    /* pass */
  }

  if (ancestor_id != NULL && ancestor_id->override_library != NULL) {
    /* Original ID has a template, use it! */
    BKE_lib_override_library_copy(local_id, ancestor_id, true);
    if (local_id->override_library->reference != reference_id) {
      id_us_min(local_id->override_library->reference);
      local_id->override_library->reference = reference_id;
      id_us_plus(local_id->override_library->reference);
    }
    return local_id->override_library;
  }

  /* Else, generate new empty override. */
  local_id->override_library = MEM_callocN(sizeof(*local_id->override_library), __func__);
  local_id->override_library->reference = reference_id;
  id_us_plus(local_id->override_library->reference);
  local_id->tag &= ~LIB_TAG_OVERRIDE_LIBRARY_REFOK;
  /* TODO do we want to add tag or flag to referee to mark it as such? */
  return local_id->override_library;
}

/** Shalow or deep copy of a whole override from \a src_id to \a dst_id. */
void BKE_lib_override_library_copy(ID *dst_id, const ID *src_id, const bool do_full_copy)
{
  BLI_assert(ID_IS_OVERRIDE_LIBRARY(src_id));

  if (dst_id->override_library != NULL) {
    if (src_id->override_library == NULL) {
      BKE_lib_override_library_free(&dst_id->override_library, true);
      return;
    }

    BKE_lib_override_library_clear(dst_id->override_library, true);
  }
  else if (src_id->override_library == NULL) {
    /* Virtual overrides of embedded data does not require any extra work. */
    return;
  }
  else {
    BKE_lib_override_library_init(dst_id, NULL);
  }

  /* If source is already overriding data, we copy it but reuse its reference for dest ID.
   * Otherwise, source is only an override template, it then becomes reference of dest ID. */
  dst_id->override_library->reference = src_id->override_library->reference ?
                                            src_id->override_library->reference :
                                            (ID *)src_id;
  id_us_plus(dst_id->override_library->reference);

  if (do_full_copy) {
    BLI_duplicatelist(&dst_id->override_library->properties,
                      &src_id->override_library->properties);
    for (IDOverrideLibraryProperty *op_dst = dst_id->override_library->properties.first,
                                   *op_src = src_id->override_library->properties.first;
         op_dst;
         op_dst = op_dst->next, op_src = op_src->next) {
      lib_override_library_property_copy(op_dst, op_src);
    }
  }

  dst_id->tag &= ~LIB_TAG_OVERRIDE_LIBRARY_REFOK;
}

/** Clear any overriding data from given \a override. */
void BKE_lib_override_library_clear(IDOverrideLibrary *override, const bool do_id_user)
{
  BLI_assert(override != NULL);

  if (!ELEM(NULL, override->runtime, override->runtime->rna_path_to_override_properties)) {
    BLI_ghash_clear(override->runtime->rna_path_to_override_properties, NULL, NULL);
  }

  LISTBASE_FOREACH (IDOverrideLibraryProperty *, op, &override->properties) {
    lib_override_library_property_clear(op);
  }
  BLI_freelistN(&override->properties);

  if (do_id_user) {
    id_us_min(override->reference);
    /* override->storage should never be refcounted... */
  }
}

/** Free given \a override. */
void BKE_lib_override_library_free(struct IDOverrideLibrary **override, const bool do_id_user)
{
  BLI_assert(*override != NULL);

  if ((*override)->runtime != NULL) {
    if ((*override)->runtime->rna_path_to_override_properties != NULL) {
      BLI_ghash_free((*override)->runtime->rna_path_to_override_properties, NULL, NULL);
    }
    MEM_SAFE_FREE((*override)->runtime);
  }

  BKE_lib_override_library_clear(*override, do_id_user);
  MEM_freeN(*override);
  *override = NULL;
}

static ID *lib_override_library_create_from(Main *bmain, ID *reference_id)
{
  ID *local_id = BKE_id_copy(bmain, reference_id);

  if (local_id == NULL) {
    return NULL;
  }
  id_us_min(local_id);

  BKE_lib_override_library_init(local_id, reference_id);

  /* Note: From liboverride perspective (and RNA one), shape keys are considered as local embedded
   * data-blocks, just like root node trees or master collections. Therefore, we never need to
   * create overrides for them. We need a way to mark them as overrides though. */
  Key *reference_key;
  if ((reference_key = BKE_key_from_id(reference_id)) != NULL) {
    Key *local_key = BKE_key_from_id(local_id);
    BLI_assert(local_key != NULL);
    local_key->id.flag |= LIB_EMBEDDED_DATA_LIB_OVERRIDE;
  }

  return local_id;
}

/** Create an overridden local copy of linked reference. */
ID *BKE_lib_override_library_create_from_id(Main *bmain,
                                            ID *reference_id,
                                            const bool do_tagged_remap)
{
  BLI_assert(reference_id != NULL);
  BLI_assert(reference_id->lib != NULL);

  ID *local_id = lib_override_library_create_from(bmain, reference_id);

  if (do_tagged_remap) {
    Key *reference_key, *local_key = NULL;
    if ((reference_key = BKE_key_from_id(reference_id)) != NULL) {
      local_key = BKE_key_from_id(local_id);
      BLI_assert(local_key != NULL);
    }

    ID *other_id;
    FOREACH_MAIN_ID_BEGIN (bmain, other_id) {
      if ((other_id->tag & LIB_TAG_DOIT) != 0 && other_id->lib == NULL) {
        /* Note that using ID_REMAP_SKIP_INDIRECT_USAGE below is superfluous, as we only remap
         * local IDs usages anyway. */
        BKE_libblock_relink_ex(bmain,
                               other_id,
                               reference_id,
                               local_id,
                               ID_REMAP_SKIP_INDIRECT_USAGE | ID_REMAP_SKIP_OVERRIDE_LIBRARY);
        if (reference_key != NULL) {
          BKE_libblock_relink_ex(bmain,
                                 other_id,
                                 &reference_key->id,
                                 &local_key->id,
                                 ID_REMAP_SKIP_INDIRECT_USAGE | ID_REMAP_SKIP_OVERRIDE_LIBRARY);
        }
      }
    }
    FOREACH_MAIN_ID_END;
  }

  return local_id;
}

/**
 * Create overridden local copies of all tagged data-blocks in given Main.
 *
 * \note Set `id->newid` of overridden libs with newly created overrides,
 * caller is responsible to clean those pointers before/after usage as needed.
 *
 * \note By default, it will only remap newly created local overriding data-blocks between
 * themselves, to avoid 'enforcing' those overrides into all other usages of the linked data in
 * main. You can add more local IDs to be remapped to use new overriding ones by setting their
 * LIB_TAG_DOIT tag.
 *
 * \return \a true on success, \a false otherwise.
 */
bool BKE_lib_override_library_create_from_tag(Main *bmain)
{
  ID *reference_id;
  bool success = true;

  ListBase todo_ids = {NULL};
  LinkData *todo_id_iter;

  /* Get all IDs we want to override. */
  FOREACH_MAIN_ID_BEGIN (bmain, reference_id) {
    if ((reference_id->tag & LIB_TAG_DOIT) != 0 && reference_id->lib != NULL &&
        BKE_idtype_idcode_is_linkable(GS(reference_id->name))) {
      todo_id_iter = MEM_callocN(sizeof(*todo_id_iter), __func__);
      todo_id_iter->data = reference_id;
      BLI_addtail(&todo_ids, todo_id_iter);
    }
  }
  FOREACH_MAIN_ID_END;

  /* Override the IDs. */
  for (todo_id_iter = todo_ids.first; todo_id_iter != NULL; todo_id_iter = todo_id_iter->next) {
    reference_id = todo_id_iter->data;
    if (reference_id->newid == NULL) {
      /* If `newid` is already set, assume it has been handled by calling code.
       * Only current use case: re-using proxy ID when converting to liboverride. */
      if ((reference_id->newid = lib_override_library_create_from(bmain, reference_id)) == NULL) {
        success = false;
        break;
      }
    }
    /* We also tag the new IDs so that in next step we can remap their pointers too. */
    reference_id->newid->tag |= LIB_TAG_DOIT;

    Key *reference_key;
    if ((reference_key = BKE_key_from_id(reference_id)) != NULL) {
      reference_key->id.tag |= LIB_TAG_DOIT;

      Key *local_key = BKE_key_from_id(reference_id->newid);
      BLI_assert(local_key != NULL);
      reference_key->id.newid = &local_key->id;
      /* We also tag the new IDs so that in next step we can remap their pointers too. */
      local_key->id.tag |= LIB_TAG_DOIT;
    }
  }

  /* Only remap new local ID's pointers, we don't want to force our new overrides onto our whole
   * existing linked IDs usages. */
  if (success) {
    for (todo_id_iter = todo_ids.first; todo_id_iter != NULL; todo_id_iter = todo_id_iter->next) {
      ID *other_id;
      reference_id = todo_id_iter->data;
      ID *local_id = reference_id->newid;

      if (local_id == NULL) {
        continue;
      }

      Key *reference_key, *local_key = NULL;
      if ((reference_key = BKE_key_from_id(reference_id)) != NULL) {
        local_key = BKE_key_from_id(reference_id->newid);
        BLI_assert(local_key != NULL);
      }

      /* Still checking the whole Main, that way we can tag other local IDs as needing to be
       * remapped to use newly created overriding IDs, if needed. */
      FOREACH_MAIN_ID_BEGIN (bmain, other_id) {
        if ((other_id->tag & LIB_TAG_DOIT) != 0 && other_id->lib == NULL) {
          /* Note that using ID_REMAP_SKIP_INDIRECT_USAGE below is superfluous, as we only remap
           * local IDs usages anyway. */
          BKE_libblock_relink_ex(bmain,
                                 other_id,
                                 reference_id,
                                 local_id,
                                 ID_REMAP_SKIP_INDIRECT_USAGE | ID_REMAP_SKIP_OVERRIDE_LIBRARY);
          if (reference_key != NULL) {
            BKE_libblock_relink_ex(bmain,
                                   other_id,
                                   &reference_key->id,
                                   &local_key->id,
                                   ID_REMAP_SKIP_INDIRECT_USAGE | ID_REMAP_SKIP_OVERRIDE_LIBRARY);
          }
        }
      }
      FOREACH_MAIN_ID_END;
    }
  }
  else {
    /* We need to cleanup potentially already created data. */
    for (todo_id_iter = todo_ids.first; todo_id_iter != NULL; todo_id_iter = todo_id_iter->next) {
      reference_id = todo_id_iter->data;
      BKE_id_delete(bmain, reference_id->newid);
      reference_id->newid = NULL;
    }
  }

  BLI_freelistN(&todo_ids);

  return success;
}

typedef struct LibOverrideGroupTagData {
  ID *id_root;
  uint tag;
  uint missing_tag;
} LibOverrideGroupTagData;

static int lib_override_linked_group_tag_cb(LibraryIDLinkCallbackData *cb_data)
{
  if (cb_data->cb_flag & (IDWALK_CB_EMBEDDED | IDWALK_CB_LOOPBACK)) {
    return IDWALK_RET_STOP_RECURSION;
  }

  LibOverrideGroupTagData *data = cb_data->user_data;
  const uint tag = data->tag;
  const uint missing_tag = data->missing_tag;

  ID *id_root = data->id_root;
  Library *library_root = id_root->lib;
  ID *id = *cb_data->id_pointer;
  ID *id_owner = cb_data->id_owner;

  BLI_assert(id_owner == cb_data->id_self);

  if (ELEM(id, NULL, id_owner)) {
    return IDWALK_RET_NOP;
  }

  BLI_assert(id_owner->lib == library_root);

  if (*(uint *)&id->tag & (tag | missing_tag)) {
    /* Already processed and tagged, nothing else to do here. */
    return IDWALK_RET_STOP_RECURSION;
  }

  if (id->lib != library_root) {
    /* We do not override data-blocks from other libraries, nor do we process them. */
    return IDWALK_RET_STOP_RECURSION;
  }

  /* We tag all collections and objects for override. And we also tag all other data-blocks which
   * would use one of those.
   * Note: missing IDs (aka placeholders) are never overridden. */
  if (ELEM(GS(id->name), ID_OB, ID_GR)) {
    if ((id->tag & LIB_TAG_MISSING)) {
      id->tag |= missing_tag;
    }
    else {
      id->tag |= tag;
    }
  }

  return IDWALK_RET_NOP;
}

/* Tag all IDs in dependency relationships whithin an override hierarchy/group.
 *
 * Note: this is typically called to complete `lib_override_linked_group_tag()`.
 * Note: BMain's relations mapping won't be valid anymore after that call.
 */
static bool lib_override_hierarchy_dependencies_recursive_tag(Main *bmain,
                                                              ID *id,
                                                              const uint tag,
                                                              const uint missing_tag)
{
  void **entry_vp = BLI_ghash_lookup_p(bmain->relations->id_user_to_used, id);
  if (entry_vp == NULL) {
    /* Already processed. */
    return (*(uint *)&id->tag & tag) != 0;
  }

  /* This way we won't process again that ID, should we encounter it again through another
   * relationship hierarchy.
   * Note that this does not free any memory from relations, so we can still use the entries.
   */
  BKE_main_relations_ID_remove(bmain, id);

  for (MainIDRelationsEntry *entry = *entry_vp; entry != NULL; entry = entry->next) {
    if ((entry->usage_flag & IDWALK_CB_LOOPBACK) != 0) {
      /* Never consider 'loop back' relationships ('from', 'parents', 'owner' etc. pointers) as
       * actual dependencies. */
      continue;
    }
    /* We only consider IDs from the same library. */
    if (entry->id_pointer != NULL && (*entry->id_pointer)->lib == id->lib) {
      if (lib_override_hierarchy_dependencies_recursive_tag(
              bmain, *entry->id_pointer, tag, missing_tag)) {
        id->tag |= tag;
      }
    }
  }

  return (*(uint *)&id->tag & tag) != 0;
}

/* This will tag at least all 'boundary' linked IDs for a potential override group.
 *
 * Note that you will then need to call `lib_override_hierarchy_dependencies_recursive_tag` to
 * complete tagging of all dependencies whitin theoverride group.
 *
 * We currently only consider Collections and Objects (that are not used as bone shapes) as valid
 * boundary IDs to define an override group.
 */
static void lib_override_linked_group_tag(Main *bmain,
                                          ID *id,
                                          const uint tag,
                                          const uint missing_tag)
{
  BKE_main_relations_create(bmain, 0);

  if (ELEM(GS(id->name), ID_OB, ID_GR)) {
    LibOverrideGroupTagData data = {.id_root = id, .tag = tag, .missing_tag = missing_tag};
    /* Tag all collections and objects. */
    BKE_library_foreach_ID_link(
        bmain, id, lib_override_linked_group_tag_cb, &data, IDWALK_READONLY | IDWALK_RECURSE);

    /* Then, we remove (untag) bone shape objects, you shall never want to directly/explicitely
     * override those. */
    LISTBASE_FOREACH (Object *, ob, &bmain->objects) {
      if (ob->type == OB_ARMATURE && ob->pose != NULL && (ob->id.tag & tag)) {
        for (bPoseChannel *pchan = ob->pose->chanbase.first; pchan != NULL; pchan = pchan->next) {
          if (pchan->custom != NULL) {
            pchan->custom->id.tag &= ~(tag | missing_tag);
          }
        }
      }
    }
  }

  lib_override_hierarchy_dependencies_recursive_tag(bmain, id, tag, missing_tag);

  BKE_main_relations_free(bmain);
}

static int lib_override_local_group_tag_cb(LibraryIDLinkCallbackData *cb_data)
{
  if (cb_data->cb_flag &
      (IDWALK_CB_EMBEDDED | IDWALK_CB_LOOPBACK | IDWALK_CB_OVERRIDE_LIBRARY_REFERENCE)) {
    return IDWALK_RET_STOP_RECURSION;
  }

  LibOverrideGroupTagData *data = cb_data->user_data;
  const uint tag = data->tag;
  const uint missing_tag = data->missing_tag;

  ID *id_root = data->id_root;
  Library *library_reference_root = id_root->override_library->reference->lib;
  ID *id = *cb_data->id_pointer;
  ID *id_owner = cb_data->id_owner;

  BLI_assert(id_owner == cb_data->id_self);

  if (ELEM(id, NULL, id_owner)) {
    return IDWALK_RET_NOP;
  }

  if (*(uint *)&id->tag & (tag | missing_tag)) {
    /* Already processed and tagged, nothing else to do here. */
    return IDWALK_RET_STOP_RECURSION;
  }

  if (!ID_IS_OVERRIDE_LIBRARY(id) || ID_IS_LINKED(id)) {
    /* Fully local, or linked ID, those are never part of a local override group. */
    return IDWALK_RET_STOP_RECURSION;
  }

  /* NOTE: Since we rejected embedded data too at the beginning of this function, id should only be
   * a real override now.
   *
   * However, our usual trouble maker, Key, is not considered as an embedded ID currently, yet it
   * is never a real override either. Enjoy. */
  if (!ID_IS_OVERRIDE_LIBRARY_REAL(id)) {
    return IDWALK_RET_NOP;
  }

  if (id->override_library->reference->lib != library_reference_root) {
    /* We do not override data-blocks from other libraries, nor do we process them. */
    return IDWALK_RET_STOP_RECURSION;
  }

  if (id->override_library->reference->tag & LIB_TAG_MISSING) {
    id->tag |= missing_tag;
  }
  else {
    id->tag |= tag;
  }

  return IDWALK_RET_NOP;
}

/* This will tag at least all 'boundary' linked IDs for a potential override group.
 *
 * Note that you will then need to call `lib_override_hierarchy_dependencies_recursive_tag` to
 * complete tagging of all dependencies whitin theoverride group.
 *
 * We currently only consider Collections and Objects (that are not used as bone shapes) as valid
 * boundary IDs to define an override group.
 */
static void lib_override_local_group_tag(Main *bmain,
                                         ID *id,
                                         const uint tag,
                                         const uint missing_tag)
{
  LibOverrideGroupTagData data = {.id_root = id, .tag = tag, .missing_tag = missing_tag};
  /* Tag all local overrides in id_root's group. */
  BKE_library_foreach_ID_link(
      bmain, id, lib_override_local_group_tag_cb, &data, IDWALK_READONLY | IDWALK_RECURSE);
}

static bool lib_override_library_create_do(Main *bmain, ID *id_root)
{
  id_root->tag |= LIB_TAG_DOIT;

  BKE_main_relations_create(bmain, 0);

  lib_override_linked_group_tag(bmain, id_root, LIB_TAG_DOIT, LIB_TAG_MISSING);
  lib_override_hierarchy_dependencies_recursive_tag(bmain, id_root, LIB_TAG_DOIT, LIB_TAG_MISSING);

  return BKE_lib_override_library_create_from_tag(bmain);
}

static void lib_override_library_create_post_process(
    Main *bmain, Scene *scene, ViewLayer *view_layer, ID *id_root, ID *id_reference)
{
  BKE_main_collection_sync(bmain);

  switch (GS(id_root->name)) {
    case ID_GR: {
      Object *ob_reference = id_reference != NULL && GS(id_reference->name) == ID_OB ?
                                 (Object *)id_reference :
                                 NULL;
      Collection *collection_new = ((Collection *)id_root->newid);
      if (ob_reference != NULL) {
        BKE_collection_add_from_object(bmain, scene, ob_reference, collection_new);
      }
      else if (id_reference != NULL) {
        BKE_collection_add_from_collection(
            bmain, scene, ((Collection *)id_reference), collection_new);
      }
      else {
        BKE_collection_add_from_collection(bmain, scene, ((Collection *)id_root), collection_new);
      }

      FOREACH_COLLECTION_OBJECT_RECURSIVE_BEGIN (collection_new, ob_new) {
        if (ob_new != NULL && ob_new->id.override_library != NULL) {
          if (ob_reference != NULL) {
            Base *base;
            if ((base = BKE_view_layer_base_find(view_layer, ob_new)) == NULL) {
              BKE_collection_object_add_from(bmain, scene, ob_reference, ob_new);
              base = BKE_view_layer_base_find(view_layer, ob_new);
              DEG_id_tag_update_ex(bmain, &ob_new->id, ID_RECALC_TRANSFORM | ID_RECALC_BASE_FLAGS);
            }

            if (ob_new == (Object *)ob_reference->id.newid) {
              /* TODO: is setting active needed? */
              BKE_view_layer_base_select_and_set_active(view_layer, base);
            }
          }
          else if (BKE_view_layer_base_find(view_layer, ob_new) == NULL) {
            BKE_collection_object_add(bmain, collection_new, ob_new);
            DEG_id_tag_update_ex(bmain, &ob_new->id, ID_RECALC_TRANSFORM | ID_RECALC_BASE_FLAGS);
          }
        }
      }
      FOREACH_COLLECTION_OBJECT_RECURSIVE_END;
      break;
    }
    case ID_OB: {
      BKE_collection_object_add_from(bmain, scene, (Object *)id_root, ((Object *)id_root->newid));
      break;
    }
    default:
      break;
  }

  /* We need to ensure all new overrides of objects are properly instantiated. */
  LISTBASE_FOREACH (Object *, ob, &bmain->objects) {
    Object *ob_new = (Object *)ob->id.newid;
    if (ob_new != NULL) {
      BLI_assert(ob_new->id.override_library != NULL &&
                 ob_new->id.override_library->reference == &ob->id);

      Collection *default_instantiating_collection = NULL;
      if (BKE_view_layer_base_find(view_layer, ob_new) == NULL) {
        if (default_instantiating_collection == NULL) {
          switch (GS(id_root->name)) {
            case ID_GR: {
              default_instantiating_collection = BKE_collection_add(
                  bmain, (Collection *)id_root, "OVERRIDE_HIDDEN");
              /* Hide the collection from viewport and render. */
              default_instantiating_collection->flag |= COLLECTION_RESTRICT_VIEWPORT |
                                                        COLLECTION_RESTRICT_RENDER;
              break;
            }
            case ID_OB: {
              /* Add the other objects to one of the collections instantiating the
               * root object, or scene's master collection if none found. */
              Object *ob_root = (Object *)id_root;
              LISTBASE_FOREACH (Collection *, collection, &bmain->collections) {
                if (BKE_collection_has_object(collection, ob_root) &&
                    BKE_view_layer_has_collection(view_layer, collection) &&
                    !ID_IS_LINKED(collection) && !ID_IS_OVERRIDE_LIBRARY(collection)) {
                  default_instantiating_collection = collection;
                }
              }
              if (default_instantiating_collection == NULL) {
                default_instantiating_collection = scene->master_collection;
              }
              break;
            }
            default:
              BLI_assert(0);
          }
        }

        BKE_collection_object_add(bmain, default_instantiating_collection, ob_new);
        DEG_id_tag_update_ex(bmain, &ob_new->id, ID_RECALC_TRANSFORM | ID_RECALC_BASE_FLAGS);
      }
    }
  }
}

/**
 * Advanced 'smart' function to create fully functional overrides.
 *
 * \note Currently it only does special things if given \a id_root is an object of collection, more
 * specific behaviors may be added in the future for other ID types.
 *
 * \note It will override all IDs tagged with \a LIB_TAG_DOIT, and it does not clear that tag at
 * its beginning, so caller code can add extra data-blocks to be overridden as well.
 *
 * \param id_root: The root ID to create an override from.
 * \param id_reference: Some reference ID used to do some post-processing after overrides have been
 * created, may be NULL. Typically, the Empty object instantiating the linked collection we
 * override, currently.
 * \return true if override was successfully created.
 */
bool BKE_lib_override_library_create(
    Main *bmain, Scene *scene, ViewLayer *view_layer, ID *id_root, ID *id_reference)
{
  const bool success = lib_override_library_create_do(bmain, id_root);

  if (!success) {
    return success;
  }

  lib_override_library_create_post_process(bmain, scene, view_layer, id_root, id_reference);

  /* Cleanup. */
  BKE_main_id_clear_newpoins(bmain);
  BKE_main_id_tag_all(bmain, LIB_TAG_DOIT, false);

  return success;
}

/**
 * Convert a given proxy object into a library override.
 *
 * \note This is a thin wrapper around \a BKE_lib_override_library_create, only extra work is to
 * actually convert the proxy itself into an override first.
 *
 * \return true if override was successfully created.
 */
bool BKE_lib_override_library_proxy_convert(Main *bmain,
                                            Scene *scene,
                                            ViewLayer *view_layer,
                                            Object *ob_proxy)
{
  /* `proxy_group`, if defined, is the empty instantiating the collection from which the proxy is
   * coming. */
  Object *ob_proxy_group = ob_proxy->proxy_group;
  const bool is_override_instancing_object = ob_proxy_group != NULL;
  ID *id_root = is_override_instancing_object ? &ob_proxy_group->instance_collection->id :
                                                &ob_proxy->proxy->id;
  ID *id_reference = is_override_instancing_object ? &ob_proxy_group->id : &ob_proxy->id;

  /* In some cases the instance collection of a proxy object may be local (see e.g. T83875). Not
   * sure this is a valid state, but for now just abort the overriding process. */
  if (!ID_IS_OVERRIDABLE_LIBRARY(id_root)) {
    return false;
  }

  /* We manually convert the proxy object into a library override, further override handling will
   * then be handled by `BKE_lib_override_library_create()` just as for a regular override
   * creation.
   */
  ob_proxy->proxy->id.tag |= LIB_TAG_DOIT;
  ob_proxy->proxy->id.newid = &ob_proxy->id;
  BKE_lib_override_library_init(&ob_proxy->id, &ob_proxy->proxy->id);

  ob_proxy->proxy->proxy_from = NULL;
  ob_proxy->proxy = ob_proxy->proxy_group = NULL;

  DEG_id_tag_update(&ob_proxy->id, ID_RECALC_COPY_ON_WRITE);

  return BKE_lib_override_library_create(bmain, scene, view_layer, id_root, id_reference);
}

/**
 * Advanced 'smart' function to resync, re-create fully functional overrides up-to-date with linked
 * data, from an existing override hierarchy.
 *
 * \param id_root: The root liboverride ID to resync from.
 * \return true if override was successfully resynced.
 */
bool BKE_lib_override_library_resync(Main *bmain, Scene *scene, ViewLayer *view_layer, ID *id_root)
{
  BLI_assert(ID_IS_OVERRIDE_LIBRARY_REAL(id_root));

  id_root->tag |= LIB_TAG_DOIT;
  ID *id_root_reference = id_root->override_library->reference;

  lib_override_local_group_tag(bmain, id_root, LIB_TAG_DOIT, LIB_TAG_MISSING);

  lib_override_linked_group_tag(bmain, id_root_reference, LIB_TAG_DOIT, LIB_TAG_MISSING);

  /* Make a mapping 'linked reference IDs' -> 'Local override IDs' of existing overrides. */
  GHash *linkedref_to_old_override = BLI_ghash_new(
      BLI_ghashutil_ptrhash, BLI_ghashutil_ptrcmp, __func__);
  ID *id;
  FOREACH_MAIN_ID_BEGIN (bmain, id) {
    if (id->tag & LIB_TAG_DOIT && ID_IS_OVERRIDE_LIBRARY_REAL(id)) {
      /* While this should not happen in typical cases (and won't be properly supported here), user
       * is free to do all kind of very bad things, including having different local overrides of a
       * same linked ID in a same hierarchy. */
      if (!BLI_ghash_haskey(linkedref_to_old_override, id->override_library->reference)) {
        BLI_ghash_insert(linkedref_to_old_override, id->override_library->reference, id);
        BLI_assert(id->override_library->reference->tag & LIB_TAG_DOIT);
      }
    }
  }
  FOREACH_MAIN_ID_END;

  /* Make new override from linked data. */
  /* Note that this call also remaps all pointers of tagged IDs from old override IDs to new
   * override IDs (including within the old overrides themselves, since those are tagged too
   * above). */
  const bool success = BKE_lib_override_library_create_from_tag(bmain);

  if (!success) {
    return success;
  }

  ListBase *lb;
  FOREACH_MAIN_LISTBASE_BEGIN (bmain, lb) {
    FOREACH_MAIN_LISTBASE_ID_BEGIN (lb, id) {
      if (id->tag & LIB_TAG_DOIT && id->newid != NULL && ID_IS_LINKED(id)) {
        ID *id_override_new = id->newid;
        ID *id_override_old = BLI_ghash_lookup(linkedref_to_old_override, id);

        if (id_override_old != NULL) {
          /* Swap the names between old override ID and new one. */
          char id_name_buf[MAX_ID_NAME];
          memcpy(id_name_buf, id_override_old->name, sizeof(id_name_buf));
          memcpy(id_override_old->name, id_override_new->name, sizeof(id_override_old->name));
          memcpy(id_override_new->name, id_name_buf, sizeof(id_override_new->name));
          /* Note that this is a very efficient way to keep BMain IDs ordered as expected after
           * swapping their names.
           * However, one has to be very careful with this when iterating over the listbase at the
           * same time. Here it works because we only execute this code when we are in the linked
           * IDs, which are always *after* all local ones, and we only affect local IDs. */
          BLI_listbase_swaplinks(lb, id_override_old, id_override_new);

          /* Remap the whole local IDs to use the new override. */
          BKE_libblock_remap(
              bmain, id_override_old, id_override_new, ID_REMAP_SKIP_INDIRECT_USAGE);

          /* Copy over overrides rules from old override ID to new one. */
          BLI_duplicatelist(&id_override_new->override_library->properties,
                            &id_override_old->override_library->properties);
          for (IDOverrideLibraryProperty *
                   op_new = id_override_new->override_library->properties.first,
                  *op_old = id_override_old->override_library->properties.first;
               op_new;
               op_new = op_new->next, op_old = op_old->next) {
            lib_override_library_property_copy(op_new, op_old);
          }
        }
      }
    }
    FOREACH_MAIN_LISTBASE_ID_END;
  }
  FOREACH_MAIN_LISTBASE_END;

  /* We need to apply override rules in a separate loop, after all ID pointers have been properly
   * remapped, and all new local override IDs have gotten their proper original names, otherwise
   * override operations based on those ID names would fail. */
  FOREACH_MAIN_ID_BEGIN (bmain, id) {
    if (id->tag & LIB_TAG_DOIT && id->newid != NULL && ID_IS_LINKED(id)) {
      ID *id_override_new = id->newid;
      ID *id_override_old = BLI_ghash_lookup(linkedref_to_old_override, id);

      if (id_override_old != NULL) {
        /* Apply rules on new override ID using old one as 'source' data. */
        /* Note that since we already remapped ID pointers in old override IDs to new ones, we
         * can also apply ID pointer override rules safely here. */
        PointerRNA rnaptr_src, rnaptr_dst;
        RNA_id_pointer_create(id_override_old, &rnaptr_src);
        RNA_id_pointer_create(id_override_new, &rnaptr_dst);

        /* We remove any operation tagged with `IDOVERRIDE_LIBRARY_FLAG_IDPOINTER_MATCH_REFERENCE`,
         * that way the potentially new pointer will be properly kept, when old one is still valid
         * too (typical case: assigning new ID to some usage, while old one remains used elsewhere
         * in the override hierarchy). */
        LISTBASE_FOREACH_MUTABLE (
            IDOverrideLibraryProperty *, op, &id_override_new->override_library->properties) {
          LISTBASE_FOREACH_MUTABLE (IDOverrideLibraryPropertyOperation *, opop, &op->operations) {
            if (opop->flag & IDOVERRIDE_LIBRARY_FLAG_IDPOINTER_MATCH_REFERENCE) {
              lib_override_library_property_operation_clear(opop);
              printf("Clearing  shallow ID pointer override '%s.%s'.\n",
                     id_override_old->name,
                     op->rna_path);
              BLI_freelinkN(&op->operations, opop);
            }
          }
          if (BLI_listbase_is_empty(&op->operations)) {
            BKE_lib_override_library_property_delete(id_override_new->override_library, op);
          }
        }

        RNA_struct_override_apply(
            bmain, &rnaptr_dst, &rnaptr_src, NULL, id_override_new->override_library);
      }
    }
  }
  FOREACH_MAIN_ID_END;

  /* Delete old override IDs.
   * Note that we have to use tagged group deletion here, since ID deletion also uses LIB_TAG_DOIT.
   * This improves performances anyway, so everything is fine. */
  FOREACH_MAIN_ID_BEGIN (bmain, id) {
    if (id->tag & LIB_TAG_DOIT) {
      /* Note that this works because linked IDs are always after local ones (including overrides),
       * so we will only ever tag an old override ID after we have already checked it in this loop,
       * hence we cannot untag it later. */
      if (id->newid != NULL && ID_IS_LINKED(id)) {
        ID *id_override_old = BLI_ghash_lookup(linkedref_to_old_override, id);

        if (id_override_old != NULL) {
          id->newid->tag &= ~LIB_TAG_DOIT;
          id_override_old->tag |= LIB_TAG_DOIT;
        }
      }
      id->tag &= ~LIB_TAG_DOIT;
    }
    /* Also cleanup old overrides that went missing in new linked data. */
    else if (id->tag & LIB_TAG_MISSING && !ID_IS_LINKED(id)) {
      BLI_assert(ID_IS_OVERRIDE_LIBRARY(id));
      id->tag |= LIB_TAG_DOIT;
      id->tag &= ~LIB_TAG_MISSING;
    }
  }
  FOREACH_MAIN_ID_END;
  BKE_id_multi_tagged_delete(bmain);

  /* At this point, `id_root` has very likely been deleted, we need to update it to its new
   * version.
   */
  id_root = id_root_reference->newid;

  /* Essentially ensures that potentially new overrides of new objects will be instantiated. */
  /* Note: Here 'reference' collection and 'newly added' collection are the same, which is fine
   * since we already relinked old root override collection to new resync'ed one above. So this
   * call is not expected to instantiate this new resync'ed collection anywhere, just to ensure
   * that we do not have any stray objects. */
  lib_override_library_create_post_process(bmain, scene, view_layer, id_root_reference, id_root);

  /* Cleanup. */
  BLI_ghash_free(linkedref_to_old_override, NULL, NULL);

  BKE_main_id_clear_newpoins(bmain);
  BKE_main_id_tag_all(bmain, LIB_TAG_DOIT, false); /* That one should not be needed in fact. */

  return success;
}

/**
 * Advanced 'smart' function to delete library overrides (including their existing override
 * hierarchy) and remap their usages to their linked reference IDs.
 *
 * \note All IDs tagged with `LIB_TAG_DOIT` will be deleted.
 *
 * \param id_root: The root liboverride ID to delete.
 */
void BKE_lib_override_library_delete(Main *bmain, ID *id_root)
{
  BLI_assert(ID_IS_OVERRIDE_LIBRARY_REAL(id_root));

  /* Tag all collections and objects, as well as other IDs using them. */
  id_root->tag |= LIB_TAG_DOIT;

  /* Tag all library overrides in the chains of dependencies from the given root one. */
  lib_override_local_group_tag(bmain, id_root, LIB_TAG_DOIT, LIB_TAG_DOIT);

  ID *id;
  FOREACH_MAIN_ID_BEGIN (bmain, id) {
    if (id->tag & LIB_TAG_DOIT) {
      if (ID_IS_OVERRIDE_LIBRARY_REAL(id)) {
        ID *id_override_reference = id->override_library->reference;

        /* Remap the whole local IDs to use the linked data. */
        BKE_libblock_remap(bmain, id, id_override_reference, ID_REMAP_SKIP_INDIRECT_USAGE);
      }
    }
  }
  FOREACH_MAIN_ID_END;

  /* Delete the override IDs. */
  BKE_id_multi_tagged_delete(bmain);

  /* Should not actually be needed here. */
  BKE_main_id_tag_all(bmain, LIB_TAG_DOIT, false);
}

BLI_INLINE IDOverrideLibraryRuntime *override_library_rna_path_runtime_ensure(
    IDOverrideLibrary *override)
{
  if (override->runtime == NULL) {
    override->runtime = MEM_callocN(sizeof(*override->runtime), __func__);
  }
  return override->runtime;
}

/* We only build override GHash on request. */
BLI_INLINE GHash *override_library_rna_path_mapping_ensure(IDOverrideLibrary *override)
{
  IDOverrideLibraryRuntime *override_runtime = override_library_rna_path_runtime_ensure(override);
  if (override_runtime->rna_path_to_override_properties == NULL) {
    override_runtime->rna_path_to_override_properties = BLI_ghash_new(
        BLI_ghashutil_strhash_p_murmur, BLI_ghashutil_strcmp, __func__);
    for (IDOverrideLibraryProperty *op = override->properties.first; op != NULL; op = op->next) {
      BLI_ghash_insert(override_runtime->rna_path_to_override_properties, op->rna_path, op);
    }
  }

  return override_runtime->rna_path_to_override_properties;
}

/**
 * Find override property from given RNA path, if it exists.
 */
IDOverrideLibraryProperty *BKE_lib_override_library_property_find(IDOverrideLibrary *override,
                                                                  const char *rna_path)
{
  GHash *override_runtime = override_library_rna_path_mapping_ensure(override);
  return BLI_ghash_lookup(override_runtime, rna_path);
}

/**
 * Find override property from given RNA path, or create it if it does not exist.
 */
IDOverrideLibraryProperty *BKE_lib_override_library_property_get(IDOverrideLibrary *override,
                                                                 const char *rna_path,
                                                                 bool *r_created)
{
  IDOverrideLibraryProperty *op = BKE_lib_override_library_property_find(override, rna_path);

  if (op == NULL) {
    op = MEM_callocN(sizeof(IDOverrideLibraryProperty), __func__);
    op->rna_path = BLI_strdup(rna_path);
    BLI_addtail(&override->properties, op);

    GHash *override_runtime = override_library_rna_path_mapping_ensure(override);
    BLI_ghash_insert(override_runtime, op->rna_path, op);

    if (r_created) {
      *r_created = true;
    }
  }
  else if (r_created) {
    *r_created = false;
  }

  return op;
}

void lib_override_library_property_copy(IDOverrideLibraryProperty *op_dst,
                                        IDOverrideLibraryProperty *op_src)
{
  op_dst->rna_path = BLI_strdup(op_src->rna_path);
  BLI_duplicatelist(&op_dst->operations, &op_src->operations);

  for (IDOverrideLibraryPropertyOperation *opop_dst = op_dst->operations.first,
                                          *opop_src = op_src->operations.first;
       opop_dst;
       opop_dst = opop_dst->next, opop_src = opop_src->next) {
    lib_override_library_property_operation_copy(opop_dst, opop_src);
  }
}

void lib_override_library_property_clear(IDOverrideLibraryProperty *op)
{
  BLI_assert(op->rna_path != NULL);

  MEM_freeN(op->rna_path);

  LISTBASE_FOREACH (IDOverrideLibraryPropertyOperation *, opop, &op->operations) {
    lib_override_library_property_operation_clear(opop);
  }
  BLI_freelistN(&op->operations);
}

/**
 * Remove and free given \a override_property from given ID \a override.
 */
void BKE_lib_override_library_property_delete(IDOverrideLibrary *override,
                                              IDOverrideLibraryProperty *override_property)
{
  if (!ELEM(NULL, override->runtime, override->runtime->rna_path_to_override_properties)) {
    BLI_ghash_remove(override->runtime->rna_path_to_override_properties,
                     override_property->rna_path,
                     NULL,
                     NULL);
  }
  lib_override_library_property_clear(override_property);
  BLI_freelinkN(&override->properties, override_property);
}

/**
 * Find override property operation from given sub-item(s), if it exists.
 */
IDOverrideLibraryPropertyOperation *BKE_lib_override_library_property_operation_find(
    IDOverrideLibraryProperty *override_property,
    const char *subitem_refname,
    const char *subitem_locname,
    const int subitem_refindex,
    const int subitem_locindex,
    const bool strict,
    bool *r_strict)
{
  IDOverrideLibraryPropertyOperation *opop;
  const int subitem_defindex = -1;

  if (r_strict) {
    *r_strict = true;
  }

  if (subitem_locname != NULL) {
    opop = BLI_findstring_ptr(&override_property->operations,
                              subitem_locname,
                              offsetof(IDOverrideLibraryPropertyOperation, subitem_local_name));

    if (opop == NULL) {
      return NULL;
    }

    if (subitem_refname == NULL || opop->subitem_reference_name == NULL) {
      return subitem_refname == opop->subitem_reference_name ? opop : NULL;
    }
    return (subitem_refname != NULL && opop->subitem_reference_name != NULL &&
            STREQ(subitem_refname, opop->subitem_reference_name)) ?
               opop :
               NULL;
  }

  if (subitem_refname != NULL) {
    opop = BLI_findstring_ptr(
        &override_property->operations,
        subitem_refname,
        offsetof(IDOverrideLibraryPropertyOperation, subitem_reference_name));

    if (opop == NULL) {
      return NULL;
    }

    if (subitem_locname == NULL || opop->subitem_local_name == NULL) {
      return subitem_locname == opop->subitem_local_name ? opop : NULL;
    }
    return (subitem_locname != NULL && opop->subitem_local_name != NULL &&
            STREQ(subitem_locname, opop->subitem_local_name)) ?
               opop :
               NULL;
  }

  if ((opop = BLI_listbase_bytes_find(
           &override_property->operations,
           &subitem_locindex,
           sizeof(subitem_locindex),
           offsetof(IDOverrideLibraryPropertyOperation, subitem_local_index)))) {
    return ELEM(subitem_refindex, -1, opop->subitem_reference_index) ? opop : NULL;
  }

  if ((opop = BLI_listbase_bytes_find(
           &override_property->operations,
           &subitem_refindex,
           sizeof(subitem_refindex),
           offsetof(IDOverrideLibraryPropertyOperation, subitem_reference_index)))) {
    return ELEM(subitem_locindex, -1, opop->subitem_local_index) ? opop : NULL;
  }

  /* `index == -1` means all indices, that is a valid fallback in case we requested specific index.
   */
  if (!strict && (subitem_locindex != subitem_defindex) &&
      (opop = BLI_listbase_bytes_find(
           &override_property->operations,
           &subitem_defindex,
           sizeof(subitem_defindex),
           offsetof(IDOverrideLibraryPropertyOperation, subitem_local_index)))) {
    if (r_strict) {
      *r_strict = false;
    }
    return opop;
  }

  return NULL;
}

/**
 * Find override property operation from given sub-item(s), or create it if it does not exist.
 */
IDOverrideLibraryPropertyOperation *BKE_lib_override_library_property_operation_get(
    IDOverrideLibraryProperty *override_property,
    const short operation,
    const char *subitem_refname,
    const char *subitem_locname,
    const int subitem_refindex,
    const int subitem_locindex,
    const bool strict,
    bool *r_strict,
    bool *r_created)
{
  IDOverrideLibraryPropertyOperation *opop = BKE_lib_override_library_property_operation_find(
      override_property,
      subitem_refname,
      subitem_locname,
      subitem_refindex,
      subitem_locindex,
      strict,
      r_strict);

  if (opop == NULL) {
    opop = MEM_callocN(sizeof(IDOverrideLibraryPropertyOperation), __func__);
    opop->operation = operation;
    if (subitem_locname) {
      opop->subitem_local_name = BLI_strdup(subitem_locname);
    }
    if (subitem_refname) {
      opop->subitem_reference_name = BLI_strdup(subitem_refname);
    }
    opop->subitem_local_index = subitem_locindex;
    opop->subitem_reference_index = subitem_refindex;

    BLI_addtail(&override_property->operations, opop);

    if (r_created) {
      *r_created = true;
    }
  }
  else if (r_created) {
    *r_created = false;
  }

  return opop;
}

void lib_override_library_property_operation_copy(IDOverrideLibraryPropertyOperation *opop_dst,
                                                  IDOverrideLibraryPropertyOperation *opop_src)
{
  if (opop_src->subitem_reference_name) {
    opop_dst->subitem_reference_name = BLI_strdup(opop_src->subitem_reference_name);
  }
  if (opop_src->subitem_local_name) {
    opop_dst->subitem_local_name = BLI_strdup(opop_src->subitem_local_name);
  }
}

void lib_override_library_property_operation_clear(IDOverrideLibraryPropertyOperation *opop)
{
  if (opop->subitem_reference_name) {
    MEM_freeN(opop->subitem_reference_name);
  }
  if (opop->subitem_local_name) {
    MEM_freeN(opop->subitem_local_name);
  }
}

/**
 * Remove and free given \a override_property_operation from given ID \a override_property.
 */
void BKE_lib_override_library_property_operation_delete(
    IDOverrideLibraryProperty *override_property,
    IDOverrideLibraryPropertyOperation *override_property_operation)
{
  lib_override_library_property_operation_clear(override_property_operation);
  BLI_freelinkN(&override_property->operations, override_property_operation);
}

/**
 * Validate that required data for a given operation are available.
 */
bool BKE_lib_override_library_property_operation_operands_validate(
    struct IDOverrideLibraryPropertyOperation *override_property_operation,
    struct PointerRNA *ptr_dst,
    struct PointerRNA *ptr_src,
    struct PointerRNA *ptr_storage,
    struct PropertyRNA *prop_dst,
    struct PropertyRNA *prop_src,
    struct PropertyRNA *prop_storage)
{
  switch (override_property_operation->operation) {
    case IDOVERRIDE_LIBRARY_OP_NOOP:
      return true;
    case IDOVERRIDE_LIBRARY_OP_ADD:
      ATTR_FALLTHROUGH;
    case IDOVERRIDE_LIBRARY_OP_SUBTRACT:
      ATTR_FALLTHROUGH;
    case IDOVERRIDE_LIBRARY_OP_MULTIPLY:
      if (ptr_storage == NULL || ptr_storage->data == NULL || prop_storage == NULL) {
        BLI_assert(!"Missing data to apply differential override operation.");
        return false;
      }
      ATTR_FALLTHROUGH;
    case IDOVERRIDE_LIBRARY_OP_INSERT_AFTER:
      ATTR_FALLTHROUGH;
    case IDOVERRIDE_LIBRARY_OP_INSERT_BEFORE:
      ATTR_FALLTHROUGH;
    case IDOVERRIDE_LIBRARY_OP_REPLACE:
      if ((ptr_dst == NULL || ptr_dst->data == NULL || prop_dst == NULL) ||
          (ptr_src == NULL || ptr_src->data == NULL || prop_src == NULL)) {
        BLI_assert(!"Missing data to apply override operation.");
        return false;
      }
  }

  return true;
}

/**
 * Check that status of local data-block is still valid against current reference one.
 *
 * It means that all overridable, but not overridden, properties' local values must be equal to
 * reference ones. Clears #LIB_TAG_OVERRIDE_OK if they do not.
 *
 * This is typically used to detect whether some property has been changed in local and a new
 * #IDOverrideProperty (of #IDOverridePropertyOperation) has to be added.
 *
 * \return true if status is OK, false otherwise. */
bool BKE_lib_override_library_status_check_local(Main *bmain, ID *local)
{
  BLI_assert(ID_IS_OVERRIDE_LIBRARY_REAL(local));

  ID *reference = local->override_library->reference;

  if (reference == NULL) {
    /* This is an override template, local status is always OK! */
    return true;
  }

  BLI_assert(GS(local->name) == GS(reference->name));

  if (GS(local->name) == ID_OB) {
    /* Our beloved pose's bone cross-data pointers. Usually, depsgraph evaluation would
     * ensure this is valid, but in some situations (like hidden collections etc.) this won't
     * be the case, so we need to take care of this ourselves. */
    Object *ob_local = (Object *)local;
    if (ob_local->type == OB_ARMATURE) {
      Object *ob_reference = (Object *)local->override_library->reference;
      BLI_assert(ob_local->data != NULL);
      BLI_assert(ob_reference->data != NULL);
      BKE_pose_ensure(bmain, ob_local, ob_local->data, true);
      BKE_pose_ensure(bmain, ob_reference, ob_reference->data, true);
    }
  }

  /* Note that reference is assumed always valid, caller has to ensure that itself. */

  PointerRNA rnaptr_local, rnaptr_reference;
  RNA_id_pointer_create(local, &rnaptr_local);
  RNA_id_pointer_create(reference, &rnaptr_reference);

  if (!RNA_struct_override_matches(bmain,
                                   &rnaptr_local,
                                   &rnaptr_reference,
                                   NULL,
                                   0,
                                   local->override_library,
                                   RNA_OVERRIDE_COMPARE_IGNORE_NON_OVERRIDABLE |
                                       RNA_OVERRIDE_COMPARE_IGNORE_OVERRIDDEN,
                                   NULL)) {
    local->tag &= ~LIB_TAG_OVERRIDE_LIBRARY_REFOK;
    return false;
  }

  return true;
}

/**
 * Check that status of reference data-block is still valid against current local one.
 *
 * It means that all non-overridden properties' local values must be equal to reference ones.
 * Clears LIB_TAG_OVERRIDE_OK if they do not.
 *
 * This is typically used to detect whether some reference has changed and local
 * needs to be updated against it.
 *
 * \return true if status is OK, false otherwise. */
bool BKE_lib_override_library_status_check_reference(Main *bmain, ID *local)
{
  BLI_assert(ID_IS_OVERRIDE_LIBRARY_REAL(local));

  ID *reference = local->override_library->reference;

  if (reference == NULL) {
    /* This is an override template, reference is virtual, so its status is always OK! */
    return true;
  }

  BLI_assert(GS(local->name) == GS(reference->name));

  if (reference->override_library && (reference->tag & LIB_TAG_OVERRIDE_LIBRARY_REFOK) == 0) {
    if (!BKE_lib_override_library_status_check_reference(bmain, reference)) {
      /* If reference is also an override of another data-block, and its status is not OK,
       * then this override is not OK either.
       * Note that this should only happen when reloading libraries. */
      local->tag &= ~LIB_TAG_OVERRIDE_LIBRARY_REFOK;
      return false;
    }
  }

  if (GS(local->name) == ID_OB) {
    /* Our beloved pose's bone cross-data pointers. Usually, depsgraph evaluation would
     * ensure this is valid, but in some situations (like hidden collections etc.) this won't
     * be the case, so we need to take care of this ourselves. */
    Object *ob_local = (Object *)local;
    if (ob_local->type == OB_ARMATURE) {
      Object *ob_reference = (Object *)local->override_library->reference;
      BLI_assert(ob_local->data != NULL);
      BLI_assert(ob_reference->data != NULL);
      BKE_pose_ensure(bmain, ob_local, ob_local->data, true);
      BKE_pose_ensure(bmain, ob_reference, ob_reference->data, true);
    }
  }

  PointerRNA rnaptr_local, rnaptr_reference;
  RNA_id_pointer_create(local, &rnaptr_local);
  RNA_id_pointer_create(reference, &rnaptr_reference);

  if (!RNA_struct_override_matches(bmain,
                                   &rnaptr_local,
                                   &rnaptr_reference,
                                   NULL,
                                   0,
                                   local->override_library,
                                   RNA_OVERRIDE_COMPARE_IGNORE_OVERRIDDEN,
                                   NULL)) {
    local->tag &= ~LIB_TAG_OVERRIDE_LIBRARY_REFOK;
    return false;
  }

  return true;
}

/**
 * Compare local and reference data-blocks and create new override operations as needed,
 * or reset to reference values if overriding is not allowed.
 *
 * \note Defining override operations is only mandatory before saving a `.blend` file on disk
 * (not for undo!).
 * Knowing that info at runtime is only useful for UI/UX feedback.
 *
 * \note This is by far the biggest operation (the more time-consuming) of the three so far,
 * since it has to go over all properties in depth (all overridable ones at least).
 * Generating differential values and applying overrides are much cheaper.
 *
 * \return true if a new overriding op was created, or some local data was reset. */
bool BKE_lib_override_library_operations_create(Main *bmain, ID *local)
{
  BLI_assert(local->override_library != NULL);
  const bool is_template = (local->override_library->reference == NULL);
  bool ret = false;

  if (!is_template) {
    /* Do not attempt to generate overriding rules from an empty place-holder generated by link
     * code when it cannot find the actual library/ID. Much better to keep the local data-block as
     * is in the file in that case, until broken lib is fixed. */
    if (ID_MISSING(local->override_library->reference)) {
      return ret;
    }

    if (GS(local->name) == ID_OB) {
      /* Our beloved pose's bone cross-data pointers. Usually, depsgraph evaluation would
       * ensure this is valid, but in some situations (like hidden collections etc.) this won't
       * be the case, so we need to take care of this ourselves. */
      Object *ob_local = (Object *)local;
      if (ob_local->type == OB_ARMATURE) {
        Object *ob_reference = (Object *)local->override_library->reference;
        BLI_assert(ob_local->data != NULL);
        BLI_assert(ob_reference->data != NULL);
        BKE_pose_ensure(bmain, ob_local, ob_local->data, true);
        BKE_pose_ensure(bmain, ob_reference, ob_reference->data, true);
      }
    }

    PointerRNA rnaptr_local, rnaptr_reference;
    RNA_id_pointer_create(local, &rnaptr_local);
    RNA_id_pointer_create(local->override_library->reference, &rnaptr_reference);

    eRNAOverrideMatchResult report_flags = 0;
    RNA_struct_override_matches(bmain,
                                &rnaptr_local,
                                &rnaptr_reference,
                                NULL,
                                0,
                                local->override_library,
                                RNA_OVERRIDE_COMPARE_CREATE | RNA_OVERRIDE_COMPARE_RESTORE,
                                &report_flags);
    if (report_flags & RNA_OVERRIDE_MATCH_RESULT_CREATED) {
      ret = true;
    }
#ifndef NDEBUG
    if (report_flags & RNA_OVERRIDE_MATCH_RESULT_RESTORED) {
      printf("We did restore some properties of %s from its reference.\n", local->name);
    }
    if (ret) {
      printf("We did generate library override rules for %s\n", local->name);
    }
    else {
      printf("No new library override rules for %s\n", local->name);
    }
#endif
  }
  return ret;
}

static void lib_override_library_operations_create_cb(TaskPool *__restrict pool, void *taskdata)
{
  Main *bmain = BLI_task_pool_user_data(pool);
  ID *id = taskdata;

  BKE_lib_override_library_operations_create(bmain, id);
}

/** Check all overrides from given \a bmain and create/update overriding operations as needed. */
void BKE_lib_override_library_main_operations_create(Main *bmain, const bool force_auto)
{
  ID *id;

#ifdef DEBUG_OVERRIDE_TIMEIT
  TIMEIT_START_AVERAGED(BKE_lib_override_library_main_operations_create);
#endif

  /* When force-auto is set, we also remove all unused existing override properties & operations.
   */
  if (force_auto) {
    BKE_lib_override_library_main_tag(bmain, IDOVERRIDE_LIBRARY_TAG_UNUSED, true);
  }

  /* Usual pose bones issue, need to be done outside of the threaded process or we may run into
   * concurrency issues here.
   * Note that calling #BKE_pose_ensure again in thread in
   * #BKE_lib_override_library_operations_create is not a problem then. */
  LISTBASE_FOREACH (Object *, ob, &bmain->objects) {
    if (ob->type == OB_ARMATURE) {
      BLI_assert(ob->data != NULL);
      BKE_pose_ensure(bmain, ob, ob->data, true);
    }
  }

  TaskPool *task_pool = BLI_task_pool_create(bmain, TASK_PRIORITY_HIGH);

  FOREACH_MAIN_ID_BEGIN (bmain, id) {
    if (ID_IS_OVERRIDE_LIBRARY_REAL(id) &&
        (force_auto || (id->tag & LIB_TAG_OVERRIDE_LIBRARY_AUTOREFRESH))) {
      /* Usual issue with pose, it's quiet rare but sometimes they may not be up to date when this
       * function is called. */
      if (GS(id->name) == ID_OB) {
        Object *ob = (Object *)id;
        if (ob->type == OB_ARMATURE) {
          BLI_assert(ob->data != NULL);
          BKE_pose_ensure(bmain, ob, ob->data, true);
        }
      }
      /* Only check overrides if we do have the real reference data available, and not some empty
       * 'placeholder' for missing data (broken links). */
      if ((id->override_library->reference->tag & LIB_TAG_MISSING) == 0) {
        BLI_task_pool_push(task_pool, lib_override_library_operations_create_cb, id, false, NULL);
      }
      else {
        BKE_lib_override_library_properties_tag(
            id->override_library, IDOVERRIDE_LIBRARY_TAG_UNUSED, false);
      }
    }
    id->tag &= ~LIB_TAG_OVERRIDE_LIBRARY_AUTOREFRESH;
  }
  FOREACH_MAIN_ID_END;

  BLI_task_pool_work_and_wait(task_pool);

  BLI_task_pool_free(task_pool);

  if (force_auto) {
    BKE_lib_override_library_main_unused_cleanup(bmain);
  }

#ifdef DEBUG_OVERRIDE_TIMEIT
  TIMEIT_END_AVERAGED(BKE_lib_override_library_main_operations_create);
#endif
}

static bool lib_override_library_id_reset_do(Main *bmain, ID *id_root)
{
  bool was_op_deleted = false;

  LISTBASE_FOREACH_MUTABLE (
      IDOverrideLibraryProperty *, op, &id_root->override_library->properties) {
    bool do_op_delete = true;
    const bool is_collection = op->rna_prop_type == PROP_COLLECTION;
    if (is_collection || op->rna_prop_type == PROP_POINTER) {
      PointerRNA ptr_root, ptr_root_lib, ptr, ptr_lib;
      PropertyRNA *prop, *prop_lib;

      RNA_pointer_create(id_root, &RNA_ID, id_root, &ptr_root);
      RNA_pointer_create(id_root->override_library->reference,
                         &RNA_ID,
                         id_root->override_library->reference,
                         &ptr_root_lib);

      bool prop_exists = RNA_path_resolve_property(&ptr_root, op->rna_path, &ptr, &prop);
      BLI_assert(prop_exists);
      prop_exists = RNA_path_resolve_property(&ptr_root_lib, op->rna_path, &ptr_lib, &prop_lib);

      if (prop_exists) {
        BLI_assert(ELEM(RNA_property_type(prop), PROP_POINTER, PROP_COLLECTION));
        BLI_assert(RNA_property_type(prop) == RNA_property_type(prop_lib));
        if (is_collection) {
          ptr.type = RNA_property_pointer_type(&ptr, prop);
          ptr_lib.type = RNA_property_pointer_type(&ptr_lib, prop_lib);
        }
        else {
          ptr = RNA_property_pointer_get(&ptr, prop);
          ptr_lib = RNA_property_pointer_get(&ptr_lib, prop_lib);
        }
        if (ptr.owner_id != NULL && ptr_lib.owner_id != NULL) {
          BLI_assert(ptr.type == ptr_lib.type);
          do_op_delete = !(RNA_struct_is_ID(ptr.type) && ptr.owner_id->override_library != NULL &&
                           ptr.owner_id->override_library->reference == ptr_lib.owner_id);
        }
      }
    }

    if (do_op_delete) {
      BKE_lib_override_library_property_delete(id_root->override_library, op);
      was_op_deleted = true;
    }
  }

  if (was_op_deleted) {
    DEG_id_tag_update_ex(bmain, id_root, ID_RECALC_COPY_ON_WRITE);
    IDOverrideLibraryRuntime *override_runtime = override_library_rna_path_runtime_ensure(
        id_root->override_library);
    override_runtime->tag |= IDOVERRIDE_LIBRARY_RUNTIME_TAG_NEEDS_RELOAD;
  }

  return was_op_deleted;
}

/** Reset all overrides in given \a id_root, while preserving ID relations. */
void BKE_lib_override_library_id_reset(Main *bmain, ID *id_root)
{
  if (!ID_IS_OVERRIDE_LIBRARY_REAL(id_root)) {
    return;
  }

  if (lib_override_library_id_reset_do(bmain, id_root)) {
    if (id_root->override_library->runtime != NULL &&
        (id_root->override_library->runtime->tag & IDOVERRIDE_LIBRARY_RUNTIME_TAG_NEEDS_RELOAD) !=
            0) {
      BKE_lib_override_library_update(bmain, id_root);
      id_root->override_library->runtime->tag &= ~IDOVERRIDE_LIBRARY_RUNTIME_TAG_NEEDS_RELOAD;
    }
  }
}

static void lib_override_library_id_hierarchy_recursive_reset(Main *bmain, ID *id_root)
{
  if (!ID_IS_OVERRIDE_LIBRARY_REAL(id_root)) {
    return;
  }

  void **entry_pp = BLI_ghash_lookup(bmain->relations->id_user_to_used, id_root);
  if (entry_pp == NULL) {
    /* Already processed. */
    return;
  }

  lib_override_library_id_reset_do(bmain, id_root);

  /* This way we won't process again that ID, should we encounter it again through another
   * relationship hierarchy.
   * Note that this does not free any memory from relations, so we can still use the entries.
   */
  BKE_main_relations_ID_remove(bmain, id_root);

  for (MainIDRelationsEntry *entry = *entry_pp; entry != NULL; entry = entry->next) {
    if ((entry->usage_flag & IDWALK_CB_LOOPBACK) != 0) {
      /* Never consider 'loop back' relationships ('from', 'parents', 'owner' etc. pointers) as
       * actual dependencies. */
      continue;
    }
    /* We only consider IDs from the same library. */
    if (entry->id_pointer != NULL) {
      ID *id_entry = *entry->id_pointer;
      if (id_entry->override_library != NULL) {
        lib_override_library_id_hierarchy_recursive_reset(bmain, id_entry);
      }
    }
  }
}

/** Reset all overrides in given \a id_root and its dependencies, while preserving ID relations. */
void BKE_lib_override_library_id_hierarchy_reset(Main *bmain, ID *id_root)
{
  BKE_main_relations_create(bmain, 0);

  lib_override_library_id_hierarchy_recursive_reset(bmain, id_root);

  BKE_main_relations_free(bmain);

  ID *id;
  FOREACH_MAIN_ID_BEGIN (bmain, id) {
    if (!ID_IS_OVERRIDE_LIBRARY_REAL(id) || id->override_library->runtime == NULL ||
        (id->override_library->runtime->tag & IDOVERRIDE_LIBRARY_RUNTIME_TAG_NEEDS_RELOAD) == 0) {
      continue;
    }
    BKE_lib_override_library_update(bmain, id);
    id->override_library->runtime->tag &= ~IDOVERRIDE_LIBRARY_RUNTIME_TAG_NEEDS_RELOAD;
  }
  FOREACH_MAIN_ID_END;
}

/** Set or clear given tag in all operations in that override property data. */
void BKE_lib_override_library_operations_tag(struct IDOverrideLibraryProperty *override_property,
                                             const short tag,
                                             const bool do_set)
{
  if (override_property != NULL) {
    if (do_set) {
      override_property->tag |= tag;
    }
    else {
      override_property->tag &= ~tag;
    }

    LISTBASE_FOREACH (IDOverrideLibraryPropertyOperation *, opop, &override_property->operations) {
      if (do_set) {
        opop->tag |= tag;
      }
      else {
        opop->tag &= ~tag;
      }
    }
  }
}

/** Set or clear given tag in all properties and operations in that override data. */
void BKE_lib_override_library_properties_tag(struct IDOverrideLibrary *override,
                                             const short tag,
                                             const bool do_set)
{
  if (override != NULL) {
    LISTBASE_FOREACH (IDOverrideLibraryProperty *, op, &override->properties) {
      BKE_lib_override_library_operations_tag(op, tag, do_set);
    }
  }
}

/** Set or clear given tag in all properties and operations in that Main's ID override data. */
void BKE_lib_override_library_main_tag(struct Main *bmain, const short tag, const bool do_set)
{
  ID *id;

  FOREACH_MAIN_ID_BEGIN (bmain, id) {
    if (ID_IS_OVERRIDE_LIBRARY(id)) {
      BKE_lib_override_library_properties_tag(id->override_library, tag, do_set);
    }
  }
  FOREACH_MAIN_ID_END;
}

/** Remove all tagged-as-unused properties and operations from that ID override data. */
void BKE_lib_override_library_id_unused_cleanup(struct ID *local)
{
  if (ID_IS_OVERRIDE_LIBRARY_REAL(local)) {
    LISTBASE_FOREACH_MUTABLE (
        IDOverrideLibraryProperty *, op, &local->override_library->properties) {
      if (op->tag & IDOVERRIDE_LIBRARY_TAG_UNUSED) {
        BKE_lib_override_library_property_delete(local->override_library, op);
      }
      else {
        LISTBASE_FOREACH_MUTABLE (IDOverrideLibraryPropertyOperation *, opop, &op->operations) {
          if (opop->tag & IDOVERRIDE_LIBRARY_TAG_UNUSED) {
            BKE_lib_override_library_property_operation_delete(op, opop);
          }
        }
      }
    }
  }
}

/** Remove all tagged-as-unused properties and operations from that Main's ID override data. */
void BKE_lib_override_library_main_unused_cleanup(struct Main *bmain)
{
  ID *id;

  FOREACH_MAIN_ID_BEGIN (bmain, id) {
    if (ID_IS_OVERRIDE_LIBRARY(id)) {
      BKE_lib_override_library_id_unused_cleanup(id);
    }
  }
  FOREACH_MAIN_ID_END;
}

/** Update given override from its reference (re-applying overridden properties). */
void BKE_lib_override_library_update(Main *bmain, ID *local)
{
  if (!ID_IS_OVERRIDE_LIBRARY_REAL(local)) {
    return;
  }

  /* Do not attempt to apply overriding rules over an empty place-holder generated by link code
   * when it cannot find the actual library/ID. Much better to keep the local data-block as loaded
   * from the file in that case, until broken lib is fixed. */
  if (ID_MISSING(local->override_library->reference)) {
    return;
  }

  /* Recursively do 'ancestor' overrides first, if any. */
  if (local->override_library->reference->override_library &&
      (local->override_library->reference->tag & LIB_TAG_OVERRIDE_LIBRARY_REFOK) == 0) {
    BKE_lib_override_library_update(bmain, local->override_library->reference);
  }

  /* We want to avoid having to remap here, however creating up-to-date override is much simpler
   * if based on reference than on current override.
   * So we work on temp copy of reference, and 'swap' its content with local. */

  /* XXX We need a way to get off-Main copies of IDs (similar to localized mats/texts/ etc.)!
   *     However, this is whole bunch of code work in itself, so for now plain stupid ID copy
   *     will do, as inefficient as it is. :/
   *     Actually, maybe not! Since we are swapping with original ID's local content, we want to
   *     keep user-count in correct state when freeing tmp_id
   *     (and that user-counts of IDs used by 'new' local data also remain correct). */
  /* This would imply change in handling of user-count all over RNA
   * (and possibly all over Blender code).
   * Not impossible to do, but would rather see first if extra useless usual user handling
   * is actually a (performances) issue here. */

  ID *tmp_id = BKE_id_copy(bmain, local->override_library->reference);

  if (tmp_id == NULL) {
    return;
  }

  /* This ID name is problematic, since it is an 'rna name property' it should not be editable or
   * different from reference linked ID. But local ID names need to be unique in a given type
   * list of Main, so we cannot always keep it identical, which is why we need this special
   * manual handling here. */
  BLI_strncpy(tmp_id->name, local->name, sizeof(tmp_id->name));

  /* Those ugly loop-back pointers again. Luckily we only need to deal with the shape keys here,
   * collections' parents are fully runtime and reconstructed later. */
  Key *local_key = BKE_key_from_id(local);
  Key *tmp_key = BKE_key_from_id(tmp_id);
  if (local_key != NULL && tmp_key != NULL) {
    tmp_key->id.flag |= (local_key->id.flag & LIB_EMBEDDED_DATA_LIB_OVERRIDE);
  }

  PointerRNA rnaptr_src, rnaptr_dst, rnaptr_storage_stack, *rnaptr_storage = NULL;
  RNA_id_pointer_create(local, &rnaptr_src);
  RNA_id_pointer_create(tmp_id, &rnaptr_dst);
  if (local->override_library->storage) {
    rnaptr_storage = &rnaptr_storage_stack;
    RNA_id_pointer_create(local->override_library->storage, rnaptr_storage);
  }

  RNA_struct_override_apply(
      bmain, &rnaptr_dst, &rnaptr_src, rnaptr_storage, local->override_library);

  /* This also transfers all pointers (memory) owned by local to tmp_id, and vice-versa.
   * So when we'll free tmp_id, we'll actually free old, outdated data from local. */
  BKE_lib_id_swap(bmain, local, tmp_id);

  if (local_key != NULL && tmp_key != NULL) {
    /* This is some kind of hard-coded 'always enforced override'. */
    BKE_lib_id_swap(bmain, &local_key->id, &tmp_key->id);
    tmp_key->id.flag |= (local_key->id.flag & LIB_EMBEDDED_DATA_LIB_OVERRIDE);
    /* The swap of local and tmp_id inverted those pointers, we need to redefine proper
     * relationships. */
    *BKE_key_from_id_p(local) = local_key;
    *BKE_key_from_id_p(tmp_id) = tmp_key;
    local_key->from = local;
    tmp_key->from = tmp_id;
  }

  /* Again, horribly inefficient in our case, we need something off-Main
   * (aka more generic nolib copy/free stuff)! */
  BKE_id_free_ex(bmain, tmp_id, LIB_ID_FREE_NO_UI_USER, true);

  if (GS(local->name) == ID_AR) {
    /* Fun times again, thanks to bone pointers in pose data of objects. We keep same ID addresses,
     * but internal data has changed for sure, so we need to invalidate pose-bones caches. */
    LISTBASE_FOREACH (Object *, ob, &bmain->objects) {
      if (ob->pose != NULL && ob->data == local) {
        BLI_assert(ob->type == OB_ARMATURE);
        ob->pose->flag |= POSE_RECALC;
        /* We need to clear pose bone pointers immediately, some code may access those before pose
         * is actually recomputed, which can lead to segfault. */
        BKE_pose_clear_pointers(ob->pose);
      }
    }
  }

  if (local->override_library->storage) {
    /* We know this data-block is not used anywhere besides local->override->storage. */
    /* XXX For until we get fully shadow copies, we still need to ensure storage releases
     *     its usage of any ID pointers it may have. */
    BKE_id_free_ex(bmain, local->override_library->storage, LIB_ID_FREE_NO_UI_USER, true);
    local->override_library->storage = NULL;
  }

  local->tag |= LIB_TAG_OVERRIDE_LIBRARY_REFOK;

  /* Full rebuild of Depsgraph! */
  /* Note: this is really brute force, in theory updates from RNA should have handled this already,
   * but for now let's play it safe. */
  DEG_id_tag_update_ex(bmain, local, ID_RECALC_COPY_ON_WRITE);
  DEG_relations_tag_update(bmain);
}

/** Update all overrides from given \a bmain. */
void BKE_lib_override_library_main_update(Main *bmain)
{
  ID *id;

  /* This temporary swap of G_MAIN is rather ugly,
   * but necessary to avoid asserts checks in some RNA assignment functions,
   * since those always use on G_MAIN when they need access to a Main database. */
  Main *orig_gmain = G_MAIN;
  G_MAIN = bmain;

  FOREACH_MAIN_ID_BEGIN (bmain, id) {
    if (id->override_library != NULL) {
      BKE_lib_override_library_update(bmain, id);
    }
  }
  FOREACH_MAIN_ID_END;

  G_MAIN = orig_gmain;
}

/**
 * Storage (how to store overriding data into `.blend` files).
 *
 * Basically:
 * 1) Only 'differential' overrides needs special handling here. All others (replacing values or
 *    inserting/removing items from a collection) can be handled with simply storing current
 *    content of local data-block.
 * 2) We store the differential value into a second 'ghost' data-block, which is an empty ID of
 *    same type as the local one, where we only define values that need differential data.
 *
 * This avoids us having to modify 'real' data-block at write time (and restoring it afterwards),
 * which is inefficient, and potentially dangerous (in case of concurrent access...), while not
 * using much extra memory in typical cases.  It also ensures stored data-block always contains
 * exact same data as "desired" ones (kind of "baked" data-blocks).
 */

/** Initialize an override storage. */
OverrideLibraryStorage *BKE_lib_override_library_operations_store_init(void)
{
  return BKE_main_new();
}

/**
 * Generate suitable 'write' data (this only affects differential override operations).
 *
 * Note that \a local ID is no more modified by this call,
 * all extra data are stored in its temp \a storage_id copy. */
ID *BKE_lib_override_library_operations_store_start(Main *bmain,
                                                    OverrideLibraryStorage *override_storage,
                                                    ID *local)
{
  if (ID_IS_OVERRIDE_LIBRARY_TEMPLATE(local) || ID_IS_OVERRIDE_LIBRARY_VIRTUAL(local)) {
    /* This is actually purely local data with an override template, nothing to do here! */
    return NULL;
  }

  BLI_assert(ID_IS_OVERRIDE_LIBRARY_REAL(local));
  BLI_assert(override_storage != NULL);
  UNUSED_VARS_NDEBUG(override_storage);

  /* Forcefully ensure we know about all needed override operations. */
  BKE_lib_override_library_operations_create(bmain, local);

  ID *storage_id;
#ifdef DEBUG_OVERRIDE_TIMEIT
  TIMEIT_START_AVERAGED(BKE_lib_override_library_operations_store_start);
#endif

  /* This is fully disabled for now, as it generated very hard to solve issues with Collections and
   * how they reference each-other in their parents/children relations.
   * Core of the issue is creating and storing those copies in a separate Main, while collection
   * copy code re-assign blindly parents/children, even if they do not belong to the same Main.
   * One solution could be to implement special flag as discussed below, and prevent any
   * other-ID-reference creation/update in that case (since no differential operation is expected
   * to involve those anyway). */
#if 0
  /* XXX TODO We may also want a specialized handling of things here too, to avoid copying heavy
   * never-overridable data (like Mesh geometry etc.)? And also maybe avoid lib
   * reference-counting completely (shallow copy). */
  /* This would imply change in handling of user-count all over RNA
   * (and possibly all over Blender code).
   * Not impossible to do, but would rather see first is extra useless usual user handling is
   * actually a (performances) issue here, before doing it. */
  storage_id = BKE_id_copy((Main *)override_storage, local);

  if (storage_id != NULL) {
    PointerRNA rnaptr_reference, rnaptr_final, rnaptr_storage;
    RNA_id_pointer_create(local->override_library->reference, &rnaptr_reference);
    RNA_id_pointer_create(local, &rnaptr_final);
    RNA_id_pointer_create(storage_id, &rnaptr_storage);

    if (!RNA_struct_override_store(
            bmain, &rnaptr_final, &rnaptr_reference, &rnaptr_storage, local->override_library)) {
      BKE_id_free_ex(override_storage, storage_id, LIB_ID_FREE_NO_UI_USER, true);
      storage_id = NULL;
    }
  }
#else
  storage_id = NULL;
#endif

  local->override_library->storage = storage_id;

#ifdef DEBUG_OVERRIDE_TIMEIT
  TIMEIT_END_AVERAGED(BKE_lib_override_library_operations_store_start);
#endif
  return storage_id;
}

/** Restore given ID modified by \a BKE_lib_override_library_operations_store_start, to its
 * original state. */
void BKE_lib_override_library_operations_store_end(
    OverrideLibraryStorage *UNUSED(override_storage), ID *local)
{
  BLI_assert(ID_IS_OVERRIDE_LIBRARY_REAL(local));

  /* Nothing else to do here really, we need to keep all temp override storage data-blocks in
   * memory until whole file is written anyway (otherwise we'd get mem pointers overlap). */
  local->override_library->storage = NULL;
}

void BKE_lib_override_library_operations_store_finalize(OverrideLibraryStorage *override_storage)
{
  /* We cannot just call BKE_main_free(override_storage), not until we have option to make
   * 'ghost' copies of IDs without increasing usercount of used data-blocks. */
  ID *id;

  FOREACH_MAIN_ID_BEGIN (override_storage, id) {
    BKE_id_free_ex(override_storage, id, LIB_ID_FREE_NO_UI_USER, true);
  }
  FOREACH_MAIN_ID_END;

  BKE_main_free(override_storage);
}

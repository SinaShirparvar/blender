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
 */

#include "usd_writer_material.h"

#include "usd.h"
#include "usd_exporter_context.h"

#include "BKE_image.h"
#include "BKE_main.h"
#include "BKE_node.h"

#include "BLI_fileops.h"
#include "BLI_linklist.h"
#include "BLI_listbase.h"
#include "BLI_math.h"
#include "BLI_path_util.h"
#include "BLI_string.h"

#include "DNA_material_types.h"

#include "MEM_guardedalloc.h"

#include <pxr/base/tf/stringUtils.h>
#include <pxr/pxr.h>
#include <pxr/usd/usdGeom/scope.h>

#include <iostream>

/* TfToken objects are not cheap to construct, so we do it once. */
namespace usdtokens {
// Materials
static const pxr::TfToken clearcoat("clearcoat", pxr::TfToken::Immortal);
static const pxr::TfToken clearcoatRoughness("clearcoatRoughness", pxr::TfToken::Immortal);
static const pxr::TfToken diffuse_color("diffuseColor", pxr::TfToken::Immortal);
static const pxr::TfToken metallic("metallic", pxr::TfToken::Immortal);
static const pxr::TfToken preview_shader("previewShader", pxr::TfToken::Immortal);
static const pxr::TfToken preview_surface("UsdPreviewSurface", pxr::TfToken::Immortal);
static const pxr::TfToken uv_texture("UsdUVTexture", pxr::TfToken::Immortal);
static const pxr::TfToken primvar_float2("UsdPrimvarReader_float2", pxr::TfToken::Immortal);
static const pxr::TfToken roughness("roughness", pxr::TfToken::Immortal);
static const pxr::TfToken specular("specular", pxr::TfToken::Immortal);
static const pxr::TfToken opacity("opacity", pxr::TfToken::Immortal);
static const pxr::TfToken surface("surface", pxr::TfToken::Immortal);
static const pxr::TfToken perspective("perspective", pxr::TfToken::Immortal);
static const pxr::TfToken orthographic("orthographic", pxr::TfToken::Immortal);
static const pxr::TfToken rgb("rgb", pxr::TfToken::Immortal);
static const pxr::TfToken r("r", pxr::TfToken::Immortal);
static const pxr::TfToken g("g", pxr::TfToken::Immortal);
static const pxr::TfToken b("b", pxr::TfToken::Immortal);
static const pxr::TfToken st("st", pxr::TfToken::Immortal);
static const pxr::TfToken result("result", pxr::TfToken::Immortal);
static const pxr::TfToken varname("varname", pxr::TfToken::Immortal);
static const pxr::TfToken mdl("mdl", pxr::TfToken::Immortal);
static const pxr::TfToken out("out", pxr::TfToken::Immortal);
static const pxr::TfToken normal("normal", pxr::TfToken::Immortal);
static const pxr::TfToken ior("ior", pxr::TfToken::Immortal);
static const pxr::TfToken file("file", pxr::TfToken::Immortal);
static const pxr::TfToken preview("preview", pxr::TfToken::Immortal);
static const pxr::TfToken raw("raw", pxr::TfToken::Immortal);
static const pxr::TfToken sRGB("sRGB", pxr::TfToken::Immortal);
static const pxr::TfToken sourceColorSpace("sourceColorSpace", pxr::TfToken::Immortal);
static const pxr::TfToken Shader("Shader", pxr::TfToken::Immortal);
}  // namespace usdtokens

/* Cycles specific tokens (Blender Importer and HdCycles) */
namespace cyclestokens {
static const pxr::TfToken cycles("cycles", pxr::TfToken::Immortal);
static const pxr::TfToken UVMap("UVMap", pxr::TfToken::Immortal);
static const pxr::TfToken filename("filename", pxr::TfToken::Immortal);
static const pxr::TfToken interpolation("interpolation", pxr::TfToken::Immortal);
static const pxr::TfToken projection("projection", pxr::TfToken::Immortal);
static const pxr::TfToken extension("extension", pxr::TfToken::Immortal);
static const pxr::TfToken colorspace("colorspace", pxr::TfToken::Immortal);
static const pxr::TfToken attribute("attribute", pxr::TfToken::Immortal);
static const pxr::TfToken bsdf("bsdf", pxr::TfToken::Immortal);
static const pxr::TfToken closure("closure", pxr::TfToken::Immortal);
static const pxr::TfToken vector("vector", pxr::TfToken::Immortal);
}  // namespace cyclestokens

namespace blender::io::usd {

/* Returns true if the given paths are equal,
 * returns false otherwise. */
static bool paths_equal(const char *p1, const char *p2)
{
  /* Normalize the paths so we can compare them. */
  char norm_p1[FILE_MAX];
  char norm_p2[FILE_MAX];

  BLI_strncpy(norm_p1, p1, sizeof(norm_p1));
  BLI_strncpy(norm_p2, p2, sizeof(norm_p2));

#ifdef WIN32
  /* On Windows, BLI_path_normalize() expects
   * backslash separators, so we caonvert all
   * forward slashes to backslashes.
   * TODO(makowalski): consider alternatives to
   * avoid this. */
  BLI_str_replace_char(norm_p1, '/', '\\');
  BLI_str_replace_char(norm_p2, '/', '\\');
#endif

  BLI_path_normalize(nullptr, norm_p1);
  BLI_path_normalize(nullptr, norm_p2);

  return BLI_path_cmp(norm_p1, norm_p2) == 0;
}

/* Generate a file name for an in-memory image that doesn't have a
 * filepath already defined. */
static std::string get_in_memory_texture_filename(bNode *node)
{
  if (!node) {
    return "";
  }

  Image *ima = reinterpret_cast<Image *>(node->id);

  if (!ima) {
    return "";
  }

  if (strlen(ima->filepath) > 0) {
    /* We only generate a filename if the image
     * doesn't already have one. */
    return "";
  }

  bool is_dirty = BKE_image_is_dirty(ima);
  bool is_generated = ima->source == IMA_SRC_GENERATED;
  bool is_packed = BKE_image_has_packedfile(ima);

  if (!(is_generated || is_dirty || is_packed)) {
    return "";
  }

  char file_name[FILE_MAX];
  file_name[0] = '\0';

  /* Try using the iamge name for the file name. */

  /* Sanity check that the id name isn't empty. */
  if (strlen(ima->id.name) < 3) {
    return "";
  }

  strcpy(file_name, ima->id.name + 2);

  /* Determine the correct file extension from the image format. */

  ImBuf *imbuf = BKE_image_acquire_ibuf(ima, nullptr, nullptr);

  if (!imbuf) {
    return "";
  }

  ImageFormatData imageFormat;
  BKE_imbuf_to_image_format(&imageFormat, imbuf);

  BKE_image_path_ensure_ext_from_imformat(file_name, &imageFormat);

  return file_name;
}

static void export_in_memory_texture(Image *ima,
                                     const std::string &export_dir,
                                     const bool allow_overwrite)
{
  if (!ima) {
    return;
  }

  char file_name[FILE_MAX] = {0};

  if (strlen(ima->filepath) > 0) {
    BLI_split_file_part(ima->filepath, file_name, FILE_MAX);
  }
  else {
    /* Try using the image name for the file name.  */
    strcpy(file_name, ima->id.name + 2);
  }

  if (strlen(file_name) == 0) {
    printf("WARNING:  Couldn't retrieve in memory texture file name.\n");
    return;
  }

  ImBuf *imbuf = BKE_image_acquire_ibuf(ima, nullptr, nullptr);
  if (!imbuf) {
    return;
  }

  ImageFormatData imageFormat;
  BKE_imbuf_to_image_format(&imageFormat, imbuf);

  /* This image in its current state only exists in Blender memory.
   * So we have to export it. The export will keep the image state intact,
   * so the exported file will not be associated with the image. */

  BKE_image_path_ensure_ext_from_imformat(file_name, &imageFormat);

  std::string export_path = export_dir;

  if (export_path.back() != '/' && export_path.back() != '\\') {
    export_path += "/";
  }

  export_path += std::string(file_name);

  if (!allow_overwrite && BLI_exists(export_path.c_str())) {
    return;
  }

  std::cout << "Exporting in-memory texture to " << export_path << std::endl;

  if (BKE_imbuf_write_as(imbuf, export_path.c_str(), &imageFormat, true) == 0) {
    std::cout << "WARNING: couldn't export in-memory texture to " << export_path << std::endl;
  }
}

/* Get the absolute filepath of the given image. */
static void get_absolute_path(Image *ima, size_t path_len, char *r_path)
{
  if (!r_path || path_len == 0) {
    return;
  }

  if (!ima) {
    r_path[0] = '\0';
    return;
  }
  /* make absolute source path */
  BLI_strncpy(r_path, ima->filepath, path_len);
  BLI_path_abs(r_path, ID_BLEND_PATH_FROM_GLOBAL(&ima->id));
  BLI_path_normalize(nullptr, r_path);
}

/* If the given image is tiled, copy the image tiles to the given
 * destination directory. */
static void copy_tiled_textures(Image *ima,
                                const std::string &in_dest_dir,
                                const bool allow_overwrite)
{
  if (!ima || in_dest_dir.empty()) {
    return;
  }

  if (ima->source != IMA_SRC_TILED) {
    return;
  }

  std::string dest_dir = in_dest_dir;

  if (dest_dir.back() != '/' && dest_dir.back() != '\\') {
    dest_dir += "/";
  }

  char src_path[FILE_MAX];
  get_absolute_path(ima, sizeof(src_path), src_path);

  char src_dir[FILE_MAX];
  char src_file[FILE_MAX];
  BLI_split_dirfile(src_path, src_dir, src_file, FILE_MAX, FILE_MAX);

  char head[FILE_MAX], tail[FILE_MAX];
  unsigned short numlen;
  BLI_path_sequence_decode(src_file, head, tail, &numlen);

  /* Copy all tiles. */
  LISTBASE_FOREACH (ImageTile *, tile, &ima->tiles) {
    char tile_file[FILE_MAX];

    /* Build filepath of the tile. */
    BLI_path_sequence_encode(tile_file, head, tail, numlen, tile->tile_number);

    std::string dest_tile_path = dest_dir + std::string(tile_file);

    if (!allow_overwrite && BLI_exists(dest_tile_path.c_str())) {
      continue;
    }

    std::string src_tile_path = std::string(src_dir) + std::string(tile_file);

    if (allow_overwrite && paths_equal(src_tile_path.c_str(), dest_tile_path.c_str())) {
      /* Source and destination paths are the same, don't copy. */
      continue;
    }

    std::cout << "Copying texture tile from " << src_tile_path << " to " << dest_tile_path
              << std::endl;

    /* Copy the file. */
    if (BLI_copy(src_tile_path.c_str(), dest_tile_path.c_str()) != 0) {
      std::cout << "WARNING: couldn't copy texture tile from " << src_tile_path << " to "
                << dest_tile_path << std::endl;
    }
  }
}

/* Copy the given image to the destination directory. */
static void copy_single_file(Image *ima, const std::string &dest_dir, const bool allow_overwrite)
{
  if (!ima || dest_dir.empty()) {
    return;
  }

  char source_path[FILE_MAX];
  get_absolute_path(ima, sizeof(source_path), source_path);

  char file_name[FILE_MAX];
  BLI_split_file_part(source_path, file_name, FILE_MAX);

  std::string dest_path = dest_dir;

  if (dest_path.back() != '/' && dest_path.back() != '\\') {
    dest_path += "/";
  }

  dest_path += std::string(file_name);

  if (!allow_overwrite && BLI_exists(dest_path.c_str())) {
    return;
  }

  if (allow_overwrite && paths_equal(source_path, dest_path.c_str())) {
    /* Source and destination paths are the same, don't copy. */
    return;
  }

  std::cout << "Copying texture from " << source_path << " to " << dest_path << std::endl;

  /* Copy the file. */
  if (BLI_copy(source_path, dest_path.c_str()) != 0) {
    std::cout << "WARNING: couldn't copy texture from " << source_path << " to " << dest_path
              << std::endl;
  }
}

/* Gets a NodeTexImage's filepath */
static std::string get_node_tex_image_filepath(bNode *node)
{
  NodeTexImage *tex_original = (NodeTexImage *)node->storage;

  Image *ima = (Image *)node->id;

  if (!ima)
    return "";
  if (sizeof(ima->filepath) == 0)
    return "";

  char filepath[1024] = "\0";

  strncpy(filepath, ima->filepath, sizeof(ima->filepath));

  BKE_image_user_file_path(&tex_original->iuser, ima, filepath);

  BLI_str_replace_char(filepath, '\\', '/');

  if (ima->source == IMA_SRC_TILED) {
    char head[FILE_MAX], tail[FILE_MAX];
    unsigned short numlen;

    BLI_path_sequence_decode(filepath, head, tail, &numlen);
    return (std::string(head) + "<UDIM>" + std::string(tail));
  }

  return std::string(filepath);
}

static pxr::TfToken get_node_tex_image_color_space(bNode *node)
{
  if (node->type != SH_NODE_TEX_IMAGE) {
    std::cout << "get_node_tex_image_color_space() called with unexpected type.\n";
    return pxr::TfToken();
  }

  if (node->id == nullptr) {
    return pxr::TfToken();
  }

  Image *ima = reinterpret_cast<Image *>(node->id);

  pxr::TfToken color_space;

  if (strcmp(ima->colorspace_settings.name, "Raw") == 0) {
    color_space = usdtokens::raw;
  }
  else if (strcmp(ima->colorspace_settings.name, "Non-Color") == 0) {
    color_space = usdtokens::raw;
  }
  else if (strcmp(ima->colorspace_settings.name, "sRGB") == 0) {
    color_space = usdtokens::sRGB;
  }

  return color_space;
}

/* Search the upstream nodes connected to the given socket and return the first occurrance
 * of the node of the given type. Return null if no node of this type was found. */
static bNode *traverse_channel(bNodeSocket *input, short target_type = SH_NODE_TEX_IMAGE)
{
  if (input->link) {
    bNode *linked_node = input->link->fromnode;

    if (linked_node->type == target_type) {
      /* Return match. */
      return linked_node;
    }

    /* Recursively traverse the linked node's sockets. */
    for (bNodeSocket *sock = static_cast<bNodeSocket *>(linked_node->inputs.first); sock;
         sock = sock->next) {
      if (bNode *found_node = traverse_channel(sock)) {
        return found_node;
      }
    }
  }

  return nullptr;
}

/* Returns the first occurence of a principled bsdf or a diffuse bsdf node found in the given
 * material's node tree.  Returns null if no instance of either type was found.*/
static bNode *get_bsdf_node(Material *material)
{
  if (!material) {
    return nullptr;
  }

  LISTBASE_FOREACH (bNode *, node, &material->nodetree->nodes) {
    if (node->type == SH_NODE_BSDF_PRINCIPLED || node->type == SH_NODE_BSDF_DIFFUSE) {
      return node;
    }
  }

  return nullptr;
}

/* Creates a USD Preview Surface shader based on the given cycles node name and type. */
static pxr::UsdShadeShader create_usd_preview_shader(const USDExporterContext &usd_export_context,
                                                     pxr::UsdShadeMaterial &material,
                                                     const char *name,
                                                     int type)
{
  if (!name || !material) {
    return pxr::UsdShadeShader();
  }

  pxr::SdfPath shader_path = material.GetPath()
                                 .AppendChild(usdtokens::preview)
                                 .AppendChild(pxr::TfToken(pxr::TfMakeValidIdentifier(name)));
  pxr::UsdShadeShader shader = pxr::UsdShadeShader::Define(usd_export_context.stage, shader_path);

  switch (type) {
    case SH_NODE_TEX_IMAGE: {
      shader.CreateIdAttr(pxr::VtValue(usdtokens::uv_texture));
      break;
    }
    case SH_NODE_TEX_COORD:
    case SH_NODE_UVMAP: {
      shader.CreateIdAttr(pxr::VtValue(usdtokens::primvar_float2));
      break;
    }
    case SH_NODE_BSDF_DIFFUSE:
    case SH_NODE_BSDF_PRINCIPLED: {
      shader.CreateIdAttr(pxr::VtValue(usdtokens::preview_surface));
      material.CreateSurfaceOutput().ConnectToSource(shader, usdtokens::surface);
      break;
    }

    default:
      break;
  }

  return shader;
}

/* Creates a USD Preview Surface shader based on the given cycles shading node. */
static pxr::UsdShadeShader create_usd_preview_shader(const USDExporterContext &usd_export_context,
                                                     pxr::UsdShadeMaterial &material,
                                                     bNode *node)
{
  if (!node) {
    return pxr::UsdShadeShader();
  }

  pxr::UsdShadeShader shader = create_usd_preview_shader(
      usd_export_context, material, node->name, node->type);

  if (node->type == SH_NODE_TEX_IMAGE) {

    /* For texture image nodes we set the image path and color space. */
    std::string imagePath = get_node_tex_image_filepath(
        node, usd_export_context.stage, usd_export_context.export_params);
    if (!imagePath.empty()) {
      shader.CreateInput(usdtokens::file, pxr::SdfValueTypeNames->Asset)
          .Set(pxr::SdfAssetPath(imagePath));
    }

    pxr::TfToken colorSpace = get_node_tex_image_color_space(node);
    if (!colorSpace.IsEmpty()) {
      shader.CreateInput(usdtokens::sourceColorSpace, pxr::SdfValueTypeNames->Token)
          .Set(colorSpace);
    }
  }

  return shader;
}

/* Entry point to create an approximate USD Preview Surface network from a Cycles node graph.
 * Due to the limited nodes in the USD Preview Surface specification, only the following nodes
 * are supported:
 *  - UVMap
 *  - Texture Coordinate
 *  - Image Texture
 *  - Principled BSDF
 * More may be added in the future.
 *
 * The 'default_uv' paramter is used as the default UV set name sampled by the primvar
 * reader shaders generated for image texture nodes shat don't have an attached UVMAp node. */
void create_usd_preview_surface_material(const USDExporterContext &usd_export_context,
                                         Material *material,
                                         pxr::UsdShadeMaterial &usd_material,
                                         const std::string &default_uv)
{
  /* Define a 'preview' scope beneath the material which will contain the preview shaders. */
  pxr::UsdGeomScope::Define(usd_export_context.stage,
                            usd_material.GetPath().AppendChild(usdtokens::preview));

  pxr::TfToken default_uv_sampler = default_uv.empty() ? cyclestokens::UVMap :
                                                         pxr::TfToken(default_uv);

  /* We only handle the first instance of either principled or
   * diffuse bsdf nodes in the material's node tree, because
   * USD Preview Surface has no concept of layering materials. */
  bNode *node = get_bsdf_node(material);

  if (!node) {
    return;
  }

  pxr::UsdShadeShader preview_surface = create_usd_preview_shader(
      usd_export_context, usd_material, node);

  /* Set the preview surface inputs. */
  LISTBASE_FOREACH (bNodeSocket *, sock, &node->inputs) {
    bNode *input_node = nullptr;
    pxr::UsdShadeShader created_shader;

    if (STREQ(sock->name, "Base Color") || STREQ(sock->name, "Color")) {

      input_node = traverse_channel(sock);
      if (input_node) {
        /* Create connection. */
        created_shader = create_usd_preview_shader(usd_export_context, usd_material, input_node);
        preview_surface.CreateInput(usdtokens::diffuse_color, pxr::SdfValueTypeNames->Float3)
            .ConnectToSource(created_shader, usdtokens::rgb);
      }
      else {
        /* Set hardcoded value. */
        bNodeSocketValueRGBA *socket_data = (bNodeSocketValueRGBA *)sock->default_value;
        preview_surface.CreateInput(usdtokens::diffuse_color, pxr::SdfValueTypeNames->Float3)
            .Set(pxr::VtValue(pxr::GfVec3f(
                socket_data->value[0], socket_data->value[1], socket_data->value[2])));
      }
    }
    else if (STREQ(sock->name, "Roughness")) {

      input_node = traverse_channel(sock);
      if (input_node) {
        /* Create connection. */
        created_shader = create_usd_preview_shader(usd_export_context, usd_material, input_node);
        preview_surface.CreateInput(usdtokens::roughness, pxr::SdfValueTypeNames->Float)
            .ConnectToSource(created_shader, usdtokens::r);
      }
      else {
        /* Set hardcoded value. */
        bNodeSocketValueFloat *socket_data = (bNodeSocketValueFloat *)sock->default_value;
        preview_surface.CreateInput(usdtokens::roughness, pxr::SdfValueTypeNames->Float)
            .Set(pxr::VtValue(socket_data->value));
      }
    }
    else if (STREQ(sock->name, "Metallic")) {

      input_node = traverse_channel(sock);
      if (input_node) {
        /* Create connection. */
        created_shader = create_usd_preview_shader(usd_export_context, usd_material, input_node);
        preview_surface.CreateInput(usdtokens::metallic, pxr::SdfValueTypeNames->Float)
            .ConnectToSource(created_shader, usdtokens::r);
      }
      else {
        /* Set hardcoded value. */
        bNodeSocketValueFloat *socket_data = (bNodeSocketValueFloat *)sock->default_value;
        preview_surface.CreateInput(usdtokens::metallic, pxr::SdfValueTypeNames->Float)
            .Set(pxr::VtValue(socket_data->value));
      }
    }
    else if (STREQ(sock->name, "Specular")) {

      input_node = traverse_channel(sock);
      if (input_node) {
        /* Create connection. */
        created_shader = create_usd_preview_shader(usd_export_context, usd_material, input_node);
        preview_surface.CreateInput(usdtokens::specular, pxr::SdfValueTypeNames->Float)
            .ConnectToSource(created_shader, usdtokens::r);
      }
      else {
        /* Set hardcoded value. */
        bNodeSocketValueFloat *socket_data = (bNodeSocketValueFloat *)sock->default_value;
        preview_surface.CreateInput(usdtokens::specular, pxr::SdfValueTypeNames->Float)
            .Set(pxr::VtValue(socket_data->value));
      }
    }
    else if (STREQ(sock->name, "Alpha")) {

      input_node = traverse_channel(sock);
      if (input_node) {
        /* Create connection. */
        created_shader = create_usd_preview_shader(usd_export_context, usd_material, input_node);
        preview_surface.CreateInput(usdtokens::opacity, pxr::SdfValueTypeNames->Float)
            .ConnectToSource(created_shader, usdtokens::r);
      }
      else {
        /* Set hardcoded value. */
        bNodeSocketValueFloat *socket_data = (bNodeSocketValueFloat *)sock->default_value;
        preview_surface.CreateInput(usdtokens::opacity, pxr::SdfValueTypeNames->Float)
            .Set(pxr::VtValue(socket_data->value));
      }
    }
    else if (STREQ(sock->name, "IOR")) {

      input_node = traverse_channel(sock);
      if (input_node) {
        /* Create connection. */
        created_shader = create_usd_preview_shader(usd_export_context, usd_material, input_node);
        preview_surface.CreateInput(usdtokens::ior, pxr::SdfValueTypeNames->Float)
            .ConnectToSource(created_shader, usdtokens::r);
      }
      else {
        /* Set hardcoded value. */
        bNodeSocketValueFloat *socket_data = (bNodeSocketValueFloat *)sock->default_value;
        preview_surface.CreateInput(usdtokens::ior, pxr::SdfValueTypeNames->Float)
            .Set(pxr::VtValue(socket_data->value));
      }
    }
    else if (STREQ(sock->name, "Normal")) {

      input_node = traverse_channel(sock);
      if (input_node) {
        created_shader = create_usd_preview_shader(usd_export_context, usd_material, input_node);
        preview_surface.CreateInput(usdtokens::normal, pxr::SdfValueTypeNames->Float3)
            .ConnectToSource(created_shader, usdtokens::rgb);
      }
      /* We don't handle hardcoded value. */
    }
    else if (STREQ(sock->name, "Clearcoat")) {

      input_node = traverse_channel(sock);
      if (input_node) {
        /* Create connection. */
        created_shader = create_usd_preview_shader(usd_export_context, usd_material, input_node);
        preview_surface.CreateInput(usdtokens::clearcoat, pxr::SdfValueTypeNames->Float)
            .ConnectToSource(created_shader, usdtokens::r);
      }
      else {
        /* Set hardcoded value. */
        bNodeSocketValueFloat *socket_data = (bNodeSocketValueFloat *)sock->default_value;
        preview_surface.CreateInput(usdtokens::clearcoat, pxr::SdfValueTypeNames->Float)
            .Set(pxr::VtValue(socket_data->value));
      }
    }
    else if (STREQ(sock->name, "Clearcoat Roughness")) {

      input_node = traverse_channel(sock);
      if (input_node) {
        /* Create connection. */
        created_shader = create_usd_preview_shader(usd_export_context, usd_material, input_node);
        preview_surface.CreateInput(usdtokens::clearcoatRoughness, pxr::SdfValueTypeNames->Float)
            .ConnectToSource(created_shader, usdtokens::r);
      }
      else {
        /* Set hardcoded value. */
        bNodeSocketValueFloat *socket_data = (bNodeSocketValueFloat *)sock->default_value;
        preview_surface.CreateInput(usdtokens::clearcoatRoughness, pxr::SdfValueTypeNames->Float)
            .Set(pxr::VtValue(socket_data->value));
      }
    }

    /* If any input texture node has been found, export the texture, if necessary,
     * and look for a connected uv node. */
    if (created_shader && input_node && input_node->type == SH_NODE_TEX_IMAGE) {

      if (usd_export_context.export_params.export_textures) {
        export_texture(input_node,
                       usd_export_context.stage,
                       usd_export_context.export_params.overwrite_textures);
      }

      bool found_uv_node = false;

      /* Find UV input to the texture node. */
      LISTBASE_FOREACH (bNodeSocket *, input_node_sock, &input_node->inputs) {

        if (!input_node_sock || !input_node_sock->link ||
            !STREQ(input_node_sock->name, "Vector")) {
          continue;
        }

        bNode *uv_node = traverse_channel(input_node_sock, SH_NODE_UVMAP);

        if (uv_node == NULL) {
          continue;
        }

        pxr::UsdShadeShader uv_shader = create_usd_preview_shader(
            usd_export_context, usd_material, uv_node);
        if (!uv_shader.GetPrim().IsValid())
          continue;

        found_uv_node = true;

        if (NodeShaderUVMap *shader_uv_map = static_cast<NodeShaderUVMap *>(uv_node->storage)) {
          /* We need to make valid here because actual uv primvar has been. */
          std::string uv_set = pxr::TfMakeValidIdentifier(shader_uv_map->uv_map);

          uv_shader.CreateInput(usdtokens::varname, pxr::SdfValueTypeNames->Token)
              .Set(pxr::TfToken(uv_set));
          created_shader.CreateInput(usdtokens::st, pxr::SdfValueTypeNames->Float2)
              .ConnectToSource(uv_shader, usdtokens::result);
        }
        else {
          uv_shader.CreateInput(usdtokens::varname, pxr::SdfValueTypeNames->Token)
              .Set(default_uv_sampler);
          created_shader.CreateInput(usdtokens::st, pxr::SdfValueTypeNames->Float2)
              .ConnectToSource(uv_shader, usdtokens::result);
        }
      }

      if (!found_uv_node) {
        /* No UVMAP node was linked to the texture node. However, we generate
         * a primvar reader node that specifies the UV set to sample, as some
         * DCCs require this. */

        pxr::UsdShadeShader uv_shader = create_usd_preview_shader(
            usd_export_context, usd_material, "uvmap", SH_NODE_TEX_COORD);

        if (uv_shader.GetPrim().IsValid()) {
          uv_shader.CreateInput(usdtokens::varname, pxr::SdfValueTypeNames->Token)
              .Set(default_uv_sampler);
          created_shader.CreateInput(usdtokens::st, pxr::SdfValueTypeNames->Float2)
              .ConnectToSource(uv_shader, usdtokens::result);
        }
      }
    }
  }
}

/* Entry point to create USD Shade Material network from Blender "Viewport Display". */
void create_usd_viewport_material(const USDExporterContext &usd_export_context,
                                  Material *material,
                                  pxr::UsdShadeMaterial &usd_material)
{
  /* Construct the shader. */
  pxr::SdfPath shader_path = usd_material.GetPath().AppendChild(usdtokens::preview_shader);
  pxr::UsdShadeShader shader = pxr::UsdShadeShader::Define(usd_export_context.stage, shader_path);

  shader.CreateIdAttr(pxr::VtValue(usdtokens::preview_surface));
  shader.CreateInput(usdtokens::diffuse_color, pxr::SdfValueTypeNames->Color3f)
      .Set(pxr::GfVec3f(material->r, material->g, material->b));
  shader.CreateInput(usdtokens::roughness, pxr::SdfValueTypeNames->Float).Set(material->roughness);
  shader.CreateInput(usdtokens::metallic, pxr::SdfValueTypeNames->Float).Set(material->metallic);

  /* Connect the shader and the material together. */
  usd_material.CreateSurfaceOutput().ConnectToSource(shader, usdtokens::surface);
}

/* Gets a NodeTexImage's filepath, returning a path in the texture export directory or a relative
 * path, if the export parameters require it. */
std::string get_node_tex_image_filepath(bNode *node,
                                        const pxr::UsdStageRefPtr stage,
                                        const USDExportParams &export_params)
{
  std::string image_path = get_node_tex_image_filepath(node);

  if (image_path.empty() && export_params.export_textures) {
    /* The path may be empty because this is an in-memory texture.
     * Since we are exporting textures, check if this is an
     * in-memory texture for which we can generate a file name. */
    image_path = get_in_memory_texture_filename(node);
  }

  return get_texture_filepath(image_path, stage, export_params);
}

/* Export the given texture node's image to a 'textures' directory
 * next to given stage's root layer USD.
 * Based on ImagesExporter::export_UV_Image() */
void export_texture(bNode *node, const pxr::UsdStageRefPtr stage, const bool allow_overwrite)
{
  if (!stage || !node ||
      (node->type != SH_NODE_TEX_IMAGE && node->type != SH_NODE_TEX_ENVIRONMENT)) {
    return;
  }

  // Get the path relative to the USD.
  // TODO(makowalski): avoid recomputing the USD path, if possible.
  pxr::SdfLayerHandle layer = stage->GetRootLayer();

  std::string stage_path = layer->GetRealPath();

  if (stage_path.empty()) {
    return;
  }

  Image *ima = reinterpret_cast<Image *>(node->id);

  if (!ima) {
    return;
  }

  char usd_dir_path[FILE_MAX];
  BLI_split_dir_part(stage_path.c_str(), usd_dir_path, FILE_MAX);

  std::string dest_dir(usd_dir_path);
  dest_dir += "textures";

  BLI_dir_create_recursive(dest_dir.c_str());

  dest_dir += "/";

  bool is_dirty = BKE_image_is_dirty(ima);
  bool is_generated = ima->source == IMA_SRC_GENERATED;
  bool is_packed = BKE_image_has_packedfile(ima);

  if (is_generated || is_dirty || is_packed) {
    export_in_memory_texture(ima, dest_dir, allow_overwrite);
  }
  else if (ima->source == IMA_SRC_TILED) {
    copy_tiled_textures(ima, dest_dir, allow_overwrite);
  }
  else {
    copy_single_file(ima, dest_dir, allow_overwrite);
  }
}

/* Process the given file path 'in_path' to convert it to an export path
 * for textures or to make the path relative to the given stage's root USD
 * layer. */
std::string get_texture_filepath(const std::string &in_path,
                                 const pxr::UsdStageRefPtr stage,
                                 const USDExportParams &export_params)
{
  /* Do nothing if we are not exporting textures or using relative texture paths. */
  if (!(export_params.relative_texture_paths || export_params.export_textures)) {
    return in_path;
  }

  if (in_path.empty() || !stage) {
    return in_path;
  }

  pxr::SdfLayerHandle layer = stage->GetRootLayer();
  std::string stage_path = layer->GetRealPath();

  if (stage_path.empty()) {
    return in_path;
  }

  /* If we are exporting textures, set the textures directory in the path. */
  if (export_params.export_textures) {
    /* The texture is exported to a 'textures' directory next to the
     * USD root layer. */

    char dir_path[FILE_MAX];
    char file_path[FILE_MAX];
    BLI_split_dir_part(stage_path.c_str(), dir_path, FILE_MAX);
    BLI_split_file_part(in_path.c_str(), file_path, FILE_MAX);

    BLI_str_replace_char(dir_path, '\\', '/');

    std::string result;

    if (export_params.relative_texture_paths) {
      result = "./textures/";
    }
    else {
      result = std::string(dir_path);
      if (result.back() != '/' && result.back() != '\\') {
        result += "/";
      }
      result += "textures/";
    }

    result += std::string(file_path);
    return result;
  }

  // Get the path relative to the USD.
  char rel_path[FILE_MAX];

  strcpy(rel_path, in_path.c_str());

  BLI_path_rel(rel_path, stage_path.c_str());

  /* BLI_path_rel adds '//' as a prefix to the path, if
   * generating the relative path was successful. */
  if (rel_path[0] != '/' || rel_path[1] != '/') {
    /* No relative path generated. */
    return in_path;
  }

  int offset = 0;

  if (rel_path[2] != '.') {
    rel_path[0] = '.';
  }
  else {
    offset = 2;
  }

  BLI_str_replace_char(rel_path, '\\', '/');

  return std::string(rel_path + offset);
}

}  // namespace blender::io::usd
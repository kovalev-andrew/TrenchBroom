/*
 Copyright (C) 2026 Kristian Duske

 This file is part of TrenchBroom.

 TrenchBroom is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 TrenchBroom is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with TrenchBroom. If not, see <http://www.gnu.org/licenses/>.
 */

#include "ui/UnitySyncDialog.h"

#include <QApplication>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressDialog>
#include <QPushButton>
#include <QSettings>

#include "gl/Material.h"
#include "gl/Texture.h"
#include "gl/TextureBuffer.h"
#include "mdl/BezierPatch.h"
#include "mdl/Brush.h"
#include "mdl/BrushFace.h"
#include "mdl/BrushNode.h"
#include "mdl/ExportOptions.h"
#include "mdl/Map.h"
#include "mdl/Node.h"
#include "mdl/NodeWriter.h"
#include "mdl/ObjSerializer.h"
#include "mdl/PatchNode.h"
#include "mdl/WorldNode.h"
#include "ui/BorderLine.h"
#include "ui/DialogButtonLayout.h"
#include "ui/DialogHeader.h"
#include "ui/FormWithSectionsLayout.h"
#include "ui/MapDocument.h" // IWYU pragma: keep
#include "ui/MapWindow.h"
#include "ui/QPathUtils.h"
#include "ui/QStyleUtils.h"
#include "ui/ViewConstants.h"

#include "kd/contracts.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <locale>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace tb::ui
{
namespace
{

constexpr auto TargetFolderSetting = "UnitySync/TargetFolder";

struct UnityTargetFolder
{
  std::filesystem::path folder;
  std::string assetPath;
};

std::string lowerAscii(std::string value)
{
  std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

bool pathComponentEquals(const std::filesystem::path& component, const std::string& value)
{
  return lowerAscii(component.string()) == lowerAscii(value);
}

std::string safeFileName(std::string value)
{
  if (value.empty())
  {
    return "unnamed";
  }

  for (auto& c : value)
  {
    const auto isAllowed =
      std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_' || c == '-';
    if (!isAllowed)
    {
      c = '_';
    }
  }
  return value;
}

std::string uniqueName(const std::string& requestedName, std::set<std::string>& usedNames)
{
  auto base = safeFileName(requestedName);
  if (base == "." || base == "..")
  {
    base = "unnamed";
  }

  auto result = base;
  auto index = size_t{1u};
  while (usedNames.contains(result))
  {
    result = base + "_" + std::to_string(index++);
  }

  usedNames.insert(result);
  return result;
}

std::optional<UnityTargetFolder> resolveUnityTargetFolder(
  const std::filesystem::path& inputFolder)
{
  if (inputFolder.empty())
  {
    return std::nullopt;
  }

  const auto folder = std::filesystem::absolute(inputFolder).lexically_normal();
  auto components = std::vector<std::filesystem::path>{};
  for (const auto& component : folder)
  {
    components.push_back(component);
  }

  for (size_t i = 0; i < components.size(); ++i)
  {
    if (!pathComponentEquals(components[i], "Assets"))
    {
      continue;
    }

    auto assetPath = std::filesystem::path{"Assets"};
    for (size_t j = i + 1u; j < components.size(); ++j)
    {
      assetPath /= components[j];
    }
    return UnityTargetFolder{folder, assetPath.generic_string()};
  }

  const auto projectAssetsPath = folder / "Assets";
  if (
    std::filesystem::exists(projectAssetsPath)
    && std::filesystem::is_directory(projectAssetsPath))
  {
    return UnityTargetFolder{
      projectAssetsPath / "TrenchBroomSync", "Assets/TrenchBroomSync"};
  }

  return std::nullopt;
}

bool isImageTexturePath(const std::filesystem::path& path)
{
  const auto extension = lowerAscii(path.extension().string());
  return extension == ".png" || extension == ".jpg" || extension == ".jpeg"
         || extension == ".tga" || extension == ".bmp" || extension == ".psd"
         || extension == ".tif" || extension == ".tiff";
}

std::optional<QImage::Format> qImageFormatForTextureFormat(const GLenum format)
{
  switch (format)
  {
  case GL_RGB:
    return QImage::Format_RGB888;
  case GL_BGR:
    return QImage::Format_BGR888;
  case GL_RGBA:
    return QImage::Format_RGBA8888;
  case GL_BGRA:
    return QImage::Format_ARGB32;
  default:
    return std::nullopt;
  }
}

std::optional<std::string> exportLoadedTextureAsPng(
  const gl::Material& material,
  const std::string& materialName,
  const std::filesystem::path& texturesPath,
  std::set<std::string>& warnings)
{
  const auto* texture = material.texture();
  if (texture == nullptr)
  {
    return std::nullopt;
  }

  const auto& buffers = texture->buffersIfLoaded();
  if (buffers.empty() || buffers.front().data() == nullptr)
  {
    return std::nullopt;
  }

  const auto format = qImageFormatForTextureFormat(texture->format());
  if (!format)
  {
    warnings.insert(
      "Material '" + materialName + "' uses a texture format that cannot be exported.");
    return std::nullopt;
  }

  const auto bytesPerPixel = gl::bytesPerPixelForFormat(texture->format());
  const auto width = static_cast<int>(texture->width());
  const auto height = static_cast<int>(texture->height());
  const auto bytesPerLine = static_cast<int>(texture->width() * bytesPerPixel);
  const auto image =
    QImage{buffers.front().data(), width, height, bytesPerLine, *format}.copy();

  const auto textureFileName = safeFileName(materialName) + ".png";
  const auto destinationPath = texturesPath / textureFileName;
  if (std::filesystem::exists(destinationPath))
  {
    return "../Textures/" + textureFileName;
  }

  if (!image.save(pathAsQString(destinationPath), "PNG"))
  {
    warnings.insert("Could not export texture for material '" + materialName + "'.");
    return std::nullopt;
  }

  return "../Textures/" + textureFileName;
}

std::string escapeCSharpString(const std::string& value)
{
  auto result = std::string{};
  result.reserve(value.size());
  for (const auto c : value)
  {
    switch (c)
    {
    case '\\':
      result += "\\\\";
      break;
    case '"':
      result += "\\\"";
      break;
    case '\n':
      result += "\\n";
      break;
    case '\r':
      result += "\\r";
      break;
    case '\t':
      result += "\\t";
      break;
    default:
      result += c;
      break;
    }
  }
  return result;
}

std::string csharpFloatLiteral(const double value)
{
  auto stream = std::ostringstream{};
  stream.imbue(std::locale::classic());
  stream << std::setprecision(9) << value << "f";
  return stream.str();
}

std::string fnv1aHex(const std::string& value)
{
  auto hash = uint32_t{2166136261u};
  for (const auto c : value)
  {
    hash ^= static_cast<uint8_t>(c);
    hash *= 16777619u;
  }

  auto stream = std::ostringstream{};
  stream << std::hex << std::setw(8) << std::setfill('0') << hash;
  return stream.str();
}

struct SurfaceMaterial
{
  std::string materialName;
  const gl::Material* material;
};

struct UnityMaterialAsset
{
  std::string objMaterialName;
  std::string textureAssetPath;
  std::string materialAssetPath;
};

struct UnityModelAsset
{
  std::string modelAssetPath;
  std::string objectName;
};

std::string brushFileStem(const size_t index)
{
  auto stream = std::ostringstream{};
  stream << "brush_" << std::setw(4) << std::setfill('0') << index;
  return stream.str();
}

std::vector<mdl::BrushNode*> collectBrushNodes(mdl::Map& map)
{
  auto result = std::vector<mdl::BrushNode*>{};

  const auto collect = [&](const auto& thisLambda, mdl::Node& node) -> void {
    if (auto* brushNode = dynamic_cast<mdl::BrushNode*>(&node))
    {
      result.push_back(brushNode);
      return;
    }

    for (auto* child : node.children())
    {
      thisLambda(thisLambda, *child);
    }
  };
  collect(collect, map.worldNode());

  return result;
}

std::vector<SurfaceMaterial> collectSurfaceMaterials(mdl::Map& map)
{
  auto result = std::vector<SurfaceMaterial>{};
  auto seenMaterialNames = std::set<std::string>{};

  const auto addMaterial = [&](std::string materialName, const gl::Material* material) {
    if (materialName.empty() || seenMaterialNames.contains(materialName))
    {
      return;
    }

    seenMaterialNames.insert(materialName);
    result.push_back(SurfaceMaterial{std::move(materialName), material});
  };

  const auto collect = [&](const auto& thisLambda, mdl::Node& node) -> void {
    if (auto* brushNode = dynamic_cast<mdl::BrushNode*>(&node))
    {
      for (const auto& face : brushNode->brush().faces())
      {
        addMaterial(face.attributes().materialName(), face.material());
      }
    }
    else if (auto* patchNode = dynamic_cast<mdl::PatchNode*>(&node))
    {
      const auto& patch = patchNode->patch();
      addMaterial(patch.materialName(), patch.material());
    }
    else
    {
      for (auto* child : node.children())
      {
        thisLambda(thisLambda, *child);
      }
    }
  };
  collect(collect, map.worldNode());

  return result;
}

std::unordered_map<std::string, std::string> buildObjMaterialNames(
  const std::vector<SurfaceMaterial>& materials)
{
  auto result = std::unordered_map<std::string, std::string>{};
  auto usedNames = std::set<std::string>{};

  for (const auto& material : materials)
  {
    result.emplace(material.materialName, uniqueName(material.materialName, usedNames));
  }

  return result;
}

void removeOldPerBrushExports(
  const std::filesystem::path& modelsPath, const std::filesystem::path& prefabsPath)
{
  const auto removeGeneratedFiles = [](const std::filesystem::path& directory) {
    if (!std::filesystem::exists(directory))
    {
      return;
    }

    for (const auto& entry : std::filesystem::directory_iterator{directory})
    {
      if (!entry.is_regular_file())
      {
        continue;
      }

      const auto stem = entry.path().stem().string();
      const auto extension = lowerAscii(entry.path().extension().string());
      if (
        stem.rfind("brush_", 0) == 0
        && (extension == ".obj" || extension == ".mtl" || extension == ".prefab"))
      {
        std::filesystem::remove(entry.path());
      }
    }
  };

  removeGeneratedFiles(modelsPath);
  removeGeneratedFiles(prefabsPath);
}

std::optional<std::string> exportMaterialTexture(
  const gl::Material* material,
  const std::string& materialName,
  const std::string& objMaterialName,
  const std::filesystem::path& texturesPath,
  const bool copyTextures,
  std::set<std::string>& warnings)
{
  if (!copyTextures)
  {
    return std::nullopt;
  }

  if (material == nullptr)
  {
    warnings.insert("Material '" + materialName + "' has no loaded texture.");
    return std::nullopt;
  }

  const auto& sourcePath = material->absolutePath();
  auto sourceImageFileMissing = false;
  if (!sourcePath.empty() && isImageTexturePath(sourcePath))
  {
    if (std::filesystem::exists(sourcePath))
    {
      auto textureFileName = objMaterialName;
      if (std::filesystem::path{textureFileName}.extension().empty())
      {
        textureFileName += sourcePath.extension().string();
      }

      const auto destinationPath = texturesPath / textureFileName;
      if (!std::filesystem::exists(destinationPath))
      {
        std::filesystem::copy_file(sourcePath, destinationPath);
      }
      return "../Textures/" + textureFileName;
    }

    sourceImageFileMissing = true;
  }

  if (
    const auto exportedPath =
      exportLoadedTextureAsPng(*material, objMaterialName, texturesPath, warnings))
  {
    return exportedPath;
  }

  if (sourceImageFileMissing)
  {
    warnings.insert(
      "Texture file for material '" + materialName
      + "' was not found: " + sourcePath.string());
  }

  if (sourcePath.empty() || !isImageTexturePath(sourcePath))
  {
    warnings.insert(
      "Material '" + materialName
      + "' is embedded, uploaded, or not an image file, so Unity will need a matching material.");
  }

  return std::nullopt;
}

std::unordered_map<std::string, std::string> exportMaterialTextures(
  const std::vector<SurfaceMaterial>& materials,
  const std::unordered_map<std::string, std::string>& objMaterialNames,
  const std::filesystem::path& texturesPath,
  const bool copyTextures,
  std::set<std::string>& warnings)
{
  auto result = std::unordered_map<std::string, std::string>{};

  for (const auto& material : materials)
  {
    const auto objMaterialName = objMaterialNames.at(material.materialName);
    if (
      const auto texturePath = exportMaterialTexture(
        material.material,
        material.materialName,
        objMaterialName,
        texturesPath,
        copyTextures,
        warnings))
    {
      result.emplace(material.materialName, *texturePath);
    }
  }

  return result;
}

void rewriteObjMaterialNames(
  const std::filesystem::path& objPath,
  const std::unordered_map<std::string, std::string>& objMaterialNames)
{
  auto input = std::ifstream{objPath};
  if (!input)
  {
    throw std::runtime_error{"Could not read " + objPath.string()};
  }

  auto output = std::ostringstream{};
  auto line = std::string{};
  while (std::getline(input, line))
  {
    if (line.rfind("usemtl ", 0) == 0)
    {
      const auto materialName = line.substr(7);
      if (
        const auto it = objMaterialNames.find(materialName); it != objMaterialNames.end())
      {
        line = "usemtl " + it->second;
      }
    }

    output << line << '\n';
  }

  auto objOutput = std::ofstream{objPath, std::ios::trunc};
  if (!objOutput)
  {
    throw std::runtime_error{"Could not write " + objPath.string()};
  }
  objOutput << output.str();
}

void rewriteMtlMaterialNamesAndTexturePaths(
  const std::filesystem::path& mtlPath,
  const std::unordered_map<std::string, std::string>& materialTexturePaths,
  const std::unordered_map<std::string, std::string>& objMaterialNames)
{
  auto input = std::ifstream{mtlPath};
  if (!input)
  {
    throw std::runtime_error{"Could not read " + mtlPath.string()};
  }

  auto currentMaterial = std::string{};
  auto wroteMapKdForCurrentMaterial = false;
  auto output = std::ostringstream{};
  const auto flushMaterialTexture = [&]() {
    if (currentMaterial.empty() || wroteMapKdForCurrentMaterial)
    {
      return;
    }

    if (
      const auto it = materialTexturePaths.find(currentMaterial);
      it != materialTexturePaths.end())
    {
      output << "map_Kd " << it->second << '\n';
      wroteMapKdForCurrentMaterial = true;
    }
  };

  auto line = std::string{};
  while (std::getline(input, line))
  {
    if (line.rfind("newmtl ", 0) == 0)
    {
      flushMaterialTexture();
      currentMaterial = line.substr(7);
      wroteMapKdForCurrentMaterial = false;
      if (
        const auto it = objMaterialNames.find(currentMaterial);
        it != objMaterialNames.end())
      {
        line = "newmtl " + it->second;
      }
    }
    else if (line.rfind("map_Kd ", 0) == 0)
    {
      if (
        const auto it = materialTexturePaths.find(currentMaterial);
        it != materialTexturePaths.end())
      {
        line = "map_Kd " + it->second;
        wroteMapKdForCurrentMaterial = true;
      }
    }

    output << line << '\n';
  }
  flushMaterialTexture();

  auto mtlOutput = std::ofstream{mtlPath, std::ios::trunc};
  if (!mtlOutput)
  {
    throw std::runtime_error{"Could not write " + mtlPath.string()};
  }
  mtlOutput << output.str();
}

void exportMapObj(
  mdl::Map& map,
  const std::filesystem::path& objPath,
  const std::filesystem::path& mtlPath)
{
  auto objStream = std::ofstream{objPath, std::ios::trunc};
  if (!objStream)
  {
    throw std::runtime_error{"Could not write " + objPath.string()};
  }

  auto mtlStream = std::ofstream{mtlPath, std::ios::trunc};
  if (!mtlStream)
  {
    throw std::runtime_error{"Could not write " + mtlPath.string()};
  }

  auto options =
    mdl::ObjExportOptions{objPath, mdl::ObjMtlPathMode::RelativeToExportPath};
  auto writer = mdl::NodeWriter{
    map.worldNode(),
    std::make_unique<mdl::ObjSerializer>(
      objStream, mtlStream, mtlPath.filename().string(), options)};
  writer.setExporting(true);
  writer.writeMap(map.taskManager());
}

void exportBrushObj(
  mdl::Map& map,
  mdl::BrushNode& brushNode,
  const std::filesystem::path& objPath,
  const std::filesystem::path& mtlPath)
{
  auto objStream = std::ofstream{objPath, std::ios::trunc};
  if (!objStream)
  {
    throw std::runtime_error{"Could not write " + objPath.string()};
  }

  auto mtlStream = std::ofstream{mtlPath, std::ios::trunc};
  if (!mtlStream)
  {
    throw std::runtime_error{"Could not write " + mtlPath.string()};
  }

  auto options =
    mdl::ObjExportOptions{objPath, mdl::ObjMtlPathMode::RelativeToExportPath};
  auto writer = mdl::NodeWriter{
    map.worldNode(),
    std::make_unique<mdl::ObjSerializer>(
      objStream, mtlStream, mtlPath.filename().string(), options)};
  writer.setExporting(true);
  writer.writeNodes(std::vector<mdl::Node*>{&brushNode}, map.taskManager());
}

std::vector<UnityMaterialAsset> buildUnityMaterialAssets(
  const std::vector<SurfaceMaterial>& materials,
  const std::unordered_map<std::string, std::string>& objMaterialNames,
  const std::unordered_map<std::string, std::string>& materialTexturePaths,
  const std::string& targetAssetPath)
{
  auto result = std::vector<UnityMaterialAsset>{};
  result.reserve(materials.size());

  for (const auto& material : materials)
  {
    const auto objMaterialName = objMaterialNames.at(material.materialName);
    auto textureAssetPath = std::string{};
    if (
      const auto textureIt = materialTexturePaths.find(material.materialName);
      textureIt != materialTexturePaths.end())
    {
      const auto textureFileName =
        std::filesystem::path{textureIt->second}.filename().generic_string();
      textureAssetPath = targetAssetPath + "/Textures/" + textureFileName;
    }

    result.push_back(
      UnityMaterialAsset{
        objMaterialName,
        textureAssetPath,
        targetAssetPath + "/Materials/" + objMaterialName + ".mat"});
  }

  return result;
}

void writeUnityImporterScript(
  const std::filesystem::path& scriptPath,
  const std::string& className,
  const std::vector<UnityModelAsset>& modelAssets,
  const std::string& prefabAssetPath,
  const double prefabScale,
  const std::vector<UnityMaterialAsset>& materialAssets)
{
  auto script = std::ofstream{scriptPath, std::ios::trunc};
  if (!script)
  {
    throw std::runtime_error{"Could not write " + scriptPath.string()};
  }

  script << "using System.Collections.Generic;\n";
  script << "using System.IO;\n";
  script << "using UnityEditor;\n";
  script << "using UnityEngine;\n\n";
  script << "[InitializeOnLoad]\n";
  script << "public static class " << className << "\n";
  script << "{\n";
  script << "    private const string Prefab = \"" << escapeCSharpString(prefabAssetPath)
         << "\";\n\n";
  script << "    private const float PrefabScale = " << csharpFloatLiteral(prefabScale)
         << ";\n\n";
  script << "    private static readonly string[] Models = new string[]\n";
  script << "    {\n";
  for (const auto& modelAsset : modelAssets)
  {
    script << "        \"" << escapeCSharpString(modelAsset.modelAssetPath) << "\",\n";
  }
  script << "    };\n\n";
  script << "    private static readonly string[] ObjectNames = new string[]\n";
  script << "    {\n";
  for (const auto& modelAsset : modelAssets)
  {
    script << "        \"" << escapeCSharpString(modelAsset.objectName) << "\",\n";
  }
  script << "    };\n\n";
  script << "    private static readonly string[] MaterialNames = new string[]\n";
  script << "    {\n";
  for (const auto& materialAsset : materialAssets)
  {
    script << "        \"" << escapeCSharpString(materialAsset.objMaterialName)
           << "\",\n";
  }
  script << "    };\n\n";
  script << "    private static readonly string[] TexturePaths = new string[]\n";
  script << "    {\n";
  for (const auto& materialAsset : materialAssets)
  {
    script << "        \"" << escapeCSharpString(materialAsset.textureAssetPath)
           << "\",\n";
  }
  script << "    };\n\n";
  script << "    private static readonly string[] MaterialPaths = new string[]\n";
  script << "    {\n";
  for (const auto& materialAsset : materialAssets)
  {
    script << "        \"" << escapeCSharpString(materialAsset.materialAssetPath)
           << "\",\n";
  }
  script << "    };\n\n";
  script << "    static " << className << "()\n";
  script << "    {\n";
  script << "        EditorApplication.delayCall += Sync;\n";
  script << "    }\n\n";
  script << "    [MenuItem(\"Tools/TrenchBroom/Regenerate Synced Prefabs\")]\n";
  script << "    public static void Sync()\n";
  script << "    {\n";
  script << "        AssetDatabase.Refresh(ImportAssetOptions.ForceSynchronousImport);\n";
  script << "        var materials = EnsureMaterials();\n";
  script << "        var directory = Path.GetDirectoryName(Prefab);\n";
  script << "        if (!string.IsNullOrEmpty(directory))\n";
  script << "        {\n";
  script << "            Directory.CreateDirectory(directory);\n";
  script << "        }\n\n";
  script
    << "        var root = new GameObject(Path.GetFileNameWithoutExtension(Prefab));\n";
  script << "        root.transform.localScale = Vector3.one * PrefabScale;\n";
  script << "        try\n";
  script << "        {\n";
  script << "            for (var i = 0; i < Models.Length; ++i)\n";
  script << "            {\n";
  script << "                var model = "
            "AssetDatabase.LoadAssetAtPath<GameObject>(Models[i]);\n";
  script << "                if (model == null)\n";
  script << "                {\n";
  script << "                    Debug.LogWarning(\"TrenchBroom sync skipped missing "
            "model: \" + Models[i]);\n";
  script << "                    continue;\n";
  script << "                }\n\n";
  script << "                var child = PrefabUtility.InstantiatePrefab(model) as "
            "GameObject;\n";
  script << "                if (child == null)\n";
  script << "                {\n";
  script << "                    child = Object.Instantiate(model);\n";
  script << "                }\n";
  script << "                child.name = i < ObjectNames.Length && "
            "!string.IsNullOrEmpty(ObjectNames[i])\n";
  script << "                    ? ObjectNames[i]\n";
  script << "                    : Path.GetFileNameWithoutExtension(Models[i]);\n";
  script << "                child.transform.SetParent(root.transform, false);\n";
  script << "                AssignMaterials(child, materials);\n";
  script << "            }\n\n";
  script << "            PrefabUtility.SaveAsPrefabAsset(root, Prefab);\n";
  script << "        }\n";
  script << "        finally\n";
  script << "        {\n";
  script << "            Object.DestroyImmediate(root);\n";
  script << "        }\n\n";
  script << "        AssetDatabase.SaveAssets();\n";
  script << "        Debug.Log(\"TrenchBroom sync generated prefab: \" + Prefab);\n";
  script << "    }\n\n";
  script << "    private static Dictionary<string, Material> EnsureMaterials()\n";
  script << "    {\n";
  script << "        var result = new Dictionary<string, Material>();\n";
  script << "        var shader = FindShader();\n";
  script << "        for (var i = 0; i < MaterialNames.Length && i < "
            "MaterialPaths.Length; ++i)\n";
  script << "        {\n";
  script << "            var materialPath = MaterialPaths[i];\n";
  script << "            var material = "
            "AssetDatabase.LoadAssetAtPath<Material>(materialPath);\n";
  script << "            if (material == null)\n";
  script << "            {\n";
  script
    << "                var materialDirectory = Path.GetDirectoryName(materialPath);\n";
  script << "                if (!string.IsNullOrEmpty(materialDirectory))\n";
  script << "                {\n";
  script << "                    Directory.CreateDirectory(materialDirectory);\n";
  script << "                }\n";
  script << "                if (shader == null)\n";
  script << "                {\n";
  script << "                    Debug.LogWarning(\"TrenchBroom sync cannot create "
            "material because no supported shader was found: \" + MaterialNames[i]);\n";
  script << "                    continue;\n";
  script << "                }\n";
  script << "                material = new Material(shader);\n";
  script << "                material.name = MaterialNames[i];\n";
  script << "                if (i < TexturePaths.Length && "
            "!string.IsNullOrEmpty(TexturePaths[i]))\n";
  script << "                {\n";
  script << "                    var texture = "
            "AssetDatabase.LoadAssetAtPath<Texture2D>(TexturePaths[i]);\n";
  script << "                    if (texture != null)\n";
  script << "                    {\n";
  script << "                        material.mainTexture = texture;\n";
  script << "                        if (material.HasProperty(\"_BaseMap\")) "
            "material.SetTexture(\"_BaseMap\", texture);\n";
  script << "                        if (material.HasProperty(\"_MainTex\")) "
            "material.SetTexture(\"_MainTex\", texture);\n";
  script << "                    }\n";
  script << "                }\n";
  script << "                AssetDatabase.CreateAsset(material, materialPath);\n";
  script << "                EditorUtility.SetDirty(material);\n";
  script << "            }\n";
  script << "            result[MaterialNames[i]] = material;\n";
  script << "        }\n";
  script << "        return result;\n";
  script << "    }\n\n";
  script << "    private static Shader FindShader()\n";
  script << "    {\n";
  script << "        var shader = Shader.Find(\"Universal Render Pipeline/Lit\");\n";
  script << "        if (shader == null) shader = Shader.Find(\"Standard\");\n";
  script << "        if (shader == null) shader = Shader.Find(\"Diffuse\");\n";
  script << "        return shader;\n";
  script << "    }\n\n";
  script << "    private static void AssignMaterials(GameObject root, Dictionary<string, "
            "Material> materials)\n";
  script << "    {\n";
  script
    << "        foreach (var renderer in root.GetComponentsInChildren<Renderer>(true))\n";
  script << "        {\n";
  script << "            var rendererMaterials = renderer.sharedMaterials;\n";
  script << "            var changed = false;\n";
  script << "            for (var i = 0; i < rendererMaterials.Length; ++i)\n";
  script << "            {\n";
  script << "                var source = rendererMaterials[i];\n";
  script << "                if (source == null) continue;\n";
  script << "                var name = NormalizeMaterialName(source.name);\n";
  script << "                if (materials.TryGetValue(name, out var replacement))\n";
  script << "                {\n";
  script << "                    rendererMaterials[i] = replacement;\n";
  script << "                    changed = true;\n";
  script << "                }\n";
  script << "            }\n";
  script << "            if (changed)\n";
  script << "            {\n";
  script << "                renderer.sharedMaterials = rendererMaterials;\n";
  script << "            }\n";
  script << "        }\n";
  script << "    }\n\n";
  script << "    private static string NormalizeMaterialName(string value)\n";
  script << "    {\n";
  script << "        if (string.IsNullOrEmpty(value)) return string.Empty;\n";
  script << "        const string instanceSuffix = \" (Instance)\";\n";
  script << "        if (value.EndsWith(instanceSuffix))\n";
  script << "        {\n";
  script
    << "            value = value.Substring(0, value.Length - instanceSuffix.Length);\n";
  script << "        }\n";
  script << "        return value;\n";
  script << "    }\n";
  script << "}\n";
}

std::string warningSummary(const std::set<std::string>& warnings)
{
  if (warnings.empty())
  {
    return "";
  }

  auto stream = std::ostringstream{};
  auto count = size_t{0};
  for (const auto& warning : warnings)
  {
    if (count >= 8u)
    {
      stream << "\n...and " << (warnings.size() - count) << " more warning(s).";
      break;
    }
    stream << "\n- " << warning;
    ++count;
  }
  return stream.str();
}

} // namespace

UnitySyncDialog::UnitySyncDialog(MapWindow* mapWindow)
  : QDialog{mapWindow}
  , m_mapWindow{mapWindow}
{
  contract_pre(m_mapWindow != nullptr);

  createGui();
  resize(560, 0);
}

void UnitySyncDialog::createGui()
{
  setWindowIconTB(this);
  setWindowTitle("Unity Sync");

  auto* header = new DialogHeader{tr("Sync Map to Unity")};

  auto* formLayout = new FormWithSectionsLayout{};
  formLayout->setContentsMargins(0, 20, 0, 20);
  formLayout->setHorizontalSpacing(LayoutConstants::WideHMargin);
  formLayout->setVerticalSpacing(LayoutConstants::MediumVMargin);
  formLayout->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

  formLayout->addSection(
    tr("Unity Project Folder"),
    tr("Choose a folder below Assets, or choose the Unity project root."));

  auto* targetFolderLayout = new QHBoxLayout{};
  targetFolderLayout->setContentsMargins(0, 0, 0, 0);
  targetFolderLayout->setSpacing(LayoutConstants::MediumHMargin);

  m_targetFolderEdit = new QLineEdit{};
  m_targetFolderEdit->setPlaceholderText(tr("Unity Assets folder or Unity project root"));
  targetFolderLayout->addWidget(m_targetFolderEdit);

  m_browseTargetFolderButton = new QPushButton{};
  m_browseTargetFolderButton->setText(tr("Browse..."));
  targetFolderLayout->addWidget(m_browseTargetFolderButton);

  formLayout->addRow(tr("Folder"), targetFolderLayout);

  m_copyTexturesCheckBox = new QCheckBox{};
  m_copyTexturesCheckBox->setText(tr("Copy and convert textures"));
  m_copyTexturesCheckBox->setChecked(true);
  formLayout->addRow("", m_copyTexturesCheckBox);

  m_separateBrushesCheckBox = new QCheckBox{};
  m_separateBrushesCheckBox->setText(tr("Keep brushes separate inside one prefab"));
  m_separateBrushesCheckBox->setChecked(false);
  formLayout->addRow("", m_separateBrushesCheckBox);

  m_scaleFactorSpinBox = new QDoubleSpinBox{};
  m_scaleFactorSpinBox->setDecimals(4);
  m_scaleFactorSpinBox->setRange(0.0001, 1000.0);
  m_scaleFactorSpinBox->setSingleStep(0.1);
  m_scaleFactorSpinBox->setValue(0.1);
  formLayout->addRow(tr("Scale factor"), m_scaleFactorSpinBox);

  m_statusLabel = new QLabel{};
  m_statusLabel->setWordWrap(true);
  m_statusLabel->setText(
    tr("The map will be exported as one OBJ, one material set, and one Unity prefab."));
  formLayout->addRow(tr("Output"), m_statusLabel);

  auto* innerLayout = new QVBoxLayout{};
  innerLayout->setContentsMargins(0, 0, 0, 0);
  innerLayout->setSpacing(0);
  innerLayout->addWidget(header);
  innerLayout->addWidget(new BorderLine{});
  innerLayout->addLayout(formLayout);

  auto* outerLayout = new QVBoxLayout{};
  outerLayout->setContentsMargins(0, 0, 0, 0);
  outerLayout->setSpacing(LayoutConstants::MediumVMargin);
  outerLayout->addLayout(innerLayout);

  auto* buttonBox = new QDialogButtonBox{};
  m_closeButton = buttonBox->addButton(QDialogButtonBox::Cancel);
  m_syncButton = buttonBox->addButton(tr("Sync"), QDialogButtonBox::AcceptRole);
  m_syncButton->setDefault(true);
  outerLayout->addLayout(wrapDialogButtonBox(buttonBox));

  insertTitleBarSeparator(outerLayout);

  setLayout(outerLayout);

  connect(m_closeButton, &QPushButton::clicked, this, &UnitySyncDialog::close);
  connect(m_syncButton, &QPushButton::clicked, this, &UnitySyncDialog::sync);
  connect(m_browseTargetFolderButton, &QPushButton::clicked, this, [&]() {
    const auto newFolder = QFileDialog::getExistingDirectory(
      this, tr("Choose Unity Project or Assets Folder"), m_targetFolderEdit->text());
    if (!newFolder.isEmpty())
    {
      m_targetFolderEdit->setText(newFolder);
    }
  });
}

void UnitySyncDialog::updateDefaultPath()
{
  if (!m_targetFolderEdit->text().isEmpty())
  {
    return;
  }

  const auto settingsValue = QSettings{}.value(TargetFolderSetting).toString();
  if (!settingsValue.isEmpty())
  {
    m_targetFolderEdit->setText(settingsValue);
  }
}

void UnitySyncDialog::sync()
{
  const auto targetFolder =
    resolveUnityTargetFolder(pathFromQString(m_targetFolderEdit->text()));
  if (!targetFolder)
  {
    QMessageBox::warning(
      this,
      tr("Unity Sync"),
      tr("Choose a Unity project root, an Assets folder, or a folder inside Assets."));
    return;
  }

  QSettings{}.setValue(TargetFolderSetting, pathAsQString(targetFolder->folder));

  try
  {
    auto& map = m_mapWindow->document().map();
    const auto keepBrushesSeparate = m_separateBrushesCheckBox->isChecked();
    const auto prefabScale = m_scaleFactorSpinBox->value();
    const auto brushNodes =
      keepBrushesSeparate ? collectBrushNodes(map) : std::vector<mdl::BrushNode*>{};
    if (keepBrushesSeparate && brushNodes.empty())
    {
      QMessageBox::information(
        this, tr("Unity Sync"), tr("There are no brushes to sync."));
      return;
    }

    const auto modelsPath = targetFolder->folder / "Models";
    const auto materialsPath = targetFolder->folder / "Materials";
    const auto texturesPath = targetFolder->folder / "Textures";
    const auto prefabsPath = targetFolder->folder / "Prefabs";
    const auto editorPath = targetFolder->folder / "Editor";
    std::filesystem::create_directories(modelsPath);
    std::filesystem::create_directories(materialsPath);
    std::filesystem::create_directories(texturesPath);
    std::filesystem::create_directories(prefabsPath);
    std::filesystem::create_directories(editorPath);
    removeOldPerBrushExports(modelsPath, prefabsPath);

    auto progress = QProgressDialog{
      tr("Syncing map to Unity..."),
      tr("Cancel"),
      0,
      keepBrushesSeparate ? static_cast<int>(brushNodes.size() + 4u) : 5,
      this};
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0);

    progress.setLabelText(tr("Collecting materials..."));
    progress.setValue(0);
    QApplication::processEvents();

    const auto surfaceMaterials = collectSurfaceMaterials(map);
    const auto objMaterialNames = buildObjMaterialNames(surfaceMaterials);
    auto warnings = std::set<std::string>{};

    if (progress.wasCanceled())
    {
      return;
    }

    progress.setLabelText(tr("Copying and converting textures..."));
    progress.setValue(1);
    QApplication::processEvents();

    const auto materialTexturePaths = exportMaterialTextures(
      surfaceMaterials,
      objMaterialNames,
      texturesPath,
      m_copyTexturesCheckBox->isChecked(),
      warnings);

    if (progress.wasCanceled())
    {
      return;
    }

    const auto prefabStem = std::string{"TrenchBroomMap"};
    auto modelAssets = std::vector<UnityModelAsset>{};

    progress.setLabelText(
      keepBrushesSeparate ? tr("Exporting brush models...")
                          : tr("Exporting one map model..."));
    progress.setValue(2);
    QApplication::processEvents();

    if (keepBrushesSeparate)
    {
      modelAssets.reserve(brushNodes.size());
      for (size_t i = 0; i < brushNodes.size(); ++i)
      {
        if (progress.wasCanceled())
        {
          return;
        }

        const auto stem = brushFileStem(i + 1u);
        const auto objPath = modelsPath / (stem + ".obj");
        auto mtlPath = objPath;
        mtlPath.replace_extension(".mtl");

        progress.setLabelText(
          tr("Exporting brush %1 of %2...").arg(i + 1u).arg(brushNodes.size()));
        progress.setValue(static_cast<int>(i + 2u));
        QApplication::processEvents();

        exportBrushObj(map, *brushNodes[i], objPath, mtlPath);
        rewriteObjMaterialNames(objPath, objMaterialNames);
        rewriteMtlMaterialNamesAndTexturePaths(
          mtlPath, materialTexturePaths, objMaterialNames);
        modelAssets.push_back(
          UnityModelAsset{
            targetFolder->assetPath + "/Models/" + objPath.filename().generic_string(),
            stem});
      }
    }
    else
    {
      const auto objPath = modelsPath / (prefabStem + ".obj");
      auto mtlPath = objPath;
      mtlPath.replace_extension(".mtl");

      exportMapObj(map, objPath, mtlPath);
      rewriteObjMaterialNames(objPath, objMaterialNames);
      rewriteMtlMaterialNamesAndTexturePaths(
        mtlPath, materialTexturePaths, objMaterialNames);
      modelAssets.push_back(
        UnityModelAsset{
          targetFolder->assetPath + "/Models/" + objPath.filename().generic_string(),
          prefabStem});
    }

    if (progress.wasCanceled())
    {
      return;
    }

    progress.setLabelText(tr("Writing Unity importer..."));
    progress.setValue(keepBrushesSeparate ? static_cast<int>(brushNodes.size() + 2u) : 3);
    QApplication::processEvents();

    const auto prefabAssetPath =
      targetFolder->assetPath + "/Prefabs/" + prefabStem + ".prefab";
    const auto materialAssets = buildUnityMaterialAssets(
      surfaceMaterials, objMaterialNames, materialTexturePaths, targetFolder->assetPath);

    const auto className =
      "TrenchBroomUnitySyncImporter_" + fnv1aHex(targetFolder->assetPath);
    writeUnityImporterScript(
      editorPath / (className + ".cs"),
      className,
      modelAssets,
      prefabAssetPath,
      prefabScale,
      materialAssets);

    progress.setValue(keepBrushesSeparate ? static_cast<int>(brushNodes.size() + 4u) : 5);
    progress.close();

    const auto warningsText = warningSummary(warnings);
    QMessageBox::information(
      this,
      tr("Unity Sync"),
      QString::fromStdString(
        std::string{
          keepBrushesSeparate ? "Synced one prefab with separate brush objects to:\n"
                              : "Synced one map model and one prefab to:\n"}
        + targetFolder->folder.string()
        + "\n\nUnity will generate/update prefab assets after it imports the files."
        + (warningsText.empty() ? "" : "\n\nWarnings:" + warningsText)));
    close();
  }
  catch (const std::exception& e)
  {
    QMessageBox::critical(
      this,
      tr("Unity Sync"),
      tr("Sync failed:\n%1").arg(QString::fromStdString(e.what())));
  }
}

} // namespace tb::ui

/*
 Copyright (C) 2010 Kristian Duske

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

#include "render/FaceRenderer.h"

#include "PreferenceManager.h"
#include "Preferences.h"
#include "gl/ActiveShader.h"
#include "gl/Camera.h"
#include "gl/GlInterface.h"
#include "gl/Material.h"
#include "gl/MaterialRenderFunc.h"
#include "gl/PrimType.h"
#include "gl/Shaders.h"
#include "gl/Texture.h"
#include "render/BrushRendererArrays.h"
#include "render/RenderBatch.h"
#include "render/RenderContext.h"

#include "vm/util.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>

namespace tb::render
{

namespace
{

constexpr auto MaxPreviewLights = size_t{32};

std::string lowercase(std::string value)
{
  std::ranges::transform(value, value.begin(), [](const unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

bool hasSkySurfaceParm(const gl::Material* material)
{
  if (!material)
  {
    return false;
  }

  const auto& surfaceParms = material->surfaceParms();
  return surfaceParms.find("sky") != std::end(surfaceParms);
}

bool hasSkyTextureName(const gl::Material* material)
{
  if (!material)
  {
    return false;
  }

  auto name = lowercase(material->name());
  std::replace(name.begin(), name.end(), '\\', '/');

  const auto lastSeparator = name.find_last_of('/');
  const auto basename =
    lastSeparator == std::string::npos ? name : name.substr(lastSeparator + 1u);

  return basename.rfind("sky", 0) == 0 || name == "common/sky" || name.ends_with("/sky");
}

bool isPreviewSkyMaterial(const gl::Material* material)
{
  return hasSkySurfaceParm(material) || hasSkyTextureName(material);
}

bool isSplitQuakeSkyMaterial(const gl::Material* material)
{
  if (const auto* texture = gl::getTexture(material))
  {
    return texture->width() >= texture->height() * 2u;
  }
  return false;
}

float gridRotationRadians()
{
  return static_cast<float>(vm::to_radians(
    static_cast<double>(std::clamp(pref(Preferences::GridRotation), 0.0f, 90.0f))));
}

class RenderFunc : public gl::MaterialRenderFunc
{
private:
  gl::ActiveShader& m_shader;
  bool m_applyMaterial;
  Color m_defaultColor;
  int m_minFilter;
  int m_magFilter;

public:
  RenderFunc(
    gl::ActiveShader& shader,
    const bool applyMaterial,
    Color defaultColor,
    const int minFilter,
    const int magFilter)
    : m_shader{shader}
    , m_applyMaterial{applyMaterial}
    , m_defaultColor{std::move(defaultColor)}
    , m_minFilter{minFilter}
    , m_magFilter{magFilter}
  {
  }

  void before(gl::Gl& gl, const gl::Material* material) override
  {
    if (const auto* texture = gl::getTexture(material))
    {
      material->activate(gl, m_minFilter, m_magFilter);
      m_shader.set("ApplyMaterial", m_applyMaterial);
      m_shader.set("Color", texture->averageColor());
    }
    else
    {
      m_shader.set("ApplyMaterial", false);
      m_shader.set("Color", m_defaultColor);
    }
  }

  void after(gl::Gl& gl, const gl::Material* material) override
  {
    if (material)
    {
      material->deactivate(gl);
    }
  }
};

} // namespace

FaceRenderer::FaceRenderer() = default;

FaceRenderer::FaceRenderer(
  std::shared_ptr<BrushVertexArray> vertexArray,
  std::shared_ptr<MaterialToBrushIndicesMap> indexArrayMap,
  Color faceColor)
  : m_vertexArray{std::move(vertexArray)}
  , m_indexArrayMap{std::move(indexArrayMap)}
  , m_faceColor{std::move(faceColor)}
{
}

void FaceRenderer::setGrayscale(const bool grayscale)
{
  m_grayscale = grayscale;
}

void FaceRenderer::setTint(const bool tint)
{
  m_tint = tint;
}

void FaceRenderer::setTintColor(const Color& color)
{
  m_tintColor = color;
}

void FaceRenderer::setAlpha(const float alpha)
{
  m_alpha = alpha;
}

void FaceRenderer::render(RenderBatch& renderBatch)
{
  renderBatch.add(this);
}

void FaceRenderer::prepare(gl::Gl& gl, gl::VboManager& vboManager)
{
  m_vertexArray->prepare(gl, vboManager);

  for (const auto& [material, brushIndexHolderPtr] : *m_indexArrayMap)
  {
    brushIndexHolderPtr->prepare(gl, vboManager);
  }
}

void FaceRenderer::render(RenderContext& context)
{
  auto& gl = context.gl();

  auto& shaderManager = context.shaderManager();
  auto shader = gl::ActiveShader{gl, shaderManager, gl::Shaders::FaceShader};

  if (!m_indexArrayMap->empty() && m_vertexArray->setup(gl, shader.program()))
  {
    auto& prefs = PreferenceManager::instance();

    const auto applyMaterial = context.showMaterials();
    const auto shadeFaces = context.shadeFaces();
    const auto showFog = context.showFog();
    const auto gridAlpha = prefs.get(Preferences::GridAlpha);
    const auto renderGrid = context.showGrid() && gridAlpha > 0.001f;

    gl.enable(GL_TEXTURE_2D);
    gl.activeTexture(GL_TEXTURE0);
    shader.set("Brightness", prefs.get(Preferences::Brightness));
    shader.set("RenderGrid", renderGrid);
    shader.set("GridSize", static_cast<float>(context.gridSize()));
    shader.set("GridAlpha", renderGrid ? gridAlpha : 0.0f);
    const auto gridRotation = renderGrid ? gridRotationRadians() : 0.0f;
    shader.set("GridRotationCos", std::cos(gridRotation));
    shader.set("GridRotationSin", std::sin(gridRotation));
    shader.set(
      "GridUseUnified3D", renderGrid && prefs.get(Preferences::GridUseUnified3D));
    shader.set("ApplyMaterial", applyMaterial);
    shader.set("Material", 0);
    shader.set("ApplyTinting", m_tint);
    if (m_tint)
    {
      shader.set("TintColor", m_tintColor);
    }
    shader.set("GrayScale", m_grayscale);
    shader.set("CameraPosition", context.camera().position());
    shader.set("ShadeFaces", shadeFaces);
    const auto& previewLights = context.previewLights();
    const auto previewLightCount = std::min(previewLights.size(), MaxPreviewLights);
    shader.set("PreviewLighting", context.previewLighting());
    shader.set("PreviewLightCount", previewLightCount);
    for (size_t i = 0; i < previewLightCount; ++i)
    {
      const auto indexSuffix = "[" + std::to_string(i) + "]";
      shader.set("PreviewLightPositions" + indexSuffix, previewLights[i].position);
      shader.set("PreviewLightColors" + indexSuffix, previewLights[i].color);
      shader.set("PreviewLightRadii" + indexSuffix, previewLights[i].radius);
      shader.set("PreviewLightScales" + indexSuffix, previewLights[i].scale);
    }
    shader.set("ShowFog", showFog);
    shader.set("Alpha", m_alpha);
    shader.set("EnableMasked", false);
    shader.set("ShowSoftMapBounds", !context.softMapBounds().is_empty());
    shader.set("SoftMapBoundsMin", context.softMapBounds().min);
    shader.set("SoftMapBoundsMax", context.softMapBounds().max);
    shader.set(
      "SoftMapBoundsColor",
      RgbaF{prefs.get(Preferences::SoftMapBoundsColor).to<RgbF>(), 0.1f});

    auto func = RenderFunc{
      shader,
      applyMaterial,
      m_faceColor,
      context.minFilterMode(),
      context.magFilterMode()};

    if (m_alpha < 1.0f)
    {
      gl.depthMask(GL_FALSE);
    }
    for (const auto& [material, brushIndexHolderPtr] : *m_indexArrayMap)
    {
      if (brushIndexHolderPtr->hasValidIndices())
      {
        const auto* texture = getTexture(material);
        const auto enableMasked = texture && texture->mask() == gl::TextureMask::On;

        // set any per-material uniforms
        shader.set(
          "PreviewSky",
          context.previewLightingEnabled() && isPreviewSkyMaterial(material));
        shader.set("PreviewSkySplit", isSplitQuakeSkyMaterial(material));
        shader.set("PreviewSkyTime", context.timeSeconds());
        shader.set("EnableMasked", enableMasked);

        func.before(gl, material);
        brushIndexHolderPtr->setup(gl);
        brushIndexHolderPtr->render(gl, gl::PrimType::Triangles);
        brushIndexHolderPtr->cleanup(gl);
        func.after(gl, material);
      }
    }
    if (m_alpha < 1.0f)
    {
      gl.depthMask(GL_TRUE);
    }
    m_vertexArray->cleanup(gl, shader.program());
  }
}

} // namespace tb::render

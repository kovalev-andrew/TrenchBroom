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

#include "ui/MaterialCollectionEditor.h"

#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidgetItem>
#include <QListWidget>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSettings>
#include <QToolButton>
#include <QVBoxLayout>

#include "PreferenceManager.h"
#include "fs/DiskFileSystem.h"
#include "fs/DiskIO.h"
#include "fs/File.h"
#include "fs/PathInfo.h"
#include "fs/PathMatcher.h"
#include "fs/TraversalMode.h"
#include "gl/Texture.h"
#include "gl/TextureBuffer.h"
#include "mdl/Entity.h"
#include "mdl/EntityProperties.h"
#include "mdl/GameInfo.h"
#include "mdl/LoadMaterialCollections.h"
#include "mdl/LoadTexture.h"
#include "mdl/Map.h"
#include "mdl/Map_Assets.h"
#include "mdl/Map_Entities.h"
#include "mdl/MaterialUtils.h"
#include "mdl/Palette.h"
#include "mdl/WadPropertyUtils.h"
#include "mdl/WorldNode.h" // IWYU pragma: keep
#include "ui/BitmapButton.h"
#include "ui/BorderLine.h"
#include "ui/FileDialogDefaultDir.h"
#include "ui/ImageUtils.h"
#include "ui/MapDocument.h"
#include "ui/MiniToolBarLayout.h"
#include "ui/QPathUtils.h"
#include "ui/TitledPanel.h"
#include "ui/ViewConstants.h"

#include "kd/ranges/to.h"
#include "kd/string_utils.h"
#include "kd/vector_utils.h"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <ios>
#include <iterator>
#include <limits>
#include <ostream>
#include <ranges>

namespace tb::ui
{
namespace
{

const auto TextureFolderExtensions =
  std::vector<std::filesystem::path>{".png", ".jpg", ".jpeg"};
const auto TextureExportPaletteSetting = "MaterialBrowser/TextureFolderExportPalette";

std::filesystem::path textureExportPalettePath()
{
  const auto path = QSettings{}.value(TextureExportPaletteSetting).toString();
  return path.isEmpty() ? std::filesystem::path{} : pathFromQString(path);
}

void setTextureExportPalettePath(const std::filesystem::path& path)
{
  QSettings{}.setValue(TextureExportPaletteSetting, pathAsQString(path));
}

Result<mdl::Palette> loadPaletteFile(const std::filesystem::path& path)
{
  if (path.empty())
  {
    return Error{"Texture export palette is not configured"};
  }

  if (fs::Disk::pathInfo(path) != fs::PathInfo::File)
  {
    return Error{"Texture export palette file does not exist"};
  }

  return fs::Disk::openFile(path)
         | kdl::and_then([&](auto file) { return mdl::loadPalette(*file, path); });
}

bool isTextureFolderPath(const std::filesystem::path& path)
{
  return path.is_absolute() && fs::Disk::pathInfo(path) == fs::PathInfo::Directory;
}

class PalettePreview : public QWidget
{
private:
  std::vector<unsigned char> m_rgbaData;

public:
  explicit PalettePreview(QWidget* parent = nullptr)
    : QWidget{parent}
  {
    setMinimumSize(256, 256);
  }

  void setPalette(const mdl::Palette& palette)
  {
    m_rgbaData = palette.rgbaData(mdl::PaletteTransparency::Opaque);
    update();
  }

  void clearPalette()
  {
    m_rgbaData.clear();
    update();
  }

protected:
  void paintEvent(QPaintEvent*) override
  {
    auto painter = QPainter{this};
    painter.fillRect(rect(), palette().base());

    if (m_rgbaData.empty())
    {
      painter.setPen(palette().text().color());
      painter.drawText(rect(), Qt::AlignCenter, tr("No palette"));
      return;
    }

    const auto cellWidth = width() / 16;
    const auto cellHeight = height() / 16;
    for (auto i = 0u; i < 256u && i * 4u + 3u < m_rgbaData.size(); ++i)
    {
      const auto x = int(i % 16u) * cellWidth;
      const auto y = int(i / 16u) * cellHeight;
      const auto color = QColor{
        int(m_rgbaData[i * 4u + 0u]),
        int(m_rgbaData[i * 4u + 1u]),
        int(m_rgbaData[i * 4u + 2u]),
        int(m_rgbaData[i * 4u + 3u])};
      painter.fillRect(QRect{x, y, cellWidth, cellHeight}, color);
    }

    painter.setPen(QColor{0, 0, 0, 80});
    for (auto i = 0; i <= 16; ++i)
    {
      painter.drawLine(i * cellWidth, 0, i * cellWidth, 16 * cellHeight);
      painter.drawLine(0, i * cellHeight, 16 * cellWidth, i * cellHeight);
    }
  }
};

class TextureExportPaletteDialog : public QDialog
{
private:
  QLabel* m_pathLabel = nullptr;
  PalettePreview* m_preview = nullptr;
  QDialogButtonBox* m_buttons = nullptr;
  std::filesystem::path m_palettePath;

public:
  explicit TextureExportPaletteDialog(QWidget* parent = nullptr)
    : QDialog{parent}
  {
    setWindowTitle(tr("Pallete"));

    m_pathLabel = new QLabel{tr("No palette selected")};
    m_pathLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_pathLabel->setWordWrap(true);

    auto* browseButton = new QPushButton{tr("Choose...")};
    m_preview = new PalettePreview{};
    m_buttons = new QDialogButtonBox{QDialogButtonBox::Ok | QDialogButtonBox::Cancel};

    auto* pathLayout = new QHBoxLayout{};
    pathLayout->addWidget(m_pathLabel, 1);
    pathLayout->addWidget(browseButton, 0);

    auto* layout = new QVBoxLayout{};
    layout->addLayout(pathLayout);
    layout->addWidget(m_preview, 1);
    layout->addWidget(m_buttons, 0);
    setLayout(layout);

    connect(browseButton, &QAbstractButton::clicked, this, [&]() {
      const auto currentPath =
        m_palettePath.empty()
          ? fileDialogDefaultDirectory(FileDialogDir::MaterialCollection)
          : pathAsQString(m_palettePath.parent_path());
      const auto pathStr = QFileDialog::getOpenFileName(
        this,
        tr("Choose Texture Export Palette"),
        currentPath,
        tr("Palette files (*.lmp *.pcx *.bmp);;All files (*.*)"));
      if (!pathStr.isEmpty())
      {
        setPalettePath(pathFromQString(pathStr), true);
      }
    });
    connect(m_buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    setPalettePath(textureExportPalettePath(), false);
  }

  const std::filesystem::path& palettePath() const { return m_palettePath; }

private:
  void setPalettePath(const std::filesystem::path& path, const bool showErrors)
  {
    m_palettePath = path;
    m_pathLabel->setText(path.empty() ? tr("No palette selected") : pathAsQString(path));

    loadPaletteFile(path) | kdl::transform([&](const auto& palette) {
      m_preview->setPalette(palette);
      m_buttons->button(QDialogButtonBox::Ok)->setEnabled(true);
    }) | kdl::transform_error([&](const auto& e) {
      m_preview->clearPalette();
      m_buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
      if (showErrors)
      {
        QMessageBox::warning(
          this,
          tr("Texture Export Palette"),
          tr("Could not load palette:\n%1").arg(QString::fromStdString(e.msg)));
      }
    });
  }
};

void writeInt32(std::ostream& stream, const int32_t value)
{
  stream.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

void writeUInt16(std::ostream& stream, const uint16_t value)
{
  stream.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

void writePaddedName(std::ostream& stream, const std::string& name)
{
  auto buffer = std::array<char, 16u>{};
  std::memcpy(buffer.data(), name.data(), std::min(name.size(), buffer.size() - 1u));
  stream.write(buffer.data(), std::streamsize(buffer.size()));
}

unsigned char samplePaletteIndex(
  const gl::Texture& texture,
  const mdl::Palette& palette,
  const size_t x,
  const size_t y,
  const mdl::PaletteTransparency transparency)
{
  const auto& buffer = texture.buffersIfLoaded().front();
  const auto* data = buffer.data();
  const auto pixel = (y * texture.width() + x) * 4u;
  const auto format = texture.format();
  const auto r = data[pixel + (format == GL_BGRA ? 2u : 0u)];
  const auto g = data[pixel + 1u];
  const auto b = data[pixel + (format == GL_BGRA ? 0u : 2u)];
  const auto a = data[pixel + 3u];
  return palette.nearestIndex(r, g, b, a, transparency);
}

std::vector<unsigned char> buildMipLevel(
  const gl::Texture& texture,
  const mdl::Palette& palette,
  const size_t mipLevel,
  const mdl::PaletteTransparency transparency)
{
  const auto width = texture.width();
  const auto height = texture.height();
  const auto mipWidth = std::max<size_t>(1u, width >> mipLevel);
  const auto mipHeight = std::max<size_t>(1u, height >> mipLevel);

  auto result = std::vector<unsigned char>{};
  result.reserve(mipWidth * mipHeight);

  for (size_t y = 0u; y < mipHeight; ++y)
  {
    const auto sourceY = std::min(height - 1u, y * height / mipHeight);
    for (size_t x = 0u; x < mipWidth; ++x)
    {
      const auto sourceX = std::min(width - 1u, x * width / mipWidth);
      result.push_back(
        samplePaletteIndex(texture, palette, sourceX, sourceY, transparency));
    }
  }

  return result;
}

struct WadLump
{
  std::string name;
  std::vector<unsigned char> data;
};

Result<std::vector<WadLump>> buildTextureFolderWadLumps(
  const std::filesystem::path& folderPath, const mdl::Palette& palette)
{
  auto folderFs = fs::DiskFileSystem{folderPath};
  return folderFs.find(
           std::filesystem::path{},
           fs::TraversalMode::Recursive,
           fs::makeExtensionPathMatcher(TextureFolderExtensions))
         | kdl::and_then([&](auto texturePaths) {
             texturePaths = kdl::vec_sort(std::move(texturePaths));
             const auto materialNames = mdl::textureFolderMaterialNames(texturePaths);

             auto lumps = std::vector<WadLump>{};
             lumps.reserve(texturePaths.size());

             for (size_t i = 0u; i < texturePaths.size(); ++i)
             {
               const auto& texturePath = texturePaths[i];
               const auto& name = materialNames[i];

               mdl::loadTexture(texturePath, name, folderFs, std::nullopt)
                 | kdl::transform([&](auto texture) {
                     if (
                       texture.buffersIfLoaded().empty()
                       || (texture.format() != GL_RGBA && texture.format() != GL_BGRA))
                     {
                       return;
                     }

                     const auto transparency =
                       texture.mask() == gl::TextureMask::On
                         ? mdl::PaletteTransparency::Index255Transparent
                         : mdl::PaletteTransparency::Opaque;
                     auto mip0 = buildMipLevel(texture, palette, 0u, transparency);
                     auto mip1 = buildMipLevel(texture, palette, 1u, transparency);
                     auto mip2 = buildMipLevel(texture, palette, 2u, transparency);
                     auto mip3 = buildMipLevel(texture, palette, 3u, transparency);

                     const auto headerSize = size_t{40u};
                     const auto offset0 = headerSize;
                     const auto offset1 = offset0 + mip0.size();
                     const auto offset2 = offset1 + mip1.size();
                     const auto offset3 = offset2 + mip2.size();

                     auto data = std::vector<unsigned char>{};
                     data.resize(headerSize);
                     auto writeOffset = size_t{0u};
                     const auto append = [&](const auto* bytes, const size_t size) {
                       std::memcpy(data.data() + writeOffset, bytes, size);
                       writeOffset += size;
                     };
                     const auto appendInt32 = [&](const int32_t value) {
                       append(
                         reinterpret_cast<const unsigned char*>(&value), sizeof(value));
                     };

                     auto nameBuffer = std::array<char, 16u>{};
                     std::memcpy(
                       nameBuffer.data(),
                       name.data(),
                       std::min(name.size(), nameBuffer.size() - 1u));
                     append(
                       reinterpret_cast<const unsigned char*>(nameBuffer.data()),
                       nameBuffer.size());
                     appendInt32(static_cast<int32_t>(texture.width()));
                     appendInt32(static_cast<int32_t>(texture.height()));
                     appendInt32(static_cast<int32_t>(offset0));
                     appendInt32(static_cast<int32_t>(offset1));
                     appendInt32(static_cast<int32_t>(offset2));
                     appendInt32(static_cast<int32_t>(offset3));

                     data.insert(data.end(), mip0.begin(), mip0.end());
                     data.insert(data.end(), mip1.begin(), mip1.end());
                     data.insert(data.end(), mip2.begin(), mip2.end());
                     data.insert(data.end(), mip3.begin(), mip3.end());

                     lumps.push_back(WadLump{name, std::move(data)});
                   })
                 | kdl::transform_error([](auto) {});
             }

             if (lumps.empty())
             {
               return Result<std::vector<WadLump>>{
                 Error{"Could not convert any PNG/JPG textures into WAD mip textures"}};
             }

             return Result<std::vector<WadLump>>{std::move(lumps)};
           });
}

Result<std::filesystem::path> writeTextureFolderWad(
  const std::filesystem::path& folderPath, const mdl::Palette& palette)
{
  const auto wadPath = folderPath / "__tb_texture_folder.wad";

  return buildTextureFolderWadLumps(folderPath, palette)
         | kdl::and_then([&](const auto& lumps) {
             return fs::Disk::withOutputStream(
                      wadPath,
                      std::ios_base::out | std::ios_base::binary,
                      [&](auto& stream) {
                        stream.write("WAD2", 4);
                        writeInt32(stream, static_cast<int32_t>(lumps.size()));
                        writeInt32(stream, 0);
                        auto dataOffset = int32_t{12};
                        auto lumpOffsets = std::vector<int32_t>{};
                        lumpOffsets.reserve(lumps.size());

                        for (const auto& lump : lumps)
                        {
                          lumpOffsets.push_back(dataOffset);
                          stream.write(
                            reinterpret_cast<const char*>(lump.data.data()),
                            std::streamsize(lump.data.size()));
                          dataOffset += static_cast<int32_t>(lump.data.size());
                        }

                        const auto directoryOffset = dataOffset;
                        for (size_t i = 0u; i < lumps.size(); ++i)
                        {
                          const auto& lump = lumps[i];
                          writeInt32(stream, lumpOffsets[i]);
                          writeInt32(stream, static_cast<int32_t>(lump.data.size()));
                          writeInt32(stream, static_cast<int32_t>(lump.data.size()));
                          stream.put('D');
                          stream.put('\0');
                          writeUInt16(stream, 0u);
                          writePaddedName(stream, lump.name);
                        }

                        stream.seekp(8);
                        writeInt32(stream, directoryOffset);
                      })
                    | kdl::transform([&]() { return wadPath; });
           });
}

void addWadToWorldspawn(mdl::Map& map, const std::filesystem::path& wadPath)
{
  const auto& materialConfig = map.gameInfo().gameConfig.materialConfig;
  if (!materialConfig.property)
  {
    return;
  }

  auto wadPaths = std::vector<std::string>{};
  if (const auto* existing = map.worldNode().entity().property(*materialConfig.property))
  {
    wadPaths = mdl::splitWadProperty(*existing);
  }

  const auto wadPathStr = wadPath.string();
  if (!kdl::vec_contains(wadPaths, wadPathStr))
  {
    wadPaths.push_back(wadPathStr);
    mdl::setEntityProperty(map, *materialConfig.property, mdl::joinWadProperty(wadPaths));
  }
}

} // namespace

MaterialCollectionEditor::MaterialCollectionEditor(MapDocument& document, QWidget* parent)
  : QWidget{parent}
  , m_document{document}
{
  createGui();
  connectObservers();
  updateAllMaterialCollections();
  updateButtons();
}

bool MaterialCollectionEditor::configureTextureExportPalette()
{
  auto dialog = TextureExportPaletteDialog{this};
  if (dialog.exec() == QDialog::Accepted)
  {
    setTextureExportPalettePath(dialog.palettePath());
    return true;
  }

  return false;
}

void MaterialCollectionEditor::refreshTextureFolders()
{
  auto textureFolderPaths = enabledMaterialCollections()
                            | std::views::filter(isTextureFolderPath)
                            | kdl::ranges::to<std::vector>();

  if (
    !textureFolderPaths.empty()
    && m_document.map().gameInfo().gameConfig.materialConfig.property)
  {
    auto paletteResult = loadPaletteFile(textureExportPalettePath());

    if (paletteResult.is_error())
    {
      auto warning = QMessageBox{
        QMessageBox::Warning,
        tr("Texture Export Palette"),
        tr(
          "Texture folders will be reloaded in the editor, but a palette for texture "
          "export is not configured. Without a palette, the compile WAD will not be "
          "rebuilt and new textures will not appear in game."),
        QMessageBox::NoButton,
        this};

      auto* continueButton = warning.addButton(tr("Continue"), QMessageBox::AcceptRole);
      auto* configureButton = warning.addButton(tr("Configure"), QMessageBox::ActionRole);
      warning.exec();

      if (warning.clickedButton() == configureButton)
      {
        emit showSettingsPanelRequested();
        if (configureTextureExportPalette())
        {
          paletteResult = loadPaletteFile(textureExportPalettePath());
        }
      }

      if (warning.clickedButton() == continueButton || paletteResult.is_error())
      {
        reloadMaterialCollections();
        updateUi();
        return;
      }
    }

    auto exportErrors = std::vector<std::string>{};
    paletteResult | kdl::transform([&](const auto& palette) {
      for (const auto& folderPath : textureFolderPaths)
      {
        writeTextureFolderWad(folderPath, palette)
          | kdl::transform(
            [&](const auto& wadPath) { addWadToWorldspawn(m_document.map(), wadPath); })
          | kdl::transform_error([&](const auto& e) {
              exportErrors.push_back(fmt::format("{}: {}", folderPath.string(), e.msg));
            });
      }
    });

    if (!exportErrors.empty())
    {
      QMessageBox::warning(
        this,
        tr("Refresh Texture Folders"),
        tr("Some texture folders could not be exported to WAD:\n%1")
          .arg(QString::fromStdString(kdl::str_join(exportErrors, "\n"))));
    }
  }

  reloadMaterialCollections();
  updateUi();
}

void MaterialCollectionEditor::addTextureFolder()
{
  const auto folderQStr = QFileDialog::getExistingDirectory(
    this,
    tr("Add Texture Folder"),
    fileDialogDefaultDirectory(FileDialogDir::MaterialCollection),
    QFileDialog::ShowDirsOnly);

  if (folderQStr.isEmpty())
  {
    return;
  }

  updateFileDialogDefaultDirectoryWithDirectory(
    FileDialogDir::MaterialCollection, folderQStr);

  auto folderPath = pathFromQString(folderQStr);
  auto enabledCollections = enabledMaterialCollections();
  enabledCollections.push_back(folderPath);
  enabledCollections = kdl::vec_sort_and_remove_duplicates(std::move(enabledCollections));

  setEnabledMaterialCollections(m_document.map(), enabledCollections);

  if (m_document.map().gameInfo().gameConfig.materialConfig.property)
  {
    auto paletteResult = loadPaletteFile(textureExportPalettePath());

    if (paletteResult.is_error())
    {
      auto warning = QMessageBox{
        QMessageBox::Warning,
        tr("Texture Export Palette"),
        tr(
          "Texture folder was added to the editor, but a palette for texture export is "
          "not configured. Without a palette, the compile WAD will not be generated."),
        QMessageBox::NoButton,
        this};

      auto* continueButton = warning.addButton(tr("Continue"), QMessageBox::AcceptRole);
      auto* configureButton = warning.addButton(tr("Configure"), QMessageBox::ActionRole);
      warning.exec();

      if (warning.clickedButton() == configureButton)
      {
        emit showSettingsPanelRequested();
        if (configureTextureExportPalette())
        {
          paletteResult = loadPaletteFile(textureExportPalettePath());
        }
      }

      if (warning.clickedButton() == continueButton || paletteResult.is_error())
      {
        reloadMaterialCollections();
        updateUi();
        return;
      }
    }

    paletteResult | kdl::and_then([&](const auto& palette) {
      return writeTextureFolderWad(folderPath, palette);
    }) | kdl::transform([&](const auto& wadPath) {
      addWadToWorldspawn(m_document.map(), wadPath);
    }) | kdl::transform_error([&](const auto& e) {
      QMessageBox::warning(
        this,
        tr("Add Texture Folder"),
        tr("Texture folder was added, but a compile WAD could not be generated:\n%1")
          .arg(QString::fromStdString(e.msg)));
    });
  }

  reloadMaterialCollections();
  updateUi();

  const auto updatedEnabledCollections = enabledMaterialCollections();
  const auto it = std::ranges::find(updatedEnabledCollections, folderPath);
  if (it != updatedEnabledCollections.end())
  {
    m_enabledCollectionsList->setCurrentRow(
      int(std::distance(updatedEnabledCollections.begin(), it)),
      QItemSelectionModel::ClearAndSelect);
  }
}

void MaterialCollectionEditor::addSelectedMaterialCollections()
{
  const auto availableCollections = availableMaterialCollections();
  auto enabledCollections = enabledMaterialCollections();

  for (auto* selectedItem : m_availableCollectionsList->selectedItems())
  {
    const auto index = size_t(m_availableCollectionsList->row(selectedItem));
    enabledCollections.push_back(availableCollections[index]);
  }

  enabledCollections = kdl::vec_sort_and_remove_duplicates(std::move(enabledCollections));

  setEnabledMaterialCollections(m_document.map(), enabledCollections);
}

void MaterialCollectionEditor::removeSelectedMaterialCollections()
{
  auto enabledCollections = enabledMaterialCollections();

  auto selections = std::vector<int>{};
  for (auto* selectedItem : m_enabledCollectionsList->selectedItems())
  {
    selections.push_back(m_enabledCollectionsList->row(selectedItem));
  }

  // erase back to front
  for (auto sIt = std::rbegin(selections), sEnd = std::rend(selections); sIt != sEnd;
       ++sIt)
  {
    const auto index = size_t(*sIt);
    enabledCollections = kdl::vec_erase_at(std::move(enabledCollections), index);
  }

  setEnabledMaterialCollections(m_document.map(), enabledCollections);
}

void MaterialCollectionEditor::reloadMaterialCollections()
{
  mdl::reloadMaterialCollections(m_document.map());
}

void MaterialCollectionEditor::availableMaterialCollectionSelectionChanged()
{
  updateButtons();
}

void MaterialCollectionEditor::enabledMaterialCollectionSelectionChanged()
{
  updateButtons();
}

bool MaterialCollectionEditor::canAddMaterialCollections() const
{
  return !m_availableCollectionsList->selectedItems().empty();
}

bool MaterialCollectionEditor::canRemoveMaterialCollections() const
{
  return !m_enabledCollectionsList->selectedItems().empty();
}

bool MaterialCollectionEditor::canReloadMaterialCollections() const
{
  return m_enabledCollectionsList->count() != 0;
}

/**
 * See ModEditor::createGui
 */
void MaterialCollectionEditor::createGui()
{
  auto* availableCollectionsContainer = new TitledPanel{"Available", false, true};
  availableCollectionsContainer->setBackgroundRole(QPalette::Base);
  availableCollectionsContainer->setAutoFillBackground(true);

  m_availableCollectionsList = new QListWidget{};
  m_availableCollectionsList->setSelectionMode(QAbstractItemView::ExtendedSelection);

  auto* availableCollectionsContainerLayout = new QVBoxLayout{};
  availableCollectionsContainerLayout->setContentsMargins(0, 0, 0, 0);
  availableCollectionsContainerLayout->setSpacing(0);
  availableCollectionsContainerLayout->addWidget(m_availableCollectionsList);
  availableCollectionsContainer->getPanel()->setLayout(
    availableCollectionsContainerLayout);

  auto* enabledCollectionsContainer = new TitledPanel{"Enabled", false, true};
  enabledCollectionsContainer->setBackgroundRole(QPalette::Base);
  enabledCollectionsContainer->setAutoFillBackground(true);

  m_enabledCollectionsList = new QListWidget{};
  m_enabledCollectionsList->setSelectionMode(QAbstractItemView::ExtendedSelection);

  auto* enabledCollectionsContainerLayout = new QVBoxLayout{};
  enabledCollectionsContainerLayout->setContentsMargins(0, 0, 0, 0);
  enabledCollectionsContainerLayout->setSpacing(0);
  enabledCollectionsContainerLayout->addWidget(m_enabledCollectionsList);
  enabledCollectionsContainer->getPanel()->setLayout(enabledCollectionsContainerLayout);

  m_addCollectionsButton =
    createBitmapButton("Add.svg", tr("Enable the selected material collections"), this);
  m_removeCollectionsButton = createBitmapButton(
    "Remove.svg", tr("Disable the selected material collections"), this);
  m_reloadCollectionsButton = createBitmapButton(
    "Refresh.svg", tr("Reload all enabled material collections"), this);

  auto* toolBar = createMiniToolBarLayout(
    m_addCollectionsButton,
    m_removeCollectionsButton,
    LayoutConstants::WideHMargin,
    m_reloadCollectionsButton);

  auto* layout = new QGridLayout{};
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);

  layout->addWidget(availableCollectionsContainer, 0, 0);
  layout->addWidget(new BorderLine{BorderLine::Direction::Vertical}, 0, 1, 3, 1);
  layout->addWidget(enabledCollectionsContainer, 0, 2);
  layout->addWidget(new BorderLine{}, 1, 0, 1, 3);
  layout->addLayout(toolBar, 2, 2);

  setLayout(layout);

  updateAllMaterialCollections();

  connect(
    m_availableCollectionsList,
    &QListWidget::itemSelectionChanged,
    this,
    &MaterialCollectionEditor::availableMaterialCollectionSelectionChanged);
  connect(
    m_enabledCollectionsList,
    &QListWidget::itemSelectionChanged,
    this,
    &MaterialCollectionEditor::enabledMaterialCollectionSelectionChanged);
  connect(
    m_availableCollectionsList,
    &QListWidget::itemDoubleClicked,
    this,
    &MaterialCollectionEditor::addSelectedMaterialCollections);
  connect(
    m_enabledCollectionsList,
    &QListWidget::itemDoubleClicked,
    this,
    &MaterialCollectionEditor::removeSelectedMaterialCollections);
  connect(
    m_addCollectionsButton,
    &QAbstractButton::clicked,
    this,
    &MaterialCollectionEditor::addSelectedMaterialCollections);
  connect(
    m_removeCollectionsButton,
    &QAbstractButton::clicked,
    this,
    &MaterialCollectionEditor::removeSelectedMaterialCollections);
  connect(
    m_reloadCollectionsButton,
    &QAbstractButton::clicked,
    this,
    &MaterialCollectionEditor::reloadMaterialCollections);
}

void MaterialCollectionEditor::updateButtons()
{
  m_addCollectionsButton->setEnabled(canAddMaterialCollections());
  m_removeCollectionsButton->setEnabled(canRemoveMaterialCollections());
  m_reloadCollectionsButton->setEnabled(canReloadMaterialCollections());
}

void MaterialCollectionEditor::connectObservers()
{
  m_notifierConnection +=
    m_document.documentWasLoadedNotifier.connect([&] { updateUi(); });
  m_notifierConnection +=
    m_document.documentDidChangeNotifier.connect([&] { updateUi(); });

  auto& prefs = PreferenceManager::instance();
  m_notifierConnection +=
    prefs.preferenceDidChangeNotifier.connect([&](const auto& path) {
      if (path == pref(m_document.map().gameInfo().gamePathPreference))
      {
        updateUi();
      }
    });
}

void MaterialCollectionEditor::updateUi()
{
  updateAllMaterialCollections();
  updateButtons();
}

void MaterialCollectionEditor::updateAllMaterialCollections()
{
  updateAvailableMaterialCollections();
  updateEnabledMaterialCollections();
}

namespace
{

bool isTextureFolderCollectionPath(const std::filesystem::path& path)
{
  return path.is_absolute() && fs::Disk::pathInfo(path) == fs::PathInfo::Directory;
}

void updateListBox(QListWidget* box, const std::vector<std::filesystem::path>& paths)
{
  // We need to block QListWidget::itemSelectionChanged from firing while clearing and
  // rebuilding the list because it will cause debugUIConsistency() to fail, as the
  // number of list items in the UI won't match the document's material collections
  // lists.
  auto blocker = QSignalBlocker{box};

  box->clear();
  for (const auto& path : paths)
  {
    auto* item = new QListWidgetItem{pathAsQString(path)};
    if (isTextureFolderCollectionPath(path))
    {
      item->setIcon(loadSVGIcon("Folder.svg"));
      item->setToolTip(QObject::tr("Texture folder collection"));
    }
    box->addItem(item);
  }
}

} // namespace

void MaterialCollectionEditor::updateAvailableMaterialCollections()
{
  updateListBox(m_availableCollectionsList, availableMaterialCollections());
}

void MaterialCollectionEditor::updateEnabledMaterialCollections()
{
  updateListBox(m_enabledCollectionsList, enabledMaterialCollections());
}

std::vector<std::filesystem::path> MaterialCollectionEditor::
  availableMaterialCollections() const
{
  return disabledMaterialCollections(m_document.map());
}

std::vector<std::filesystem::path> MaterialCollectionEditor::enabledMaterialCollections()
  const
{
  return mdl::enabledMaterialCollections(m_document.map());
}

} // namespace tb::ui

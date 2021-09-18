/*
 * Bittorrent Client using Qt and libtorrent.
 * Copyright (C) 2021  Vladimir Golovnev <glassez@yandex.ru>
 *
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
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 * In addition, as a special exception, the copyright holders give permission to
 * link this program with the OpenSSL project's "OpenSSL" library (or with
 * modified versions of it that use the same license as the "OpenSSL" library),
 * and distribute the linked executables. You must obey the GNU General Public
 * License in all respects for all of the code used other than "OpenSSL".  If you
 * modify file(s), you may extend this exception to your version of the file(s),
 * but you are not obligated to do so. If you do not wish to do so, delete this
 * exception statement from your version.
 */

#include "watchedfolderoptionsdialog.h"

#include <QDir>
#include <QPushButton>

#include "base/bittorrent/session.h"
#include "base/global.h"
#include "base/settingsstorage.h"
#include "base/utils/fs.h"
#include "ui_watchedfolderoptionsdialog.h"
#include "utils.h"

#define SETTINGS_KEY(name) "WatchedFolderOptionsDialog/" name

WatchedFolderOptionsDialog::WatchedFolderOptionsDialog(
        const TorrentFilesWatcher::WatchedFolderOptions &watchedFolderOptions, QWidget *parent)
    : QDialog {parent}
    , m_ui {new Ui::WatchedFolderOptionsDialog}
    , m_savePath {watchedFolderOptions.addTorrentParams.savePath}
    , m_storeDialogSize {SETTINGS_KEY("DialogSize")}
{
    m_ui->setupUi(this);

    m_ui->savePath->setMode(FileSystemPathEdit::Mode::DirectorySave);
    m_ui->savePath->setDialogCaption(tr("Choose save path"));

    connect(m_ui->comboTTM, qOverload<int>(&QComboBox::currentIndexChanged), this, &WatchedFolderOptionsDialog::onTMMChanged);
    connect(m_ui->categoryComboBox, qOverload<int>(&QComboBox::currentIndexChanged), this, &WatchedFolderOptionsDialog::onCategoryChanged);

    m_ui->checkBoxRecursive->setChecked(watchedFolderOptions.recursive);
    populateSavePathComboBox();

    const auto *session = BitTorrent::Session::instance();
    const BitTorrent::AddTorrentParams &torrentParams = watchedFolderOptions.addTorrentParams;
    m_ui->startTorrentCheckBox->setChecked(!torrentParams.addPaused.value_or(session->isAddTorrentPaused()));
    m_ui->comboTTM->setCurrentIndex(torrentParams.useAutoTMM.value_or(!session->isAutoTMMDisabledByDefault()));
    m_ui->contentLayoutComboBox->setCurrentIndex(
        static_cast<int>(torrentParams.contentLayout.value_or(session->torrentContentLayout())));

    // Load categories
    QStringList categories = session->categories().keys();
    std::sort(categories.begin(), categories.end(), Utils::Compare::NaturalLessThan<Qt::CaseInsensitive>());

    if (!torrentParams.category.isEmpty())
        m_ui->categoryComboBox->addItem(torrentParams.category);
    m_ui->categoryComboBox->addItem("");

    for (const QString &category : asConst(categories))
    {
        if (category != torrentParams.category)
            m_ui->categoryComboBox->addItem(category);
    }

    loadState();

    m_ui->buttonBox->button(QDialogButtonBox::Ok)->setFocus();
}

WatchedFolderOptionsDialog::~WatchedFolderOptionsDialog()
{
    saveState();
    delete m_ui;
}

TorrentFilesWatcher::WatchedFolderOptions WatchedFolderOptionsDialog::watchedFolderOptions() const
{
    TorrentFilesWatcher::WatchedFolderOptions watchedFolderOptions;
    watchedFolderOptions.recursive = m_ui->checkBoxRecursive->isChecked();

    BitTorrent::AddTorrentParams &params = watchedFolderOptions.addTorrentParams;
    params.useAutoTMM = (m_ui->comboTTM->currentIndex() == 1);
    if (!*params.useAutoTMM)
        params.savePath = m_ui->savePath->selectedPath();
    params.category = m_ui->categoryComboBox->currentText();;
    params.addPaused = !m_ui->startTorrentCheckBox->isChecked();
    params.contentLayout = static_cast<BitTorrent::TorrentContentLayout>(m_ui->contentLayoutComboBox->currentIndex());

    return watchedFolderOptions;
}

void WatchedFolderOptionsDialog::loadState()
{
    Utils::Gui::resize(this, m_storeDialogSize);
}

void WatchedFolderOptionsDialog::saveState()
{
    m_storeDialogSize = size();
}

void WatchedFolderOptionsDialog::onCategoryChanged(const int index)
{
    Q_UNUSED(index);

    const QString category = m_ui->categoryComboBox->currentText();
    if (m_ui->comboTTM->currentIndex() == 1)
    {
        const QString savePath = BitTorrent::Session::instance()->categorySavePath(category);
        m_ui->savePath->setSelectedPath(Utils::Fs::toNativePath(savePath));
    }
}

void WatchedFolderOptionsDialog::populateSavePathComboBox()
{
    const QString defSavePath {BitTorrent::Session::instance()->defaultSavePath()};
    m_ui->savePath->setSelectedPath(!m_savePath.isEmpty() ? m_savePath : defSavePath);
}

void WatchedFolderOptionsDialog::onTMMChanged(const int index)
{
    if (index != 1)
    { // 0 is Manual mode and 1 is Automatic mode. Handle all non 1 values as manual mode.
        populateSavePathComboBox();
        m_ui->groupBoxSavePath->setEnabled(true);
        m_ui->savePath->blockSignals(false);
    }
    else
    {
        m_ui->groupBoxSavePath->setEnabled(false);
        m_ui->savePath->blockSignals(true);
        m_savePath = m_ui->savePath->selectedPath();
        const QString savePath = BitTorrent::Session::instance()->categorySavePath(m_ui->categoryComboBox->currentText());
        m_ui->savePath->setSelectedPath(savePath);
    }
}

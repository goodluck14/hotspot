/*
  settingsdialog.cpp

  This file is part of Hotspot, the Qt GUI for performance analysis.

  Copyright (C) 2020 Klarälvdalens Datakonsult AB, a KDAB Group company, info@kdab.com
  Author: Petr Lyapidevskiy <p.lyapidevskiy@nips.ru>

  Licensees holding valid commercial KDAB Hotspot licenses may use this file in
  accordance with Hotspot Commercial License Agreement provided with the Software.

  Contact info@kdab.com if any conditions of this licensing are not clear to you.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 2 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "settingsdialog.h"
#include "ui_settingsdialog.h"

#include <KComboBox>
#include <KUrlRequester>
#include <kconfiggroup.h>
#include <ksharedconfig.h>
#include <QLineEdit>
#include <QListView>

KConfigGroup config()
{
    return KSharedConfig::openConfig()->group("PerfPaths");
}

SettingsDialog::SettingsDialog(QWidget* parent)
    : QDialog(parent)
    , ui(new Ui::SettingsDialog)
{
    ui->setupUi(this);

    connect(ui->buttonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(ui->buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto setupMultiPath = [](KEditListWidget* listWidget, QLabel* buddy, QWidget* previous)
    {
        auto editor = new KUrlRequester(listWidget);
        editor->setPlaceholderText(tr("auto-detect"));
        editor->setMode(KFile::LocalOnly | KFile::Directory | KFile::ExistingOnly);
        buddy->setBuddy(editor);
        listWidget->setCustomEditor(editor->customEditor());
        QWidget::setTabOrder(previous, editor);
        QWidget::setTabOrder(editor, listWidget->listView());
        QWidget::setTabOrder(listWidget->listView(), listWidget->addButton());
        QWidget::setTabOrder(listWidget->addButton(), listWidget->removeButton());
        QWidget::setTabOrder(listWidget->removeButton(), listWidget->upButton());
        QWidget::setTabOrder(listWidget->upButton(), listWidget->downButton());
        return listWidget->downButton();
    };
    auto lastExtraLibsWidget = setupMultiPath(ui->extraLibraryPaths, ui->extraLibraryPathsLabel, ui->lineEditApplicationPath);
    setupMultiPath(ui->debugPaths, ui->debugPathsLabel, lastExtraLibsWidget);

    const auto configGroups = config().groupList();
    auto configfile = config();
    for (const auto& configName : configGroups) {
        if (configfile.hasGroup(configName)) {
            // itemdata is used to save the old name so the old config can be removed
            ui->configComboBox->addItem(configName, configName);
        }
    }

    ui->configComboBox->setDisabled(ui->configComboBox->count() == 0);
    ui->configComboBox->setInsertPolicy(QComboBox::InsertAtCurrent);

    connect(ui->copyConfigButton, &QPushButton::pressed, this, [this] {
        const auto name = QStringLiteral("Config %1").arg(ui->configComboBox->count() + 1);
        ui->configComboBox->addItem(name, name);
        ui->configComboBox->setDisabled(false);
        ui->configComboBox->setCurrentIndex(ui->configComboBox->findText(name));
        saveCurrentConfig();
    });
    connect(ui->removeConfigButton, &QPushButton::pressed, this, &SettingsDialog::removeCurrentConfig);
    connect(ui->configComboBox->lineEdit(), &QLineEdit::editingFinished, this, &SettingsDialog::renameCurrentConfig);
    connect(ui->configComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &SettingsDialog::applyCurrentConfig);
    connect(ui->buttonBox, &QDialogButtonBox::accepted, this, [this] { saveCurrentConfig(); });

    for (auto field : {ui->lineEditSysroot, ui->lineEditApplicationPath, ui->lineEditKallsyms, ui->lineEditObjdump}) {
        connect(field, &KUrlRequester::textEdited, this, &SettingsDialog::saveCurrentConfig);
        connect(field, &KUrlRequester::urlSelected, this, &SettingsDialog::saveCurrentConfig);
    }

    connect(ui->comboBoxArchitecture, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &SettingsDialog::saveCurrentConfig);

    connect(ui->debugPaths, &KEditListWidget::changed, this, &SettingsDialog::saveCurrentConfig);
    connect(ui->extraLibraryPaths, &KEditListWidget::changed, this, &SettingsDialog::saveCurrentConfig);
}

SettingsDialog::~SettingsDialog() = default;

void SettingsDialog::initSettings(const QString& configName)
{
    const int index = ui->configComboBox->findText(configName);
    if (index > -1) {
        ui->configComboBox->setCurrentIndex(index);
        applyCurrentConfig();
    }
}

void SettingsDialog::initSettings(const QString &sysroot, const QString &appPath, const QString &extraLibPaths,
                                  const QString &debugPaths, const QString &kallsyms, const QString &arch,
                                  const QString &objdump)
{
    auto fromPathString = [](KEditListWidget* listWidget, const QString &string)
    {
        listWidget->setItems(string.split(QLatin1Char(':'), Qt::SkipEmptyParts));
    };
    fromPathString(ui->extraLibraryPaths, extraLibPaths);
    fromPathString(ui->debugPaths, debugPaths);

    ui->lineEditSysroot->setText(sysroot);
    ui->lineEditApplicationPath->setText(appPath);
    ui->lineEditKallsyms->setText(kallsyms);
    ui->lineEditObjdump->setText(objdump);

    int itemIndex = 0;
    if (!arch.isEmpty()) {
        itemIndex = ui->comboBoxArchitecture->findText(arch);
        if (itemIndex == -1) {
            itemIndex = ui->comboBoxArchitecture->count();
            ui->comboBoxArchitecture->addItem(arch);
        }
    }
    ui->comboBoxArchitecture->setCurrentIndex(itemIndex);
}

QString SettingsDialog::sysroot() const
{
    return ui->lineEditSysroot->text();
}

QString SettingsDialog::appPath() const
{
    return ui->lineEditApplicationPath->text();
}

QString SettingsDialog::extraLibPaths() const
{
    return ui->extraLibraryPaths->items().join(QLatin1Char(':'));
}

QString SettingsDialog::debugPaths() const
{
    return ui->debugPaths->items().join(QLatin1Char(':'));
}

QString SettingsDialog::kallsyms() const
{
    return ui->lineEditKallsyms->text();
}

QString SettingsDialog::arch() const
{
    QString sArch = ui->comboBoxArchitecture->currentText();
    return (sArch == QLatin1String("auto-detect")) ? QString() : sArch;
}

QString SettingsDialog::objdump() const
{
    return ui->lineEditObjdump->text();
}

void SettingsDialog::saveCurrentConfig()
{
    auto config = ::config();
    KConfigGroup group(&config, ui->configComboBox->currentText());
    group.writeEntry("sysroot", sysroot());
    group.writeEntry("appPath", appPath());
    group.writeEntry("extraLibPaths", extraLibPaths());
    group.writeEntry("debugPaths", debugPaths());
    group.writeEntry("kallsyms", kallsyms());
    group.writeEntry("arch", arch());
    group.writeEntry("objdump", objdump());

    config.sync();
}

void SettingsDialog::renameCurrentConfig()
{
    // itemdata is used to save the old name so the old config can be removed
    const auto oldName = ui->configComboBox->currentData().toString();
    config().deleteGroup(oldName);

    ui->configComboBox->setItemData(ui->configComboBox->currentIndex(), ui->configComboBox->currentText());
    saveCurrentConfig();
    config().sync();
}

void SettingsDialog::removeCurrentConfig()
{
    config().deleteGroup(ui->configComboBox->currentText());
    ui->configComboBox->removeItem(ui->configComboBox->currentIndex());

    if (ui->configComboBox->count() == 0) {
        ui->configComboBox->setDisabled(true);
    }
}

void SettingsDialog::applyCurrentConfig()
{
    auto config = ::config().group(ui->configComboBox->currentText());
    const auto sysroot = config.readEntry("sysroot");
    const auto appPath = config.readEntry("appPath");
    const auto extraLibPaths = config.readEntry("extraLibPaths");
    const auto debugPaths = config.readEntry("debugPaths");
    const auto kallsyms = config.readEntry("kallsyms");
    const auto arch = config.readEntry("arch");
    const auto objdump = config.readEntry("objdump");
    initSettings(sysroot, appPath, extraLibPaths, debugPaths, kallsyms, arch, objdump);
    ::config().writeEntry("lastUsed", ui->configComboBox->currentText());
}

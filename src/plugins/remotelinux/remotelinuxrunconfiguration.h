/****************************************************************************
**
** Copyright (C) 2016 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of Qt Creator.
**
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3 as published by the Free Software
** Foundation with exceptions as appearing in the file LICENSE.GPL3-EXCEPT
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-3.0.html.
**
****************************************************************************/

#pragma once

#include "remotelinux_export.h"

#include <projectexplorer/runconfiguration.h>

#include <QStringList>

namespace RemoteLinux {
class RemoteLinuxRunConfigurationWidget;

namespace Internal { class RemoteLinuxRunConfigurationPrivate; }

class REMOTELINUX_EXPORT RemoteLinuxRunConfiguration : public ProjectExplorer::RunConfiguration
{
    Q_OBJECT
    friend class RemoteLinuxRunConfigurationWidget;

public:
    explicit RemoteLinuxRunConfiguration(ProjectExplorer::Target *target);
    ~RemoteLinuxRunConfiguration() override;

    QWidget *createConfigurationWidget() override;
    Utils::OutputFormatter *createOutputFormatter() const override;

    ProjectExplorer::Runnable runnable() const override;

    QString localExecutableFilePath() const;
    QString defaultRemoteExecutableFilePath() const;
    QString remoteExecutableFilePath() const;
    void setAlternateRemoteExecutable(const QString &exe);
    QString alternateRemoteExecutable() const;
    void setUseAlternateExecutable(bool useAlternate);
    bool useAlternateExecutable() const;

    QVariantMap toMap() const override;

    static const char *IdPrefix;

signals:
    void deploySpecsChanged();
    void targetInformationChanged() const;

protected:
    // FIXME: Used by QNX, remove.
    RemoteLinuxRunConfiguration(ProjectExplorer::Target *target, Core::Id id);

    bool fromMap(const QVariantMap &map) override;

private:
    QString defaultDisplayName() const;
    void handleBuildSystemDataUpdated();

    Internal::RemoteLinuxRunConfigurationPrivate * const d;
};

class RemoteLinuxRunConfigurationFactory : public ProjectExplorer::RunConfigurationFactory
{
public:
    RemoteLinuxRunConfigurationFactory();
};

} // namespace RemoteLinux

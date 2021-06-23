/*
  perfrecord.cpp

  This file is part of Hotspot, the Qt GUI for performance analysis.

  Copyright (C) 2017-2020 Klarälvdalens Datakonsult AB, a KDAB Group company, info@kdab.com
  Author: Nate Rogers <nate.rogers@kdab.com>

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

#include "perfrecord.h"

#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QTimer>

#include <csignal>

#include <KUser>
#include <KWindowSystem>

#include "util.h"

#include <hotspot-config.h>

#include <KAuth>

PerfRecord::PerfRecord(QObject* parent)
    : QObject(parent)
    , m_perfRecordProcess(nullptr)
    , m_outputPath()
    , m_userTerminated(false)
{
}

PerfRecord::~PerfRecord()
{
    stopRecording();
    if (m_perfRecordProcess) {
        m_perfRecordProcess->waitForFinished(100);
        delete m_perfRecordProcess;
    }
}

void PerfRecord::startRecording(bool elevatePrivileges, const QStringList& perfOptions, const QString& outputPath,
                                const QStringList& recordOptions, const QString& workingDirectory)
{
    if (elevatePrivileges) {
        // elevate privileges temporarily as root
        // use kauth to start the elevate_perf_privileges.sh script
        // then parse its output and once we get the "waiting..." line the privileges got elevated
        // in that case, we can continue to start perf and quit the elevate_perf_privileges.sh script
        // once perf has started

        const auto elevateScript = Util::findLibexecBinary(QStringLiteral("elevate_perf_privileges.sh"));
        if (elevateScript.isEmpty()) {
            emit recordingFailed(tr("Failed to find `elevate_perf_privileges.sh` script."));
            return;
        }

        // I/O redirection of client scripts launched by kdesu & friends doesn't work, i.e. no data can be read...
        // so instead we use a temporary file and parse its contents via a polling timer :-/
        auto* outputFile = new QTemporaryFile();
        outputFile->open();

        // poll the file for new input, readyRead isn't being emitted by QFile (cf. docs)
        auto* readTimer = new QTimer(outputFile);
        auto readSlot = [this, outputFile, perfOptions, outputPath, recordOptions, workingDirectory]() {
            const auto data = outputFile->readAll();
            if (data.isEmpty()) {
                return;
            }

            if (data.contains("\nprivileges elevated!\n")) {
                emit recordingOutput(QString::fromUtf8(data));
                emit recordingOutput(QStringLiteral("\n"));
                startRecording(perfOptions, outputPath, recordOptions, workingDirectory);
            } else if (data.contains("Error:")) {
                emit recordingFailed(tr("Failed to elevate privileges: %1").arg(QString::fromUtf8(data)));
            } else {
                emit recordingOutput(QString::fromUtf8(data));
            }
        };
        connect(readTimer, &QTimer::timeout, this, readSlot);

        KAuth::Action action(QLatin1String("com.kdab.hotspot.elevate"));
        action.setHelperId(QLatin1String("com.kdab.hotspot"));
        QVariantMap args;
        args[QLatin1String("script")] = elevateScript;
        args[QLatin1String("output")] = outputFile->fileName();
        action.setArguments(args);

        KAuth::ExecuteJob* job = action.execute();

        connect(job, &KAuth::ExecuteJob::percentChanged, this, [this, readTimer](KJob* job, unsigned long step) {
            Q_UNUSED(job);
            qDebug() << step;
            if (step == 1) {
                emit recordingFailed(tr("Failed to elevate privileges."));
            } else if (step == 2) {
                readTimer->start(250);
            }
        });

        job->start();
    } else {
        startRecording(perfOptions, outputPath, recordOptions, workingDirectory);
    }
}

void PerfRecord::startRecording(const QStringList& perfOptions, const QString& outputPath,
                                const QStringList& recordOptions, const QString& workingDirectory)
{
    // Reset perf record process to avoid getting signals from old processes
    if (m_perfRecordProcess) {
        m_perfRecordProcess->kill();
        m_perfRecordProcess->deleteLater();
    }
    m_perfRecordProcess = new QProcess(this);
    m_perfRecordProcess->setProcessChannelMode(QProcess::MergedChannels);

    QFileInfo outputFileInfo(outputPath);
    QString folderPath = outputFileInfo.dir().path();
    QFileInfo folderInfo(folderPath);
    if (!folderInfo.exists()) {
        emit recordingFailed(tr("Folder '%1' does not exist.").arg(folderPath));
        return;
    }
    if (!folderInfo.isDir()) {
        emit recordingFailed(tr("'%1' is not a folder.").arg(folderPath));
        return;
    }
    if (!folderInfo.isWritable()) {
        emit recordingFailed(tr("Folder '%1' is not writable.").arg(folderPath));
        return;
    }

    connect(m_perfRecordProcess.data(), static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished),
            this, [this](int exitCode, QProcess::ExitStatus exitStatus) {
                Q_UNUSED(exitStatus)

                QFileInfo outputFileInfo(m_outputPath);
                if ((exitCode == EXIT_SUCCESS || (exitCode == SIGTERM && m_userTerminated) || outputFileInfo.size() > 0)
                    && outputFileInfo.exists()) {
                    emit recordingFinished(m_outputPath);
                } else {
                    emit recordingFailed(tr("Failed to record perf data, error code %1.").arg(exitCode));
                }
                m_userTerminated = false;
            });

    connect(m_perfRecordProcess.data(), &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        Q_UNUSED(error)
        if (!m_userTerminated) {
            emit recordingFailed(m_perfRecordProcess->errorString());
        }
    });

    connect(m_perfRecordProcess.data(), &QProcess::readyRead, this, [this]() {
        QString output = QString::fromUtf8(m_perfRecordProcess->readAll());
        emit recordingOutput(output);
    });

    m_outputPath = outputPath;
    auto perfBinary = QStringLiteral("perf");

    if (!workingDirectory.isEmpty()) {
        m_perfRecordProcess->setWorkingDirectory(workingDirectory);
    }

    QStringList perfCommand = {QStringLiteral("record"), QStringLiteral("-o"), m_outputPath};
    perfCommand += perfOptions;
    perfCommand += recordOptions;

    connect(m_perfRecordProcess.data(), &QProcess::started, this,
            [this, perfBinary, perfCommand] { emit recordingStarted(perfBinary, perfCommand); });
    m_perfRecordProcess->start(perfBinary, perfCommand);
}

void PerfRecord::record(const QStringList& perfOptions, const QString& outputPath, bool elevatePrivileges,
                        const QStringList& pids)
{
    if (pids.empty()) {
        emit recordingFailed(tr("Process does not exist."));
        return;
    }

    QStringList options = perfOptions;
    options += {QStringLiteral("--pid"), pids.join(QLatin1Char(','))};
    startRecording(elevatePrivileges, options, outputPath, {});
}

void PerfRecord::record(const QStringList& perfOptions, const QString& outputPath, bool elevatePrivileges,
                        const QString& exePath, const QStringList& exeOptions, const QString& workingDirectory)
{
    QFileInfo exeFileInfo(exePath);

    if (!exeFileInfo.exists()) {
        exeFileInfo.setFile(QStandardPaths::findExecutable(exePath));
    }

    if (!exeFileInfo.exists()) {
        emit recordingFailed(tr("File '%1' does not exist.").arg(exePath));
        return;
    }
    if (!exeFileInfo.isFile()) {
        emit recordingFailed(tr("'%1' is not a file.").arg(exePath));
        return;
    }
    if (!exeFileInfo.isExecutable()) {
        emit recordingFailed(tr("File '%1' is not executable.").arg(exePath));
        return;
    }

    QStringList recordOptions = {exeFileInfo.absoluteFilePath()};
    recordOptions += exeOptions;

    startRecording(elevatePrivileges, perfOptions, outputPath, recordOptions, workingDirectory);
}

void PerfRecord::recordSystem(const QStringList& perfOptions, const QString& outputPath)
{
    auto options = perfOptions;
    options.append(QStringLiteral("--all-cpus"));
    startRecording(true, options, outputPath, {});
}

const QString PerfRecord::perfCommand()
{
    if (m_perfRecordProcess) {
        return QStringLiteral("perf ") + m_perfRecordProcess->arguments().join(QLatin1Char(' '));
    } else {
        return {};
    }
}

void PerfRecord::stopRecording()
{
    m_userTerminated = true;
    if (m_perfRecordProcess) {
        m_perfRecordProcess->terminate();
    }
}

void PerfRecord::sendInput(const QByteArray& input)
{
    Q_ASSERT(m_perfRecordProcess);
    m_perfRecordProcess->write(input);
}

QString PerfRecord::currentUsername()
{
    return KUser().loginName();
}

bool PerfRecord::canTrace(const QString& path)
{
    QFileInfo info(QLatin1String("/sys/kernel/debug/tracing/") + path);
    if (!info.isDir() || !info.isReadable()) {
        return false;
    }
    QFile paranoid(QStringLiteral("/proc/sys/kernel/perf_event_paranoid"));
    return paranoid.open(QIODevice::ReadOnly) && paranoid.readAll().trimmed() == "-1";
}

static QByteArray perfOutput(const QStringList& arguments)
{
    QProcess process;

    auto reportError = [&]() {
        qWarning() << "Failed to run perf" << process.arguments() << process.error() << process.errorString()
                   << process.readAllStandardError();
    };

    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("LANG"), QStringLiteral("C"));
    process.setProcessEnvironment(env);

    QObject::connect(&process, &QProcess::errorOccurred, &process, reportError);
    process.start(QStringLiteral("perf"), arguments);
    if (!process.waitForFinished(1000) || process.exitCode() != 0)
        reportError();
    return process.readAllStandardOutput();
}

static QByteArray perfRecordHelp()
{
    static const QByteArray recordHelp = []() {
        static QByteArray help = perfOutput({QStringLiteral("record"), QStringLiteral("--help")});
        if (help.isEmpty()) {
            // no man page installed, assume the best
            help = "--sample-cpu --switch-events";
        }
        return help;
    }();
    return recordHelp;
}

static QByteArray perfBuildOptions()
{
    static const QByteArray buildOptions = perfOutput({QStringLiteral("version"), QStringLiteral("--build-options")});
    return buildOptions;
}

bool PerfRecord::canProfileOffCpu()
{
    return canTrace(QStringLiteral("events/sched/sched_switch"));
}

QStringList PerfRecord::offCpuProfilingOptions()
{
    return {QStringLiteral("--switch-events"), QStringLiteral("--event"), QStringLiteral("sched:sched_switch")};
}

bool PerfRecord::canSampleCpu()
{
    return perfRecordHelp().contains("--sample-cpu");
}

bool PerfRecord::canSwitchEvents()
{
    return perfRecordHelp().contains("--switch-events");
}

bool PerfRecord::canUseAio()
{
    return perfBuildOptions().contains("aio: [ on  ]");
}

bool PerfRecord::canCompress()
{
    return Zstd_FOUND && perfBuildOptions().contains("zstd: [ on  ]");
}

bool PerfRecord::isPerfInstalled()
{
    return !QStandardPaths::findExecutable(QStringLiteral("perf")).isEmpty();
}

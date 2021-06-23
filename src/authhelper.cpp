#include <QObject>
#include <QProcess>

#include <KAuth>
#include <QDebug>
#include <QEventLoop>
#include <QTimer>

class AuthHelper : public QObject
{
    Q_OBJECT
public:
    AuthHelper() = default;

public slots:
    KAuth::ActionReply elevate(const QVariantMap& args)
    {
        QEventLoop loop;

        auto process = new QProcess();

        connect(process, &QProcess::QProcess::errorOccurred, this, [process] {
            qDebug() << process->errorString();
            KAuth::HelperSupport::progressStep(1);
        });
        connect(process, &QProcess::QProcess::started, this, [] { KAuth::HelperSupport::progressStep(2); });
        connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this, [&loop] { loop.quit(); });

        QTimer::singleShot(1000, process, [process] {
            QObject::disconnect(process, &QProcess::errorOccurred, nullptr, nullptr);
            process->terminate();
        });

        process->start(args[QLatin1String("script")].toString(), {args[QLatin1String("output")].toString()});
        loop.exec();

        return KAuth::ActionReply::SuccessReply();
    }
};

KAUTH_HELPER_MAIN("com.kdab.hotspot", AuthHelper)

#include "authhelper.moc"

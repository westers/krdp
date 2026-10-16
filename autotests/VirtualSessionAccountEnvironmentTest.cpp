// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "VirtualSessionAccountEnvironment.h"
#include <QTest>
using namespace KRdp;
using namespace Qt::StringLiterals;

class VirtualSessionAccountEnvironmentTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void passwdShellIsExported()
    {
        QProcessEnvironment environment;
        VirtualSessionAccountEnvironment::apply(environment, u"westers"_s, u"/home/westers"_s, u"/usr/bin/zsh"_s);
        QCOMPARE(environment.value(u"SHELL"_s), u"/usr/bin/zsh"_s);
        QCOMPARE(environment.value(u"HOME"_s), u"/home/westers"_s);
        QCOMPARE(environment.value(u"USER"_s), u"westers"_s);
        QCOMPARE(environment.value(u"LOGNAME"_s), u"westers"_s);
    }
    void missingOrUnusableShellFallsBackToBash_data()
    {
        QTest::addColumn<QString>("shell");
        QTest::newRow("empty") << QString();
        QTest::newRow("relative") << u"bash"_s;
        QTest::newRow("newline") << u"/bin/bash\nX=1"_s;
        QTest::newRow("control") << u"/bin/ba\tsh"_s;
    }
    void missingOrUnusableShellFallsBackToBash()
    {
        QFETCH(QString, shell);
        QProcessEnvironment environment;
        VirtualSessionAccountEnvironment::apply(environment, u"u"_s, u"/home/u"_s, shell);
        QCOMPARE(environment.value(u"SHELL"_s), u"/bin/bash"_s);
        QVERIFY(!environment.value(u"SHELL"_s).isEmpty());
    }
};
QTEST_APPLESS_MAIN(VirtualSessionAccountEnvironmentTest)
#include "VirtualSessionAccountEnvironmentTest.moc"

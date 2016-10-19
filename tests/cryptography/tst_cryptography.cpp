/****************************************************************************
**
** Copyright (C) 2016 Pelagicore AG
** Contact: https://www.qt.io/licensing/
**
** This file is part of the Pelagicore Application Manager.
**
** $QT_BEGIN_LICENSE:GPL-EXCEPT-QTAS$
** Commercial License Usage
** Licensees holding valid commercial Qt Automotive Suite licenses may use
** this file in accordance with the commercial license agreement provided
** with the Software or, alternatively, in accordance with the terms
** contained in a written agreement between you and The Qt Company.  For
** licensing terms and conditions see https://www.qt.io/terms-conditions.
** For further information use the contact form at https://www.qt.io/contact-us.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3 as published by the Free Software
** Foundation with exceptions as appearing in the file LICENSE.GPL3-EXCEPT
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-3.0.html.
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include <QtCore>
#include <QtTest>

#include "cryptography.h"

QT_USE_NAMESPACE_AM

class tst_Cryptography : public QObject
{
    Q_OBJECT

public:
    tst_Cryptography();

private slots:
    void random();
};

tst_Cryptography::tst_Cryptography()
{ }

void tst_Cryptography::random()
{
    QVERIFY(Cryptography::generateRandomBytes(-1).isEmpty());
    QVERIFY(Cryptography::generateRandomBytes(0).isEmpty());
    QVERIFY(!Cryptography::generateRandomBytes(1).isEmpty());
    QCOMPARE(Cryptography::generateRandomBytes(128).size(), 128);
}

QTEST_APPLESS_MAIN(tst_Cryptography)

#include "tst_cryptography.moc"

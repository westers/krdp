#! /usr/bin/env bash
# SPDX-FileCopyrightText: 2024 Akseli Lahtinen <akselmo@akselmo.dev>
# SPDX-License-Identifier: CC0-1.0

$EXTRACTRC ../../server/krdpserversettings.kcfg >> rc.cpp
$XGETTEXT `find . -name "*.cpp" -o -name "*.qml"` -o $podir/kcm_farside.pot

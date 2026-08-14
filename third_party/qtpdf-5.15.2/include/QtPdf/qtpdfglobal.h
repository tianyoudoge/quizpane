/****************************************************************************
**
** Copyright (C) 2020 The Qt Company Ltd.
** Contact: http://www.qt.io/licensing/
**
** This file is part of the QtPDF module of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:LGPL3$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between The Qt Company. For licensing terms and
** conditions see http://www.qt.io/terms-conditions. For further
** information use the contact form on http://www.qt.io/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 3 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPLv3 included in the
** packaging of this file. Please review the following information to
** ensure that the GNU Lesser General Public License version 3 requirements
** will be met: https://www.gnu.org/licenses/lgpl.html.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU General
** Public License version 2.0 or later as published by the Free Software
** Foundation and appearing in the file LICENSE.GPL included in the
** packaging of this file. Please review the following information to
** ensure that the GNU General Public License version 2.0 requirements
** will be met: https://www.gnu.org/licenses/gpl-2.0.html.
**
** $QT_END_LICENSE$
**
** Vendored unmodified from qtwebengine v5.15.2 src/pdf/api/qtpdfglobal.h
** (https://code.qt.io/qt/qtwebengine.git, tag v5.15.2). The official Qt
** 5.15.2 Windows binary packages ship Qt5Pdf.dll/.lib but no headers or
** CMake configs, so the public headers are mirrored here to link against
** those binaries.
**
****************************************************************************/

#ifndef QTPDFGLOBAL_H
#define QTPDFGLOBAL_H

#include <QtCore/qglobal.h>

QT_BEGIN_NAMESPACE

#ifndef Q_PDF_EXPORT
#  ifndef QT_STATIC
#    if defined(QT_BUILD_PDF_LIB)
#      define Q_PDF_EXPORT Q_DECL_EXPORT
#    else
#      define Q_PDF_EXPORT Q_DECL_IMPORT
#    endif
#  else
#    define Q_PDF_EXPORT
#  endif
#endif

#define Q_PDF_PRIVATE_EXPORT Q_PDF_EXPORT

QT_END_NAMESPACE

#endif // QTPDFGLOBAL_H

// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "project.hpp"
#include <QByteArray>
namespace motion {
QByteArray encodeProject(const Project &);
Project decodeProject(const QByteArray &);
Project loadProject(const QString &);
QString projectDirectory(const QString &);
void saveProject(const Project &, const QString &);
void atomicWrite(const QString &, const QByteArray &);
QString saveRecovery(const Project &, const QString &identity, std::uint64_t revision,
                     const QString &directory);
Project loadRecovery(const QString &);
} // namespace motion

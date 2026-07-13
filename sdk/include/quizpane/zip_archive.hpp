#pragma once

#include <QByteArray>
#include <QList>
#include <QString>

#include <memory>

namespace quizpane {

// ZIP 内的单个目录项。这里只暴露题库安装器真正需要的元数据，隔离底层
// 压缩库，效果类似 Java 项目中在第三方 SDK 外再包一层自己的接口。
struct ZipEntry {
    QString path;
    qint64 size = 0;
    bool isFile = false;
    bool isDirectory = false;
    bool isSymbolicLink = false;
};

struct ZipFile {
    QString path;
    QByteArray data;
};

class ZipArchiveReader {
public:
    explicit ZipArchiveReader(const QString& path);
    ~ZipArchiveReader();

    ZipArchiveReader(const ZipArchiveReader&) = delete;
    ZipArchiveReader& operator=(const ZipArchiveReader&) = delete;

    bool isReadable() const;
    QList<ZipEntry> entries() const;
    QByteArray fileData(const QString& path) const;
    QString errorString() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// 把若干内存文件写成 ZIP。使用 QFile 负责最终落盘，因此 Windows 上包含
// 中文的路径也不会经过第三方库的窄字符文件 API。
bool writeZipArchive(const QString& outputPath, const QList<ZipFile>& files,
                     QString* error = nullptr);

}  // namespace quizpane

#include "quizpane/zip_archive.hpp"

#include <QFile>

#include <cstring>
#include <limits>

#include <miniz.h>

namespace quizpane {

class ZipArchiveReader::Impl {
public:
    explicit Impl(const QString& path) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            error = file.errorString();
            return;
        }
        bytes = file.readAll();
        mz_zip_zero_struct(&archive);
        readable = !bytes.isEmpty() &&
            mz_zip_reader_init_mem(&archive, bytes.constData(),
                                   static_cast<size_t>(bytes.size()), 0);
        if (!readable) error = QString::fromLatin1(
            mz_zip_get_error_string(mz_zip_get_last_error(&archive)));
    }

    ~Impl() {
        if (readable) mz_zip_reader_end(&archive);
    }

    QByteArray bytes;
    mz_zip_archive archive{};
    bool readable = false;
    QString error;
};

ZipArchiveReader::ZipArchiveReader(const QString& path)
    : impl_(std::make_unique<Impl>(path)) {}

ZipArchiveReader::~ZipArchiveReader() = default;

bool ZipArchiveReader::isReadable() const { return impl_->readable; }

QList<ZipEntry> ZipArchiveReader::entries() const {
    QList<ZipEntry> result;
    if (!impl_->readable) return result;
    const mz_uint count = mz_zip_reader_get_num_files(&impl_->archive);
    result.reserve(static_cast<qsizetype>(count));
    for (mz_uint index = 0; index < count; ++index) {
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&impl_->archive, index, &stat)) continue;
        const bool directory = stat.m_is_directory != 0;
        // ZIP 的 Unix 权限存放在 external attributes 高 16 位；0120000 是
        // POSIX 符号链接类型。安装时拒绝符号链接，避免跳出目标目录。
        const bool symlink = ((stat.m_external_attr >> 16U) & 0170000U) == 0120000U;
        result.append(ZipEntry{
            QString::fromUtf8(stat.m_filename),
            stat.m_uncomp_size > static_cast<mz_uint64>((std::numeric_limits<qint64>::max)())
                ? -1 : static_cast<qint64>(stat.m_uncomp_size),
            !directory && !symlink,
            directory,
            symlink
        });
    }
    return result;
}

QByteArray ZipArchiveReader::fileData(const QString& path) const {
    if (!impl_->readable) return {};
    const QByteArray utf8Path = path.toUtf8();
    const int index = mz_zip_reader_locate_file(&impl_->archive,
                                                utf8Path.constData(), nullptr, 0);
    if (index < 0) return {};
    size_t size = 0;
    void* data = mz_zip_reader_extract_to_heap(
        &impl_->archive, static_cast<mz_uint>(index), &size, 0);
    if (!data && size != 0) return {};
    if (size > static_cast<size_t>((std::numeric_limits<qsizetype>::max)())) {
        mz_free(data);
        return {};
    }
    QByteArray result(static_cast<const char*>(data), static_cast<qsizetype>(size));
    mz_free(data);
    return result;
}

QString ZipArchiveReader::errorString() const { return impl_->error; }

bool writeZipArchive(const QString& outputPath, const QList<ZipFile>& files,
                     QString* error) {
    mz_zip_archive archive{};
    mz_zip_zero_struct(&archive);
    if (!mz_zip_writer_init_heap(&archive, 0, 64 * 1024)) {
        if (error) *error = QStringLiteral("无法初始化 ZIP 写入器");
        return false;
    }

    bool success = true;
    for (const ZipFile& file : files) {
        const QByteArray path = file.path.toUtf8();
        if (path.isEmpty() || !mz_zip_writer_add_mem(
                &archive, path.constData(), file.data.constData(),
                static_cast<size_t>(file.data.size()), MZ_DEFAULT_COMPRESSION)) {
            success = false;
            break;
        }
    }

    void* buffer = nullptr;
    size_t size = 0;
    if (success) success = mz_zip_writer_finalize_heap_archive(
        &archive, &buffer, &size) != 0;
    if (!success && error) *error = QString::fromLatin1(
        mz_zip_get_error_string(mz_zip_get_last_error(&archive)));
    mz_zip_writer_end(&archive);

    if (!success) {
        mz_free(buffer);
        return false;
    }
    QFile output(outputPath);
    success = output.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
        output.write(static_cast<const char*>(buffer), static_cast<qint64>(size)) ==
            static_cast<qint64>(size);
    if (!success && error) *error = output.errorString();
    mz_free(buffer);
    return success;
}

}  // namespace quizpane

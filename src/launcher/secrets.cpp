#include "secrets.hpp"

#include <QEventLoop>

#include <qt6keychain/keychain.h>

namespace fastrdp::secrets {

namespace {

const QString kService = QStringLiteral("fastrdp");

void runJob(QKeychain::Job& job) {
    job.setAutoDelete(false);
    QEventLoop loop;
    QObject::connect(&job, &QKeychain::Job::finished, &loop, &QEventLoop::quit);
    job.start();
    loop.exec();
}

} // namespace

bool available() { return QKeychain::isAvailable(); }

bool read(const QString& key, QString& value, QString* error) {
    QKeychain::ReadPasswordJob job(kService);
    job.setKey(key);
    runJob(job);
    if (job.error() == QKeychain::NoError) {
        value = job.textData();
        return true;
    }
    if (error && job.error() != QKeychain::EntryNotFound) *error = job.errorString();
    return false;
}

bool write(const QString& key, const QString& value, QString* error) {
    QKeychain::WritePasswordJob job(kService);
    job.setKey(key);
    job.setTextData(value);
    runJob(job);
    if (job.error() == QKeychain::NoError) return true;
    if (error) *error = job.errorString();
    return false;
}

void remove(const QString& key) {
    QKeychain::DeletePasswordJob job(kService);
    job.setKey(key);
    runJob(job);
}

} // namespace fastrdp::secrets

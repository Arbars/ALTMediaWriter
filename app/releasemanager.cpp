/*
 * Fedora Media Writer
 * Copyright (C) 2016 Martin Bříza <mbriza@redhat.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include "releasemanager.h"
#include "drivemanager.h"

#include "isomd5/libcheckisomd5.h"
#include <yaml-cpp/yaml.h>

#include <fstream>

#include <QtQml>
#include <QApplication>
#include <QAbstractEventDispatcher>

#define GETALT_IMAGES_LOCATION "http://getalt.org/_data/images/"
#define FRONTPAGE_ROW_COUNT 3

QString releaseImagesCacheDir() {
    QString appdataPath = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);

    QDir dir(appdataPath);
    
    // Make path if it doesn't exist
    if (!dir.exists()) {
        dir.mkpath(appdataPath);
    }

    return appdataPath + "/";
}

QString fileToString(const QString &filename) {
    QFile file(filename);
    if (!file.open(QFile::ReadOnly | QFile::Text)) {
        qInfo() << "fileToString(): Failed to open file " << filename;
        return "";
    }
    QTextStream fileStream(&file);
    // NOTE: set codec manually, default codec is no good for cyrillic
    fileStream.setCodec("UTF8");
    QString str = fileStream.readAll();
    file.close();
    return str;
}

QList<QString> getReleaseImagesFiles() {
    const QDir dir(":/images");
    const QList<QString> releaseImagesFiles = dir.entryList();

    return releaseImagesFiles;
}

QString ymlToQString(const YAML::Node &yml_value) {
    const std::string value_std = yml_value.as<std::string>();
    QString out = QString::fromStdString(value_std);

    // Remove HTML character entities that don't render in Qt
    out.replace("&colon;", ":");
    out.replace("&nbsp;", " ");
    // Remove newlines because text will have wordwrap
    out.replace("\n", " ");

    return out;
}

ReleaseManager::ReleaseManager(QObject *parent)
    : QSortFilterProxyModel(parent), m_sourceModel(new ReleaseListModel(this))
{
    mDebug() << this->metaObject()->className() << "construction";
    setSourceModel(m_sourceModel);

    qmlRegisterUncreatableType<Release>("MediaWriter", 1, 0, "Release", "");
    qmlRegisterUncreatableType<ReleaseVersion>("MediaWriter", 1, 0, "Version", "");
    qmlRegisterUncreatableType<ReleaseVariant>("MediaWriter", 1, 0, "Variant", "");
    qmlRegisterUncreatableType<ReleaseArchitecture>("MediaWriter", 1, 0, "Architecture", "");
    qmlRegisterUncreatableType<ReleaseImageType>("MediaWriter", 1, 0, "ImageType", "");
    qmlRegisterUncreatableType<Progress>("MediaWriter", 1, 0, "Progress", "");

    const QList<QString> releaseImagesList = getReleaseImagesFiles();

    // Try to load releases from cache
    bool loadedCachedReleases = true;
    for (auto release : releaseImagesList) {
        QString cachePath = releaseImagesCacheDir() + release;
        QFile cache(cachePath);
        if (!cache.open(QIODevice::ReadOnly)) {
            loadedCachedReleases = false;
            break;
        } else {
            cache.close();
        }
        loadReleaseImages(fileToString(cachePath));
    }

    if (!loadedCachedReleases) {
        // Load built-in release images if failed to load cache
        for (auto release : releaseImagesList) {
            const QString built_in_relese_images_path = ":/images/" + release;
            const QString release_images_string = fileToString(built_in_relese_images_path);
            loadReleaseImages(release_images_string);
        }
    }

    connect(this, SIGNAL(selectedChanged()), this, SLOT(variantChangedFilter()));

    // Download releases from getalt.org
    QTimer::singleShot(0, this, SLOT(fetchReleases()));
}

bool ReleaseManager::filterAcceptsRow(int source_row, const QModelIndex &source_parent) const {
    Q_UNUSED(source_parent)
    if (m_frontPage) {
        const bool on_front_page = (source_row < FRONTPAGE_ROW_COUNT);
        return on_front_page;
    } else {
        auto r = get(source_row);
        bool containsArch = false;
        for (auto version : r->versionList()) {
            for (auto variant : version->variantList()) {
                if (variant->arch()->index() == m_filterArchitecture) {
                    containsArch = true;
                    break;
                }
            }
            if (containsArch)
                break;
        }
        return r->isLocal() || (containsArch && r->displayName().contains(m_filterText, Qt::CaseInsensitive));
    }
}

Release *ReleaseManager::get(int index) const {
    return m_sourceModel->get(index);
}

void ReleaseManager::fetchReleases() {
    m_beingUpdated = true;
    emit beingUpdatedChanged();

    // Start by downloading the first file, the other files will chain download one after another
    currentDownloadingReleaseIndex = 0;

    const QList<QString> releaseImagesList = getReleaseImagesFiles();
    DownloadManager::instance()->fetchPageAsync(this, GETALT_IMAGES_LOCATION + releaseImagesList.first());
}

void ReleaseManager::variantChangedFilter() {
    // TODO here we could add some filters to help signal/slot performance
    // TODO otherwise this can just go away and connections can be directly to the signal
    emit variantChanged();
}

bool ReleaseManager::beingUpdated() const {
    return m_beingUpdated;
}

bool ReleaseManager::frontPage() const {
    return m_frontPage;
}

void ReleaseManager::setFrontPage(bool o) {
    if (m_frontPage != o) {
        m_frontPage = o;
        emit frontPageChanged();
        invalidateFilter();
    }
}

QString ReleaseManager::filterText() const {
    return m_filterText;
}

void ReleaseManager::setFilterText(const QString &o) {
    if (m_filterText != o) {
        m_filterText = o;
        emit filterTextChanged();
        invalidateFilter();
    }
}

bool ReleaseManager::updateUrl(const QString &name, const QString &version, const QString &status, const QDateTime &releaseDate, const QString &architecture, ReleaseImageType *imageType, const QString &board, const QString &url, const QString &sha256, const QString &md5, int64_t size) {
    if (!ReleaseArchitecture::isKnown(architecture)) {
        mWarning() << "Architecture" << architecture << "is not known!";
        return false;
    }
    if (imageType->id() == ReleaseImageType::UNKNOWN) {
        mWarning() << "Image type for " << url << "is not known!";
        return false;
    }
    for (int i = 0; i < m_sourceModel->rowCount(); i++) {
        Release *r = get(i);
        if (r->name().toLower().contains(name))
            return r->updateUrl(version, status, releaseDate, architecture, imageType, board, url, sha256, md5, size);
    }
    return false;
}

int ReleaseManager::filterArchitecture() const {
    return m_filterArchitecture;
}

void ReleaseManager::setFilterArchitecture(int o) {
    if (m_filterArchitecture != o && m_filterArchitecture >= 0 && m_filterArchitecture < ReleaseArchitecture::_ARCHCOUNT) {
        m_filterArchitecture = o;
        emit filterArchitectureChanged();
        for (int i = 0; i < m_sourceModel->rowCount(); i++) {
            Release *r = get(i);
            for (auto v : r->versionList()) {
                int j = 0;
                for (auto variant : v->variantList()) {
                    if (variant->arch()->index() == o) {
                        v->setSelectedVariantIndex(j);
                        break;
                    }
                    j++;
                }
            }
        }
        invalidateFilter();
    }
}

Release *ReleaseManager::selected() const {
    if (m_selectedIndex >= 0 && m_selectedIndex < m_sourceModel->rowCount())
        return m_sourceModel->get(m_selectedIndex);
    return nullptr;
}

int ReleaseManager::selectedIndex() const {
    return m_selectedIndex;
}

void ReleaseManager::setSelectedIndex(int o) {
    if (m_selectedIndex != o) {
        m_selectedIndex = o;
        emit selectedChanged();
    }
}

ReleaseVariant *ReleaseManager::variant() {
    if (selected()) {
        if (selected()->selectedVersion()) {
            if (selected()->selectedVersion()->selectedVariant()) {
                return selected()->selectedVersion()->selectedVariant();
            }
        }
    }
    return nullptr;
}

void ReleaseManager::loadReleaseImages(const QString &fileContents) {
    YAML::Node file = YAML::Load(fileContents.toStdString());

    for (auto e : file["entries"]) {
        QString url = ymlToQString(e["link"]);

        QString name = ymlToQString(e["solution"]);

        QString arch = "unknown";
        if (e["arch"]) {
            arch = ymlToQString(e["arch"]);
        } else {
            // NOTE: yml files missing arch for a couple entries so get it from filename(url)
            ReleaseArchitecture *fileNameArch = ReleaseArchitecture::fromFilename(url);

            if (fileNameArch != nullptr) {
                arch = fileNameArch->abbreviation()[0];
            }
        }

        // NOTE: yml file doesn't define "board" for pc32/pc64
        QString board = "PC";
        if (e["board"]) {
            board = ymlToQString(e["board"]);
        }

        QString md5 = "";
        if (e["md5"]) {
            // NOTE: yml files has md5's for only a few entries
            md5 = ymlToQString(e["md5"]);
        }
        QString sha256 = "";

        // TODO: implement these fields if/when yml files start to include these
        QDateTime releaseDate = QDateTime::fromString("", "yyyy-MM-dd");
        int64_t size = 0;

        // TODO: handle versions if needed
        QString version = "9";
        QString status = "0";

        ReleaseImageType *imageType = ReleaseImageType::fromFilename(url);

        mDebug() << this->metaObject()->className() << "Adding" << name << arch;

        if (!name.isEmpty() && !url.isEmpty() && !arch.isEmpty())
            updateUrl(name, version, status, releaseDate, arch, imageType, board, url, sha256, md5, size);
    }
}

void ReleaseManager::onStringDownloaded(const QString &text) {
    const QList<QString> releaseImagesList = getReleaseImagesFiles();

    mDebug() << this->metaObject()->className() << "Downloaded releases file" << releaseImagesList[currentDownloadingReleaseIndex];

    // Cache downloaded releases file
    QString cachePath = releaseImagesCacheDir() + releaseImagesList[currentDownloadingReleaseIndex];
    std::ofstream cacheFile(cachePath.toStdString());
    cacheFile << text.toStdString();

    loadReleaseImages(text);

    currentDownloadingReleaseIndex++;
    if (currentDownloadingReleaseIndex < releaseImagesList.size()) {
        DownloadManager::instance()->fetchPageAsync(this, GETALT_IMAGES_LOCATION + releaseImagesList[currentDownloadingReleaseIndex]);
    } else if (currentDownloadingReleaseIndex == releaseImagesList.size()) {
        // Downloaded the last releases file
        // Reset index and turn off beingUpdate flag
        currentDownloadingReleaseIndex = 0;
        m_beingUpdated = false;
        emit beingUpdatedChanged();
    }
}

void ReleaseManager::onDownloadError(const QString &message) {
    mWarning() << "Was not able to fetch new releases:" << message << "Retrying in 10 seconds.";

    QTimer::singleShot(10000, this, SLOT(fetchReleases()));
}

QStringList ReleaseManager::architectures() const {
    return ReleaseArchitecture::listAllDescriptions();
}

QStringList ReleaseManager::fileNameFilters() const {
    const QList<ReleaseImageType *> imageTypes = ReleaseImageType::all();

    QStringList filters;
    for (const auto type : imageTypes) {
        if (type->id() == ReleaseImageType::UNKNOWN) {
            continue;
        }

        const QString extensions =
        [type]() {
            QString out;
            out += "(";

            const QStringList abbreviation = type->abbreviation();
            for (const auto e : abbreviation) {
                if (abbreviation.indexOf(e) > 0) {
                    out += " ";
                }

                out += "*." + e;
            }

            out += ")";

            return out;
        }();

        const QString name = type->name();
        
        const QString filter = name + " " + extensions;
        filters.append(filter);
    }

    filters.append(tr("All files") + " (*)");

    return filters;
}

QVariant ReleaseListModel::headerData(int section, Qt::Orientation orientation, int role) const {
    Q_UNUSED(section); Q_UNUSED(orientation);

    if (role == Qt::UserRole + 1)
        return "release";

    return QVariant();
}

QHash<int, QByteArray> ReleaseListModel::roleNames() const {
    QHash<int, QByteArray> ret;
    ret.insert(Qt::UserRole + 1, "release");
    return ret;
}

int ReleaseListModel::rowCount(const QModelIndex &parent) const {
    Q_UNUSED(parent)
    return m_releases.count();
}

QVariant ReleaseListModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid())
        return QVariant();

    if (role == Qt::UserRole + 1)
        return QVariant::fromValue(m_releases[index.row()]);

    return QVariant();
}

ReleaseListModel::ReleaseListModel(ReleaseManager *parent)
    : QAbstractListModel(parent) {
    // Load releases from sections files
    const QDir sections_dir(":/sections");
    const QList<QString> sectionsFiles = sections_dir.entryList();

    for (auto sectionFile : sectionsFiles) {
        const QString sectionFileContents = fileToString(":/sections/" + sectionFile);
        const YAML::Node sectionsFile = YAML::Load(sectionFileContents.toStdString());

        for (unsigned int i = 0; i < sectionsFile["members"].size(); i++) {
            const YAML::Node release_yml = sectionsFile["members"][i];
            const QString name = ymlToQString(release_yml["code"]);

            std::string lang = "_en";
            if (QLocale().language() == QLocale::Russian) {
                lang = "_ru";
            }
            
            const QString display_name = ymlToQString(release_yml["name" + lang]);
            const QString summary = ymlToQString(release_yml["descr" + lang]);

            QString description = ymlToQString(release_yml["descr_full" + lang]);

            // NOTE: currently no screenshots
            const QStringList screenshots;
            
            // Check that icon file exists
            const QString icon_name = ymlToQString(release_yml["img"]);
            const QString icon_path_test = ":/logo/" + icon_name;
            const QFile icon_file(icon_path_test);
            if (!icon_file.exists()) {
                mWarning() << "Failed to find icon file at " << icon_path_test << " needed for release " << name;
            }

            // NOTE: icon_path is consumed by QML, so it needs to begin with "qrc:/" not ":/"
            const QString icon_path = "qrc" + icon_path_test;

            const auto release = new Release(manager(), name, display_name, summary, description, icon_path, screenshots);

            // Reorder releases because default order in
            // sections files is not good. Try to put
            // workstation first and server second, so that they
            // are both on the frontpage.
            // NOTE: this will break if names change in sections files, in that case the order will be the default one
            const int index =
            [this, release]() {
                const QString release_name = release->name();
                const bool is_workstation = (release_name == "alt-workstation");
                const bool is_server = (release_name == "alt-server");

                if (is_workstation) {
                    return 0;
                } else if (is_server) {
                    return 1;
                } else {
                    return m_releases.size();
                }
            }();
            m_releases.insert(index, release);
        }
    }

    // Create custom release, version and variant
    // Insert custom release at the end of the front page
    const auto customRelease = new Release (manager(), "custom", tr("Custom image"), QT_TRANSLATE_NOOP("Release", "Pick a file from your drive(s)"), { QT_TRANSLATE_NOOP("Release", "<p>Here you can choose a OS image from your hard drive to be written to your flash disk</p><p>Currently it is only supported to write raw disk images (.iso or .bin)</p>") }, "qrc:/logo/custom", {});
    m_releases.insert(FRONTPAGE_ROW_COUNT - 1, customRelease);

    const auto customVersion = new ReleaseVersion(customRelease, 0);
    customRelease->addVersion(customVersion);

    const auto customVariant = new ReleaseVariant(customVersion, QString(), QString(), QString(), 0, ReleaseArchitecture::fromId(ReleaseArchitecture::UNKNOWN), ReleaseImageType::all()[ReleaseImageType::ISO], "UNKNOWN BOARD");
    customVersion->addVariant(customVariant);
}

ReleaseManager *ReleaseListModel::manager() {
    return qobject_cast<ReleaseManager*>(parent());
}

Release *ReleaseListModel::get(int index) {
    if (index >= 0 && index < m_releases.count())
        return m_releases[index];
    return nullptr;
}


Release::Release(ReleaseManager *parent, const QString &name, const QString &display_name, const QString &summary, const QString &description, const QString &icon, const QStringList &screenshots)
    : QObject(parent), m_name(name), m_displayName(display_name), m_summary(summary), m_description(description), m_icon(icon), m_screenshots(screenshots)
{
    connect(this, SIGNAL(selectedVersionChanged()), parent, SLOT(variantChangedFilter()));
}

void Release::setLocalFile(const QString &path) {
    QFileInfo info(QUrl(path).toLocalFile());

    if (!info.exists()) {
        mCritical() << path << "doesn't exist";
        return;
    }

    if (m_versions.count() == 1) {
        m_versions.first()->deleteLater();
        m_versions.removeFirst();
    }

    m_versions.append(new ReleaseVersion(this, QUrl(path).toLocalFile(), info.size()));
    emit versionsChanged();
    emit selectedVersionChanged();
}

bool Release::updateUrl(const QString &version, const QString &status, const QDateTime &releaseDate, const QString &architecture, ReleaseImageType *imageType, const QString &board, const QString &url, const QString &sha256, const QString &md5, int64_t size) {
    int finalVersions = 0;
    for (auto i : m_versions) {
        if (i->number() == version)
            return i->updateUrl(status, releaseDate, architecture, imageType, board, url, sha256, md5, size);
        if (i->status() == ReleaseVersion::FINAL)
            finalVersions++;
    }
    ReleaseVersion::Status s = status == "alpha" ? ReleaseVersion::ALPHA : status == "beta" ? ReleaseVersion::BETA : ReleaseVersion::FINAL;
    auto ver = new ReleaseVersion(this, version, s, releaseDate);
    auto variant = new ReleaseVariant(ver, url, sha256, md5, size, ReleaseArchitecture::fromAbbreviation(architecture), imageType, board);
    ver->addVariant(variant);
    addVersion(ver);
    if (ver->status() == ReleaseVersion::FINAL)
        finalVersions++;
    if (finalVersions > 2) {
        QString min = "0";
        ReleaseVersion *oldVer = nullptr;
        for (auto i : m_versions) {
            if (i->number() < min) {
                min = i->number();
                oldVer = i;
            }
        }
        removeVersion(oldVer);
    }
    return true;
}

ReleaseManager *Release::manager() {
    return qobject_cast<ReleaseManager*>(parent());
}

QString Release::name() const {
    return m_name;
}

QString Release::displayName() const {
    return m_displayName;
}

QString Release::summary() const {
    return tr(m_summary.toUtf8());
}

QString Release::description() const {
    return m_description;
}

bool Release::isLocal() const {
    return m_name == "custom";
}

QString Release::icon() const {
    return m_icon;
}

QStringList Release::screenshots() const {
    return m_screenshots;
}

QString Release::prerelease() const {
    if (m_versions.empty() || m_versions.first()->status() == ReleaseVersion::FINAL)
        return "";
    return m_versions.first()->name();
}

QQmlListProperty<ReleaseVersion> Release::versions() {
    return QQmlListProperty<ReleaseVersion>(this, m_versions);
}

QList<ReleaseVersion *> Release::versionList() const {
    return m_versions;
}

QStringList Release::versionNames() const {
    QStringList ret;
    for (auto i : m_versions) {
        ret.append(i->name());
    }
    return ret;
}

void Release::addVersion(ReleaseVersion *version) {
    for (int i = 0; i < m_versions.count(); i++) {
        if (m_versions[i]->number() < version->number()) {
            m_versions.insert(i, version);
            emit versionsChanged();
            if (version->status() != ReleaseVersion::FINAL && m_selectedVersion >= i) {
                m_selectedVersion++;
            }
            emit selectedVersionChanged();
            return;
        }
    }
    m_versions.append(version);
    emit versionsChanged();
    emit selectedVersionChanged();
}

void Release::removeVersion(ReleaseVersion *version) {
    int idx = m_versions.indexOf(version);
    if (!version || idx < 0)
        return;

    if (m_selectedVersion == idx) {
        m_selectedVersion = 0;
        emit selectedVersionChanged();
    }
    m_versions.removeAt(idx);
    version->deleteLater();
    emit versionsChanged();
}

ReleaseVersion *Release::selectedVersion() const {
    if (m_selectedVersion >= 0 && m_selectedVersion < m_versions.count())
        return m_versions[m_selectedVersion];
    return nullptr;
}

int Release::selectedVersionIndex() const {
    return m_selectedVersion;
}

void Release::setSelectedVersionIndex(int o) {
    if (m_selectedVersion != o && m_selectedVersion >= 0 && m_selectedVersion < m_versions.count()) {
        m_selectedVersion = o;
        emit selectedVersionChanged();
    }
}


ReleaseVersion::ReleaseVersion(Release *parent, const QString &number, ReleaseVersion::Status status, QDateTime releaseDate)
    : QObject(parent), m_number(number), m_status(status), m_releaseDate(releaseDate)
{
    if (status != FINAL)
        emit parent->prereleaseChanged();
    connect(this, SIGNAL(selectedVariantChanged()), parent->manager(), SLOT(variantChangedFilter()));
}

ReleaseVersion::ReleaseVersion(Release *parent, const QString &file, int64_t size)
    : QObject(parent), m_variants({ new ReleaseVariant(this, file, size) })
{
    connect(this, SIGNAL(selectedVariantChanged()), parent->manager(), SLOT(variantChangedFilter()));
}

Release *ReleaseVersion::release() {
    return qobject_cast<Release*>(parent());
}

const Release *ReleaseVersion::release() const {
    return qobject_cast<const Release*>(parent());
}

bool ReleaseVersion::updateUrl(const QString &status, const QDateTime &releaseDate, const QString &architecture, ReleaseImageType *imageType, const QString &board, const QString &url, const QString &sha256, const QString &md5, int64_t size) {
    // first determine and eventually update the current alpha/beta/final level of this version
    Status s = status == "alpha" ? ALPHA : status == "beta" ? BETA : FINAL;
    if (s <= m_status) {
        m_status = s;
        emit statusChanged();
        if (s == FINAL)
            emit release()->prereleaseChanged();
    }
    else {
        // return if it got downgraded in the meantime
        return false;
    }
    // update release date
    if (m_releaseDate != releaseDate && releaseDate.isValid()) {
        m_releaseDate = releaseDate;
        emit releaseDateChanged();
    }

    for (auto i : m_variants) {
        if (i->arch() == ReleaseArchitecture::fromAbbreviation(architecture) && i->board() == board)
            return i->updateUrl(url, sha256, size);
    }
    // preserve the order from the ReleaseArchitecture::Id enum (to not have ARM first, etc.)
    // it's actually an array so comparing pointers is fine
    int order = 0;
    for (auto i : m_variants) {
        if (i->arch() > ReleaseArchitecture::fromAbbreviation(architecture))
            break;
        order++;
    }
    m_variants.insert(order, new ReleaseVariant(this, url, sha256, md5, size, ReleaseArchitecture::fromAbbreviation(architecture), imageType, board));
    return true;
}

QString ReleaseVersion::number() const {
    return m_number;
}

QString ReleaseVersion::name() const {
    switch (m_status) {
    case ALPHA:
        return tr("%1 Alpha").arg(m_number);
    case BETA:
        return tr("%1 Beta").arg(m_number);
    case RELEASE_CANDIDATE:
        return tr("%1 Release Candidate").arg(m_number);
    default:
        return QString("%1").arg(m_number);
    }
}

ReleaseVariant *ReleaseVersion::selectedVariant() const {
    if (m_selectedVariant >= 0 && m_selectedVariant < m_variants.count())
        return m_variants[m_selectedVariant];
    return nullptr;
}

int ReleaseVersion::selectedVariantIndex() const {
    return m_selectedVariant;
}

void ReleaseVersion::setSelectedVariantIndex(int o) {
    if (m_selectedVariant != o && m_selectedVariant >= 0 && m_selectedVariant < m_variants.count()) {
        m_selectedVariant = o;
        emit selectedVariantChanged();
    }
}

ReleaseVersion::Status ReleaseVersion::status() const {
    return m_status;
}

QDateTime ReleaseVersion::releaseDate() const {
    return m_releaseDate;
}

void ReleaseVersion::addVariant(ReleaseVariant *v) {
    m_variants.append(v);
    emit variantsChanged();
    if (m_variants.count() == 1)
        emit selectedVariantChanged();
}

QQmlListProperty<ReleaseVariant> ReleaseVersion::variants() {
    return QQmlListProperty<ReleaseVariant>(this, m_variants);
}

QList<ReleaseVariant *> ReleaseVersion::variantList() const {
    return m_variants;
}


ReleaseVariant::ReleaseVariant(ReleaseVersion *parent, QString url, QString shaHash, QString md5, int64_t size, ReleaseArchitecture *arch, ReleaseImageType *imageType, QString board)
    : QObject(parent), m_arch(arch), m_image_type(imageType), m_board(board), m_url(url), m_shaHash(shaHash), m_md5(md5), m_size(size)
{
    connect(this, &ReleaseVariant::sizeChanged, this, &ReleaseVariant::realSizeChanged);
}

ReleaseVariant::ReleaseVariant(ReleaseVersion *parent, const QString &file, int64_t size)
    : QObject(parent), m_image(file), m_arch(ReleaseArchitecture::fromId(ReleaseArchitecture::X86_64)), m_image_type(ReleaseImageType::fromFilename(file)), m_board("UNKNOWN BOARD"), m_shaHash(""), m_md5(""), m_size(size)
{
    connect(this, &ReleaseVariant::sizeChanged, this, &ReleaseVariant::realSizeChanged);
    m_status = READY;
}

bool ReleaseVariant::updateUrl(const QString &url, const QString &sha256, int64_t size) {
    bool changed = false;
    if (!url.isEmpty() && m_url.toUtf8().trimmed() != url.toUtf8().trimmed()) {
        // mWarning() << "Url" << m_url << "changed to" << url;
        m_url = url;
        emit urlChanged();
        changed = true;
    }
    if (!sha256.isEmpty() && m_shaHash.trimmed() != sha256.trimmed()) {
        mWarning() << "SHA256 hash of" << url << "changed from" << m_shaHash << "to" << sha256;
        m_shaHash = sha256;
        emit shaHashChanged();
        changed = true;
    }
    if (size != 0 && m_size != size) {
        m_size = size;
        emit sizeChanged();
        changed = true;
    }
    return changed;
}

ReleaseVersion *ReleaseVariant::releaseVersion() {
    return qobject_cast<ReleaseVersion*>(parent());
}

const ReleaseVersion *ReleaseVariant::releaseVersion() const {
    return qobject_cast<const ReleaseVersion*>(parent());
}

Release *ReleaseVariant::release() {
    return releaseVersion()->release();
}

const Release *ReleaseVariant::release() const {
    return releaseVersion()->release();
}

ReleaseArchitecture *ReleaseVariant::arch() const {
    return m_arch;
}

ReleaseImageType *ReleaseVariant::imageType() const {
    return m_image_type;
}

QString ReleaseVariant::board() const {
    return m_board;
}

QString ReleaseVariant::name() const {
    return m_arch->description() + " | " + m_board;
}

QString ReleaseVariant::fullName() {
    if (release()->isLocal())
        return QFileInfo(image()).fileName();
    else
        return QString("%1 %2 %3").arg(release()->displayName()).arg(releaseVersion()->name()).arg(name());
}

QString ReleaseVariant::url() const {
    return m_url;
}

QString ReleaseVariant::shaHash() const {
    return m_shaHash;
}

QString ReleaseVariant::md5() const {
    return m_md5;
}

QString ReleaseVariant::image() const {
    return m_image;
}

QString ReleaseVariant::temporaryPath() const {
    return m_temporaryImage;
}

qreal ReleaseVariant::size() const {
    return m_size;
}

qreal ReleaseVariant::realSize() const {
    if (m_realSize <= 0)
        return m_size;
    return m_realSize;
}

Progress *ReleaseVariant::progress() {
    if (!m_progress)
        m_progress = new Progress(this, 0.0, size());

    return m_progress;
}

void ReleaseVariant::setRealSize(qint64 o) {
    if (m_realSize != o) {
        m_realSize = o;
        emit realSizeChanged();
    }
}

ReleaseVariant::Status ReleaseVariant::status() const {
    if (m_status == READY && DriveManager::instance()->isBackendBroken())
        return WRITING_NOT_POSSIBLE;
    return m_status;
}

QString ReleaseVariant::statusString() const {
    return m_statusStrings[status()];
}

void ReleaseVariant::onStringDownloaded(const QString &text) {
    mDebug() << this->metaObject()->className() << "Downloaded MD5SUM";

    const QList<QString> releaseImagesList = getReleaseImagesFiles();

    // MD5SUM is of the form "sum image \n sum image \n ..."
    // Search for the sum by finding image matching m_url
    QStringList elements = text.split(QRegExp("\\s+"));
    QString prev = "";
    for (int i = 0; i < elements.size(); ++i) {
        if (elements[i].size() > 0 && m_url.contains(elements[i]) && prev.size() > 0) {
            // Update internal md5
            m_md5 = prev;

            // Update md5 in cached releases file
            // Have to look in all files because don't know which one contains this variant
            for (auto release : releaseImagesList) {
                QString cachePath = releaseImagesCacheDir() + release;

                // Check that file exists
                QFile cache(cachePath);
                if (cache.open(QFile::ReadOnly)) {
                    cache.close();
                } else {
                    continue;
                }

                // Open yaml file and edit it
                YAML::Node file = YAML::Load(fileToString(cachePath).toStdString());
                for (auto e : file["entries"]) {
                    if (e["link"] && ymlToQString(e["link"]) == m_url) {
                        e["md5"] = m_md5.toStdString();
                    }
                }

                // Write yaml file back out to cache
                std::ofstream fout(cachePath.toStdString()); 
                fout << file;
            }

            break;
        }

        prev = elements[i];
    }

}

void ReleaseVariant::onFileDownloaded(const QString &path, const QString &hash) {
    m_temporaryImage = QString();

    if (m_progress)
        m_progress->setValue(size());
    setStatus(DOWNLOAD_VERIFYING);
    m_progress->setValue(0.0/0.0, 1.0);

    if (!shaHash().isEmpty() && shaHash() != hash) {
        mWarning() << "Computed SHA256 hash of" << path << " - " << hash << "does not match expected" << shaHash();
        setErrorString(tr("The downloaded image is corrupted"));
        setStatus(FAILED_DOWNLOAD);
        return;
    }
    mDebug() << this->metaObject()->className() << "SHA256 check passed";

    qApp->eventDispatcher()->processEvents(QEventLoop::AllEvents);

    int checkResult = mediaCheckFile(QDir::toNativeSeparators(path).toLocal8Bit(), md5().toLocal8Bit().data(), &ReleaseVariant::staticOnMediaCheckAdvanced, this);

    if (checkResult == ISOMD5SUM_CHECK_FAILED) {
        mWarning() << "Internal MD5 media check of" << path << "failed with status" << checkResult;
        mWarning() << "sum should be:" << libcheckisomd5_last_mediasum;
        mWarning() << "computed sum:" << libcheckisomd5_last_computedsum;
        QFile::remove(path);
        setErrorString(tr("The downloaded image is corrupted"));
        setStatus(FAILED_DOWNLOAD);
        return;
    }
    else if (checkResult == ISOMD5SUM_FILE_NOT_FOUND) {
        setErrorString(tr("The downloaded file is not readable."));
        setStatus(FAILED_DOWNLOAD);
        return;
    }
    else {
        mDebug() << this->metaObject()->className() << "MD5 check passed";
        QString finalFilename(path);
        finalFilename = finalFilename.replace(QRegExp("[.]part$"), "");

        if (finalFilename != path) {
            mDebug() << this->metaObject()->className() << "Renaming from" << path << "to" << finalFilename;
            if (!QFile::rename(path, finalFilename)) {
                setErrorString(tr("Unable to rename the temporary file."));
                setStatus(FAILED_DOWNLOAD);
                return;
            }
        }

        m_image = finalFilename;
        emit imageChanged();

        mDebug() << this->metaObject()->className() << "Image is ready";
        setStatus(READY);

        if (QFile(m_image).size() != m_size) {
            m_size = QFile(m_image).size();
            emit sizeChanged();
        }
    }
}

void ReleaseVariant::onDownloadError(const QString &message) {
    setErrorString(message);
    setStatus(FAILED_DOWNLOAD);
}

int ReleaseVariant::staticOnMediaCheckAdvanced(void *data, long long offset, long long total) {
    ReleaseVariant *v = static_cast<ReleaseVariant*>(data);
    return v->onMediaCheckAdvanced(offset, total);
}

int ReleaseVariant::onMediaCheckAdvanced(long long offset, long long total) {
    qApp->eventDispatcher()->processEvents(QEventLoop::AllEvents);
    m_progress->setValue(offset, total);
    return 0;
}

void ReleaseVariant::download() {
    if (url().isEmpty() && !image().isEmpty()) {
        setStatus(READY);
    }
    else {
        resetStatus();
        setStatus(DOWNLOADING);
        if (m_size)
            m_progress->setTo(m_size);

        // Download MD5SUM
        int cutoffIndex = m_url.lastIndexOf("/");
        QString md5sumUrl = m_url.left(cutoffIndex) + "/MD5SUM";
        DownloadManager::instance()->fetchPageAsync(this, md5sumUrl);

        QString ret = DownloadManager::instance()->downloadFile(this, url(), DownloadManager::dir(), progress());
        if (!ret.endsWith(".part")) {
            m_temporaryImage = QString();
            m_image = ret;
            emit imageChanged();

            mDebug() << this->metaObject()->className() << m_image << "is already downloaded";
            setStatus(READY);

            if (QFile(m_image).size() != m_size) {
                m_size = QFile(m_image).size();
                emit sizeChanged();
            }
        }
        else {
            m_temporaryImage = ret;
        }
    }
}

void ReleaseVariant::resetStatus() {
    if (!m_image.isEmpty()) {
        setStatus(READY);
    }
    else {
        setStatus(PREPARING);
        if (m_progress)
            m_progress->setValue(0.0);
    }
    setErrorString(QString());
    emit statusChanged();
}

bool ReleaseVariant::erase() {
    if (QFile(m_image).remove()) {
        mDebug() << this->metaObject()->className() << "Deleted" << m_image;
        m_image = QString();
        emit imageChanged();
        return true;
    }
    else {
        mWarning() << this->metaObject()->className() << "An attempt to delete" << m_image << "failed!";
        return false;
    }
}

void ReleaseVariant::setStatus(Status s) {
    if (m_status != s) {
        m_status = s;
        emit statusChanged();
    }
}

QString ReleaseVariant::errorString() const {
    return m_error;
}

void ReleaseVariant::setErrorString(const QString &o) {
    if (m_error != o) {
        m_error = o;
        emit errorStringChanged();
    }
}


ReleaseArchitecture ReleaseArchitecture::m_all[] = {
    {{"x86-64"}, QT_TR_NOOP("AMD 64bit")},
    {{"x86", "i386", "i586", "i686"}, QT_TR_NOOP("Intel 32bit")},
    {{"armv7hl", "armhfp", "armh"}, QT_TR_NOOP("ARM v7")},
    {{"aarch64"}, QT_TR_NOOP("AArch64")},
    {{"mipsel"}, QT_TR_NOOP("MIPS")},
    {{"riscv", "riscv64"}, QT_TR_NOOP("RiscV64")},
    {{"e2k"}, QT_TR_NOOP("Elbrus")},
    {{"ppc64le"}, QT_TR_NOOP("PowerPC")},
    {{"", "unknown"}, QT_TR_NOOP("Unknown")},
};

ReleaseArchitecture::ReleaseArchitecture(const QStringList &abbreviation, const char *description)
    : m_abbreviation(abbreviation), m_description(description)
{

}

ReleaseArchitecture *ReleaseArchitecture::fromId(ReleaseArchitecture::Id id) {
    if (id >= 0 && id < _ARCHCOUNT)
        return &m_all[id];
    return nullptr;
}

ReleaseArchitecture *ReleaseArchitecture::fromAbbreviation(const QString &abbr) {
    for (int i = 0; i < _ARCHCOUNT; i++) {
        if (m_all[i].abbreviation().contains(abbr, Qt::CaseInsensitive))
            return &m_all[i];
    }
    return nullptr;
}

ReleaseArchitecture *ReleaseArchitecture::fromFilename(const QString &filename) {
    for (int i = 0; i < _ARCHCOUNT; i++) {
        ReleaseArchitecture *arch = &m_all[i];
        for (int j = 0; j < arch->m_abbreviation.size(); j++) {
            if (filename.contains(arch->m_abbreviation[j], Qt::CaseInsensitive))
                return &m_all[i];
        }
    }
    return nullptr;
}

bool ReleaseArchitecture::isKnown(const QString &abbr) {
    for (int i = 0; i < _ARCHCOUNT; i++) {
        if (m_all[i].abbreviation().contains(abbr, Qt::CaseInsensitive))
            return true;
    }
    return false;
}

QList<ReleaseArchitecture *> ReleaseArchitecture::listAll() {
    QList<ReleaseArchitecture *> ret;
    for (int i = 0; i < _ARCHCOUNT; i++) {
        ret.append(&m_all[i]);
    }
    return ret;
}

QStringList ReleaseArchitecture::listAllDescriptions() {
    QStringList ret;
    for (int i = 0; i < _ARCHCOUNT; i++) {
        ret.append(m_all[i].description());
    }
    return ret;
}

QStringList ReleaseArchitecture::abbreviation() const {
    return m_abbreviation;
}

QString ReleaseArchitecture::description() const {
    return tr(m_description);
}

int ReleaseArchitecture::index() const {
    return this - m_all;
}

// ReleaseImageType ReleaseImageType::m_all[] = {
//     {{"iso", "dvd"}, QT_TR_NOOP("ISO DVD"), QT_TR_NOOP("ISO format image")},
//     {{"tar"}, QT_TR_NOOP("TAR Archive"), QT_TR_NOOP("tar archive of rootfs")},
//     {{"tgz", "tar.gz"}, QT_TR_NOOP("GZip TAR Archive"), QT_TR_NOOP("GNU Zip compressed tar archive of rootfs")},
//     {{"txz", "tar.xz"}, QT_TR_NOOP("LZMA TAR Archive"), QT_TR_NOOP("LZMA-compressed tar archive of rootfs")},
//     {{"img"}, QT_TR_NOOP("TAR Archive"), QT_TR_NOOP("raw image")},
//     {{"igz", "img.gz"}, QT_TR_NOOP("GZip TAR Archive"), QT_TR_NOOP("GNU Zip compressed raw image")},
//     {{"ixz", "img.xz"}, QT_TR_NOOP("LZMA TAR Archive"), QT_TR_NOOP("LZMA-compressed raw image")},
//     {{"trc", "recovery.tar"}, QT_TR_NOOP("Recovery TAR Archive"), QT_TR_NOOP("Special recovery archive for Tavolga Terminal")},
// };

QList<ReleaseImageType *> ReleaseImageType::all() {
    static const QList<ReleaseImageType *> m_all =
    []() {
        QList<ReleaseImageType *> out;

        for (int i = 0; i < COUNT; i++) {
            const ReleaseImageType::Id id = (ReleaseImageType::Id) i;
            out.append(new ReleaseImageType(id));
        }

        return out;
    }();

    return m_all;
}

ReleaseImageType::ReleaseImageType(const ReleaseImageType::Id id_arg)
: m_id(id_arg) {

}

ReleaseImageType *ReleaseImageType::fromFilename(const QString &filename) {
    for (int i = 0; i < COUNT; i++) {
        ReleaseImageType *type = all()[i];

        const QStringList abbreviations = type->abbreviation();
        for (const QString abbreviation : abbreviations) {
            if (filename.endsWith(abbreviation, Qt::CaseInsensitive)) {
                return type;
            }
        }
    }
    return all()[UNKNOWN];
}

ReleaseImageType::Id ReleaseImageType::id() const {
    return m_id;
}

QStringList ReleaseImageType::abbreviation() const {
    switch (m_id) {
        case ISO: return {"iso", "dvd"};
        case TAR: return {"tar"};
        case TAR_GZ: return {"tgz", "tar.gz"};
        case TAR_XZ: return {"archive", "tar.xz"};
        case IMG: return {"img"};
        case IMG_GZ: return {"igz", "img.gz"};
        case IMG_XZ: return {"ixz", "img.xz"};
        case RECOVERY_TAR: return {"trc", "recovery.tar"};
        case UNKNOWN: return {};
        case COUNT: return {};
    }
    return QStringList();
}

QString ReleaseImageType::name() const {
    switch (m_id) {
        case ISO: return tr("ISO DVD");
        case TAR: return {"TAR Archive"};
        case TAR_GZ: return tr("GZIP TAR Archive");
        case TAR_XZ: return tr("LZMA TAR Archive");
        case IMG: return tr("IMG");
        case IMG_GZ: return tr("GZIP IMG");
        case IMG_XZ: return tr("LZMA IMG");
        case RECOVERY_TAR: return tr("Recovery TAR Archive");
        case UNKNOWN: return tr("Unknown");
        case COUNT: return QString();
    }
    return QString();
}

bool ReleaseImageType::supportedForWriting() const {
    static const QList<ReleaseImageType::Id> unsupported = {
        TAR_GZ, TAR_XZ, IMG_GZ, RECOVERY_TAR, UNKNOWN, COUNT
    };

    return !unsupported.contains(m_id);
}

bool ReleaseImageType::canWriteWithRootfs() const {
#if defined(__APPLE__) || defined(_WIN32)
    return false;
#else
    if (m_id == TAR_XZ) {
        return true;
    } else {
        return false;
    }
#endif
}

bool ReleaseImageType::canMD5checkAfterWrite() const {
    if (m_id == ISO) {
        return true;
    } else {
        return false;
    }
}

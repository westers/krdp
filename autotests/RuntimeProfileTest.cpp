// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
// Fixture JSON uses ASCII literals for readability; production retains Qt's
// explicit-conversion compile checks.
#undef QT_NO_CAST_FROM_ASCII
#undef QT_USE_QSTRINGBUILDER
#include "VirtualSessionRuntimeProfile.h"
// Entirely ordinary-user fixtures; no installed manifest or PAM execution.
#include <QTest>
#include <QTemporaryDir>
#include <QFile>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QCryptographicHash>
#include <QScopeGuard>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/sysmacros.h>
#include <fcntl.h>
#include <functional>
#include <unistd.h>

namespace {
std::function<void()> policyReadHook;
dev_t policyDevice = 0;
ino_t policyInode = 0;
}
extern "C" ssize_t __real_read(int, void *, size_t);
extern "C" ssize_t __wrap_read(int fd, void *buffer, size_t size) {
    const auto result = __real_read(fd, buffer, size);
    struct stat st{};
    if (result > 0 && policyReadHook && !fstat(fd, &st) && st.st_dev == policyDevice && st.st_ino == policyInode) {
        auto hook = std::move(policyReadHook); policyReadHook = {}; hook();
    }
    return result;
}

namespace KRdp {
class RuntimeProfileTest : public QObject {
    Q_OBJECT
    using Profile = VirtualSessionRuntimeProfile;
    const QByteArray elf = QByteArray("\177ELFfixture-never-executed",26);
    static bool put(const QString &path, const QByteArray &bytes, mode_t mode = 0600) {
        QFile f(path); return f.open(QIODevice::WriteOnly|QIODevice::Truncate) && f.write(bytes) == bytes.size()
            && !fchmod(f.handle(),mode);
    }
    static QJsonObject metadata(const QString &path, int mode) {
        return {{"path",path},{"mode",mode},{"uid",int(getuid())},{"gid",int(getuid())}};
    }
    QJsonObject file(const QString &p, const QByteArray &bytes) {
        auto f = metadata(p,0600); f.insert("size",bytes.size()); f.insert("sha256",QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex())); return f;
    }
    QJsonObject manifest(const QString &root) {
        if (!QDir().mkdir(root+"/lib") || !put(root+"/lib/app",elf) || !put(root+"/lib/extra",elf+"extra") || !put(root+"/lib/data","data")) return {};
        if (chmod(QFile::encodeName(root+"/lib").constData(),0700) || !QFile::link(QStringLiteral("lib"),root+"/alias")) return {};
        auto dir = metadata("/lib",0700);
        dir.insert("entries",QJsonArray{QJsonObject{{"name","app"},{"type","file"}},QJsonObject{{"name","data"},{"type","file"}},QJsonObject{{"name","extra"},{"type","file"}}});
        const QJsonObject process{{"name","keeper"},{"executable","/lib/app"},{"required",QJsonArray{"/lib/app"}},
            {"allowed",QJsonArray{"/lib/app"}},{"data",QJsonArray{"/lib/data"}}};
        return {{"v",1},{"files",QJsonArray{file("/lib/app",elf),file("/lib/data","data"),file("/lib/extra",elf+"extra")}},
            {"directories",QJsonArray{dir}},{"absent",QJsonArray{"/lib/preload"}},
            {"symlinks",QJsonArray{QJsonObject{{"path","/alias"},{"target","lib"},{"uid",int(getuid())},{"gid",int(getuid())}}}},
            {"processes",QJsonArray{process}}};
    }
    static QByteArray encode(const QJsonObject &o) { return QJsonDocument(o).toJson(QJsonDocument::Compact)+'\n'; }
    static auto load(const QString &root) { return Profile::loadAt(root,QStringLiteral("/profile.json"),getuid(),nullptr); }
    static QByteArray mapLine(const QString &root, const QString &name, quint64 start = 0x1000, bool executable = true) {
        const auto path = root+name; struct stat st{}; if (stat(QFile::encodeName(path).constData(),&st)) return {};
        return QByteArray::number(start,16)+'-'+QByteArray::number(start+0x1000,16)+(executable ? " r-xp 00000000 " : " r--p 00000000 ")
            + QByteArray::number(major(st.st_dev),16)+':'+QByteArray::number(minor(st.st_dev),16)+' '+QByteArray::number(st.st_ino)+' '+path.toLatin1()+'\n';
    }
private Q_SLOTS:
    void fixedWriterPolicy_data() {
        QTest::addColumn<QString>("kind");
        for (const char *kind : {"valid", "undeclared", "missing", "changed", "mode", "symlink", "hardlink", "empty", "oversize", "manifest-changed", "replace-during-read", "mode-during-read", "manifest-during-read"})
            QTest::newRow(kind) << QString::fromLatin1(kind);
    }
    void fixedWriterPolicy() {
        QFETCH(QString, kind);
        QTemporaryDir dir; auto m = manifest(dir.path());
        const QString logical = QStringLiteral("/etc/krdp/virtual-writer-policy.json");
        QVERIFY(QDir().mkpath(dir.path() + QStringLiteral("/etc/krdp")));
        QVERIFY(!chmod(QFile::encodeName(dir.path() + QStringLiteral("/etc")).constData(), 0700));
        QVERIFY(!chmod(QFile::encodeName(dir.path() + QStringLiteral("/etc/krdp")).constData(), 0700));
        const QString pathname = dir.path() + logical;
        // Opaque bytes here: the policy parser, not this reader, validates JSON
        // schema and policy semantics. A valid read is not admission authority.
        QByteArray data("{\"v\":1}\n");
        if (kind == QStringLiteral("empty")) data.clear();
        if (kind == QStringLiteral("oversize")) data = QByteArray(1024 * 1024 + 1, 'x');
        QVERIFY(put(pathname, data));
        if (kind != QStringLiteral("undeclared")) {
            auto files = m[QStringLiteral("files")].toArray();
            files.prepend(file(logical, data)); m[QStringLiteral("files")] = files;
        }
        QVERIFY(put(dir.filePath(QStringLiteral("profile.json")), encode(m)));
        auto profile = load(dir.path()); QVERIFY(profile);
        if (kind == QStringLiteral("missing")) QVERIFY(QFile::remove(pathname));
        if (kind == QStringLiteral("changed")) QVERIFY(put(pathname, QByteArray(data.size(), '?')));
        if (kind == QStringLiteral("mode")) QVERIFY(!chmod(QFile::encodeName(pathname).constData(), 0640));
        if (kind == QStringLiteral("symlink")) {
            const QString backup = dir.filePath(QStringLiteral("policy-backup"));
            QVERIFY(QFile::rename(pathname, backup)); QVERIFY(QFile::link(backup, pathname));
        }
        if (kind == QStringLiteral("hardlink")) QVERIFY(!link(QFile::encodeName(pathname).constData(), QFile::encodeName(dir.filePath(QStringLiteral("policy-alias"))).constData()));
        if (kind == QStringLiteral("manifest-changed")) QVERIFY(put(dir.filePath(QStringLiteral("profile.json")), encode(m) + ' '));
        bool mutated = false;
        const auto cleanup = qScopeGuard([] { policyReadHook = {}; });
        if (kind.endsWith(QStringLiteral("-during-read"))) {
            struct stat st{}; QVERIFY(!stat(QFile::encodeName(pathname).constData(), &st));
            policyDevice = st.st_dev; policyInode = st.st_ino;
            policyReadHook = [&] {
                if (kind == QStringLiteral("replace-during-read"))
                    mutated = QFile::rename(pathname, dir.filePath(QStringLiteral("old-policy"))) && put(pathname, data);
                else if (kind == QStringLiteral("mode-during-read"))
                    mutated = !chmod(QFile::encodeName(pathname).constData(), 0640);
                else {
                    const QString manifestPath = dir.filePath(QStringLiteral("profile.json"));
                    mutated = QFile::rename(manifestPath, dir.filePath(QStringLiteral("old-profile"))) && put(manifestPath, encode(m));
                }
            };
        }
        QString error = QStringLiteral("old diagnostic");
        const auto result = profile->approvedWriterPolicy(&error);
        if (kind == QStringLiteral("valid")) {
            QVERIFY(result); QCOMPARE(*result, data); QVERIFY(error.isEmpty());
            QFile f(pathname); QVERIFY(f.open(QIODevice::ReadOnly)); QCOMPARE(f.readAll(), data);
        } else {
            QVERIFY(!result); QVERIFY(!error.isEmpty());
        }
        if (kind.endsWith(QStringLiteral("-during-read"))) QVERIFY(mutated);
    }
    void validationDoesNotCreateExecutableAnonymousMappings() {
        const auto anonymousExecutable=[] {
            QFile maps("/proc/self/maps"); QSet<QByteArray> result;
            if (!maps.open(QIODevice::ReadOnly)) return result;
            for (const auto &line : maps.readAll().split('\n')) {
                const auto fields=line.simplified().split(' ');
                if (fields.size()>=5 && fields[1].size()==4 && fields[1][2]=='x' && fields[4]=="0") result.insert(line);
            }
            return result;
        };
        const auto before=anonymousExecutable(); QVERIFY(!before.isEmpty()); // At least the kernel vDSO.
        QTemporaryDir dir; auto m=manifest(dir.path()); QVERIFY(put(dir.filePath("profile.json"),encode(m)));
        auto p=load(dir.path()); QVERIFY(p); QVERIFY(p->validateFilesystem());
        auto maps=mapLine(dir.path(),"/lib/app"); QVERIFY(p->validate("keeper",&maps,"/lib/app",nullptr));
        QCOMPARE(anonymousExecutable(),before);
    }
    void canonicalAndFilesystem() {
        QTemporaryDir dir; const auto m = manifest(dir.path()); QVERIFY(!m.isEmpty()); const auto bytes = encode(m);
        QVERIFY(put(dir.filePath("profile.json"),bytes)); auto p = load(dir.path()); QVERIFY(p);
        QCOMPARE(p->digest(),QString::fromLatin1(QCryptographicHash::hash("KRDP-RUNTIME-PROFILE-V1\n"+bytes,QCryptographicHash::Sha256).toHex()));
        QVERIFY(p->validateFilesystem());
        QCOMPARE(p->approvedExecutable("keeper"),QString("/lib/app"));
        QVERIFY(p->approvedExecutable("unknown").isEmpty());
        auto maps = mapLine(dir.path(),"/lib/app") + mapLine(dir.path(),"/lib/app",0x2000) + mapLine(dir.path(),"/lib/data",0x3000,false);
        QVERIFY(p->validate("keeper",&maps,"/alias/app",nullptr));
        // Kernel maps rows without a pathname can end in padding spaces.
        maps += "4000-5000 rw-p 00000000 00:00 0 \n";
        QVERIFY(p->validate("keeper",&maps,"/lib/app",nullptr));
        QVERIFY(!p->validate("coordinator",&maps,"/lib/app",nullptr));
        QVERIFY(!p->validateCurrentProcess(QString()));
    }
    void schema_data() {
        QTest::addColumn<QString>("kind");
        for (auto kind : {"duplicate-key","unknown","whitespace","order","duplicate-path","cross-kind","type","fraction","oversize","uppercase-hash","escape","relative","required","entry-duplicate","entry-conflict","absent-parent","link-internal-dotdot"})
            QTest::newRow(kind) << QString::fromLatin1(kind);
    }
    void schema() {
        QFETCH(QString,kind); QTemporaryDir dir; auto m = manifest(dir.path()); auto files = m["files"].toArray(); auto f = files[0].toObject();
        if (kind=="unknown") m.insert("extra",0);
        if (kind=="duplicate-path") files.append(files.last());
        if (kind=="cross-kind") m.insert("absent",QJsonArray{"/lib/app"});
        if (kind=="type") f.insert("size",true);
        if (kind=="fraction") f.insert("size",1.5);
        if (kind=="uppercase-hash") f.insert("sha256",QString(64,'A'));
        if (kind=="relative") f.insert("path","lib/app");
        if (kind=="required") { auto a=m["processes"].toArray(); auto r=a[0].toObject(); r.insert("required",QJsonArray{"/lib/extra"}); a[0]=r; m["processes"]=a; }
        if (kind=="entry-duplicate" || kind=="entry-conflict") { auto a=m["directories"].toArray(); auto d=a[0].toObject(); auto e=d["entries"].toArray(); if (kind=="entry-duplicate") e.append(e.last()); else e[0]=QJsonObject{{"name","app"},{"type","directory"}}; d["entries"]=e; a[0]=d; m["directories"]=a; }
        if (kind=="absent-parent") m["absent"]=QJsonArray{"/lib"};
        if (kind=="link-internal-dotdot") { auto a=m["symlinks"].toArray(); auto l=a[0].toObject(); l["target"]="lib/../lib"; a[0]=l; m["symlinks"]=a; }
        files[0]=f; m["files"]=files; auto bytes=encode(m);
        if (kind=="duplicate-key") bytes.replace("\"v\":1","\"v\":1,\"v\":1");
        if (kind=="whitespace") bytes.prepend(' ');
        if (kind=="order") bytes.replace("\"gid\":"+QByteArray::number(getuid())+",\"mode\":384", "\"mode\":384,\"gid\":"+QByteArray::number(getuid()));
        if (kind=="escape") bytes.replace("/lib/app","\\/lib/app");
        if (kind=="oversize") bytes=QByteArray(1024*1024+1,' ');
        QVERIFY(put(dir.filePath("profile.json"),bytes));
        auto p=load(dir.path());
        QVERIFY(!p);
    }
    void mutation_data() {
        QTest::addColumn<QString>("kind");
        for (auto kind : {"hash","size","mode","hardlink","symlink","retarget","loader-entry","absent","manifest","directory-mode","missing-parent","link-loop"}) QTest::newRow(kind)<<QString::fromLatin1(kind);
    }
    void mutation() {
        QFETCH(QString,kind); QTemporaryDir dir; auto m=manifest(dir.path()); QVERIFY(put(dir.filePath("profile.json"),encode(m))); auto p=load(dir.path()); QVERIFY(p); QVERIFY(p->validateFilesystem());
        const auto app=dir.filePath("lib/app");
        if (kind=="hash") QVERIFY(put(app,QByteArray(elf.size(),'x')));
        if (kind=="size") QVERIFY(put(app,elf+'x'));
        if (kind=="mode") QVERIFY(!chmod(QFile::encodeName(app).constData(),0640));
        if (kind=="hardlink") QVERIFY(!link(QFile::encodeName(app).constData(),QFile::encodeName(dir.filePath("second")).constData()));
        if (kind=="symlink") { QVERIFY(QFile::rename(app,dir.filePath("backup"))); QVERIFY(QFile::link(dir.filePath("backup"),app)); }
        if (kind=="retarget" || kind=="link-loop") { QVERIFY(QDir().mkdir(dir.filePath("other"))); QVERIFY(QFile::remove(dir.filePath("alias"))); QVERIFY(QFile::link(kind=="retarget" ? QStringLiteral("other") : QStringLiteral("alias"),dir.filePath("alias"))); }
        if (kind=="loader-entry") QVERIFY(put(dir.filePath("lib/libnew.so"),elf));
        if (kind=="absent") QVERIFY(put(dir.filePath("lib/preload"),{}));
        if (kind=="manifest") QVERIFY(put(dir.filePath("profile.json"),encode(m)+" "));
        if (kind=="directory-mode") QVERIFY(!chmod(QFile::encodeName(dir.filePath("lib")).constData(),0755));
        if (kind=="missing-parent") QVERIFY(QDir().rename(dir.filePath("lib"),dir.filePath("moved")));
        QVERIFY(!p->validateFilesystem());
    }
    void rolesAndMappingNegatives_data() {
        QTest::addColumn<QString>("kind");
        for (auto kind : {"unknown-elf","wrong-exe","missing-required","missing-required-library","deleted","inode","anonymous-exec","nonelf-as-elf","executable-data","syntax","overlap","permissions","overflow","numeric-sign","tab-separator","missing-fields","trailing-pathname-space","nonascii"}) QTest::newRow(kind)<<QString::fromLatin1(kind);
    }
    void rolesAndMappingNegatives() {
        QFETCH(QString,kind); QTemporaryDir dir; auto m=manifest(dir.path());
        if (kind=="nonelf-as-elf") { auto a=m["processes"].toArray(); auto r=a[0].toObject(); r["data"]=QJsonArray{}; r["allowed"]=QJsonArray{"/lib/app","/lib/data"}; a[0]=r; m["processes"]=a; }
        if (kind=="missing-required-library") { auto a=m["processes"].toArray(); auto r=a[0].toObject(); r["required"]=QJsonArray{"/lib/app","/lib/extra"}; r["allowed"]=r["required"]; a[0]=r; m["processes"]=a; }
        QVERIFY(put(dir.filePath("profile.json"),encode(m))); auto p=load(dir.path()); QVERIFY(p);
        auto maps=mapLine(dir.path(),"/lib/app"); QString exe="/lib/app";
        if (kind=="unknown-elf") maps+=mapLine(dir.path(),"/lib/extra",0x2000);
        if (kind=="wrong-exe") exe="/lib/extra";
        if (kind=="missing-required") maps=mapLine(dir.path(),"/lib/data",0x1000,false);
        if (kind=="deleted") maps.replace("/lib/app\n","/lib/app (deleted)\n");
        if (kind=="inode") { const auto fields=maps.split(' '); maps.replace(" "+fields[4]+" "," 1 "); }
        if (kind=="anonymous-exec") maps+="2000-3000 rwxp 00000000 00:00 0\n";
        if (kind=="nonelf-as-elf") maps+=mapLine(dir.path(),"/lib/data",0x2000,false);
        if (kind=="executable-data") maps+=mapLine(dir.path(),"/lib/data",0x2000,true);
        if (kind=="syntax") maps="garbage\n";
        if (kind=="overlap") maps+=mapLine(dir.path(),"/lib/app",0x1000);
        if (kind=="permissions") maps.replace("r-xp","rwxz");
        if (kind=="overflow") maps.replace("1000-2000","1000-fffffffffffffffff");
        if (kind=="numeric-sign") maps.replace("1000-2000","+1000-2000");
        if (kind=="tab-separator") maps.replace(" r-xp ","\tr-xp ");
        if (kind=="missing-fields") maps="1000-2000 r-xp\n";
        if (kind=="trailing-pathname-space") maps.replace("/lib/app\n","/lib/app \n");
        if (kind=="nonascii") maps.replace("/lib/app\n",QByteArray("/lib/app")+char(0xe9)+'\n');
        QVERIFY(!p->validate("keeper",&maps,exe,nullptr));
    }
    void actualMappedObjectReplacedAtSamePath() {
        QTemporaryDir dir; auto m=manifest(dir.path()); QVERIFY(put(dir.filePath("profile.json"),encode(m))); auto p=load(dir.path()); QVERIFY(p);
        QFile file(dir.filePath("lib/app")); QVERIFY(file.open(QIODevice::ReadOnly));
        void *mapping=mmap(nullptr,elf.size(),PROT_READ,MAP_PRIVATE,file.handle(),0); QVERIFY(mapping!=MAP_FAILED);
        const auto cleanup=qScopeGuard([&]{munmap(mapping,elf.size());});
        QFile mapsFile("/proc/self/maps"); QVERIFY(mapsFile.open(QIODevice::ReadOnly)); QByteArray selected;
        for (const auto &line : mapsFile.readAll().split('\n')) if (line.endsWith(QFile::encodeName(file.fileName()))) selected+=line+'\n';
        QVERIFY(!selected.isEmpty()); QVERIFY(p->validate("keeper",&selected,"/lib/app",nullptr));
        QVERIFY(QFile::rename(file.fileName(),dir.filePath("old-app"))); QVERIFY(put(file.fileName(),elf));
        QVERIFY(p->validateFilesystem()); // Identical approved bytes, different inode.
        QVERIFY(!p->validate("keeper",&selected,"/lib/app",nullptr));
    }
    void approvedSymlinkChains() {
        QTemporaryDir dir; auto m=manifest(dir.path());
        QVERIFY(QDir().mkdir(dir.filePath("links"))); QVERIFY(!chmod(QFile::encodeName(dir.filePath("links")).constData(),0700));
        QVERIFY(QFile::link("../alias",dir.filePath("links/lib")));
        // Absolute target is interpreted inside the fixture root by the real
        // resolver; no host path is followed or read.
        QVERIFY(QFile::link("/lib/app",dir.filePath("program")));
        auto links=m["symlinks"].toArray();
        links.append(QJsonObject{{"path","/links/lib"},{"target","../alias"},{"uid",int(getuid())},{"gid",int(getuid())}});
        links.append(QJsonObject{{"path","/program"},{"target","/lib/app"},{"uid",int(getuid())},{"gid",int(getuid())}}); m["symlinks"]=links;
        QVERIFY(put(dir.filePath("profile.json"),encode(m))); auto p=load(dir.path()); QVERIFY(p); QVERIFY(p->validateFilesystem());
        auto maps=mapLine(dir.path(),"/lib/app"); QVERIFY(p->validate("keeper",&maps,"/links/lib/app",nullptr));
        QVERIFY(p->validate("keeper",&maps,"/program",nullptr));
    }
    void manifestUnsafe_data() {
        QTest::addColumn<QString>("kind");
        for (auto kind : {"symlink","hardlink","mode","directory","missing"}) QTest::newRow(kind)<<QString::fromLatin1(kind);
    }
    void manifestUnsafe() {
        QFETCH(QString,kind); QTemporaryDir dir; auto m=manifest(dir.path()); const auto path=dir.filePath("profile.json"); QVERIFY(put(path,encode(m)));
        if (kind=="mode") QVERIFY(!chmod(QFile::encodeName(path).constData(),0660));
        else if (kind=="hardlink") QVERIFY(!link(QFile::encodeName(path).constData(),QFile::encodeName(dir.filePath("copy")).constData()));
        else { QVERIFY(QFile::rename(path,dir.filePath("copy"))); if (kind=="symlink") QVERIFY(QFile::link(dir.filePath("copy"),path)); if (kind=="directory") QVERIFY(QDir().mkdir(path)); }
        QVERIFY(!load(dir.path()));
    }
};
}
QTEST_GUILESS_MAIN(KRdp::RuntimeProfileTest)
#include "RuntimeProfileTest.moc"

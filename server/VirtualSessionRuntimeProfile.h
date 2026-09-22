// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once
#include <QByteArray>
#include <QString>
#include <memory>
#include <optional>

namespace KRdp {
class VirtualSessionMaintenanceWriterPolicyTest;
/** Exact approved runtime inputs, not an approval generator or rearm authority.
 * Caller must retain package+gate exclusion and establish writer quiescence.
 * Neither validation nor a digest proves uninterrupted history or PAM completion.
 * The 10-second validation deadline is cooperative, checked between operations;
 * open/read/hash/filesystem calls can block. It is not a hard wall-clock bound.
 *
 * Canonical JSON V1 only: ASCII strings, no whitespace, lexicographically sorted
 * object keys, decimal integer numbers; all arrays sorted by path (directory
 * entries by name), no duplicates, and exactly one trailing newline.
 * Root keys: absent, directories, files, processes, symlinks, v (integer 1).
 * files: {gid,mode,path,sha256,size,uid}; directories:
 * {entries:[{name,type}],gid,mode,path,uid}; symlinks:{gid,path,target,uid}.
 * absent is an array of absolute paths. Modes are decimal permission/special
 * bits. Entry types are file/directory/symlink. Symlink targets may be relative;
 * every traversed link must be explicitly declared. No implicit realpath trust.
 * Path/name components use only ASCII letters/digits and _+@.,:- (not . or ..).
 * No JSON escapes are accepted in those strings. Relative link targets may
 * start with ../ components, but cannot contain internal . or .. components.
 * processes: [{allowed:[ELF paths],data:[non-ELF paths],executable:path,
 * name:role,required:[ELF paths]}], sorted by name; path lists sorted unique.
 * Required is a subset of allowed and includes executable. Referenced files
 * must be declared. The caller chooses its fixed process role, not client input.
 */
class VirtualSessionRuntimeProfile {
public:
    ~VirtualSessionRuntimeProfile();
    VirtualSessionRuntimeProfile(const VirtualSessionRuntimeProfile &) = delete;
    VirtualSessionRuntimeProfile &operator=(const VirtualSessionRuntimeProfile &) = delete;
    // No configurable production path or public manifest-byte parser.
    static std::unique_ptr<VirtualSessionRuntimeProfile> loadApproved(QString *error = nullptr);
    QString digest() const;
    // Fixed caller-selected role only, never a CLI/client-selected identity.
    // Empty means unknown role; this lookup alone does not validate the file.
    QString approvedExecutable(const QString &fixedRole) const;
    // Fixed writer-policy input only, declared and hashed by this profile.
    // Reads at most 1 MiB under caller-held exclusion. Not policy approval or
    // validation of the rest of the runtime profile; no arbitrary path API.
    std::optional<QByteArray> approvedWriterPolicy(QString *error = nullptr) const;
    bool validateFilesystem(QString *error = nullptr) const;
    // Validates filesystem plus every current file-backed mapping and executable
    // against pinned approved objects. Unknown/deleted mappings refuse. Checks
    // backing-object identity, not private modified memory pages. Call again
    // after PAM loading; coordinator self-check cannot certify a keeper process.
    bool validateCurrentProcess(const QString &role, QString *error = nullptr) const;
private:
    friend class VirtualSessionMaintenanceWriterPolicyTest;
    friend class RuntimeProfileTest;
    struct Data;
    explicit VirtualSessionRuntimeProfile(std::unique_ptr<Data> data);
    static std::unique_ptr<VirtualSessionRuntimeProfile> loadAt(const QString &root, const QString &manifest,
        unsigned owner, QString *error);
    bool validate(const QString &role, const QByteArray *fixtureMaps, const QString &fixtureExe, QString *error) const;
    std::unique_ptr<Data> d;
};
}

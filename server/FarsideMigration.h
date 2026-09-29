#pragma once

namespace FarsideMigration
{
// Safe to call on every start. Never changes or deletes KRDP data.
void copyUserFiles();
void migrateCredentialsAndPermission();
}

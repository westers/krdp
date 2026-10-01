#pragma once

namespace FarsideMigration
{
// Safe to call on every start. Never changes or deletes KRDP data.
void copyUserFiles();
// Metadata only; never accesses a wallet or rewrites existing pending state.
void recordPendingCredentials();
// Portal permission migration is independent of credential storage.
void migratePermission();
}

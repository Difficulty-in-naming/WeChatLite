use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::collections::{BTreeMap, BTreeSet};

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct ClientAccess {
    pub id: String,
    pub name: String,
    pub enabled: bool,
    pub revoked: bool,
    pub groups: BTreeSet<String>,
    pub can_send: bool,
    pub can_download: bool,
    pub created_at: i64,
    pub updated_at: i64,
    pub revision: u64,
}

// Recoverable secrets live separately from the public policy. Never derive Debug or Serialize
// here: only the loopback administrator credential endpoints may return the original token.
#[derive(Clone)]
pub struct ClientRecord {
    pub access: ClientAccess,
    pub token_hash: String,
    pub token: Option<String>,
}
pub type Clients = BTreeMap<String, ClientRecord>;

#[derive(Clone, Debug)]
pub enum Principal {
    Admin,
    Client { id: String, revision: u64 },
}

#[derive(Clone, Copy)]
pub enum Capability {
    Read,
    Send,
    Download,
}

impl Principal {
    pub fn client_id(&self) -> &str {
        match self {
            Self::Admin => "",
            Self::Client { id, .. } => id,
        }
    }

    pub fn valid(&self, clients: &Clients) -> bool {
        match self {
            Self::Admin => true,
            Self::Client { id, revision } => clients.get(id).is_some_and(|record| {
                record.access.enabled
                    && !record.access.revoked
                    && record.access.revision == *revision
            }),
        }
    }

    pub fn allows(&self, clients: &Clients, group: &str, capability: Capability) -> bool {
        if !self.valid(clients) {
            return false;
        }
        match self {
            Self::Admin => true,
            Self::Client { id, .. } => clients.get(id).is_some_and(|record| {
                record.access.groups.contains(group)
                    && match capability {
                        Capability::Read => true,
                        Capability::Send => record.access.can_send,
                        Capability::Download => record.access.can_download,
                    }
            }),
        }
    }
}

pub fn token_hash(token: &str) -> String {
    format!("{:x}", Sha256::digest(token.as_bytes()))
}

// Queue ownership comes from authentication, never the caller-provided display client_id.
// Empty ownership preserves existing local administrator messages after migration.
pub fn can_submit(clients: &Clients, client: &str, group: &str) -> bool {
    client.is_empty()
        || clients.get(client).is_some_and(|record| {
            record.access.enabled
                && !record.access.revoked
                && record.access.can_send
                && record.access.groups.contains(group)
        })
}

pub fn can_read(clients: &Clients, client: &str, group: &str) -> bool {
    client.is_empty()
        || clients.get(client).is_some_and(|record| {
            record.access.enabled && !record.access.revoked && record.access.groups.contains(group)
        })
}

#ifndef TURN_SECRETS_H
#define TURN_SECRETS_H

/*
 * Cloudflare Realtime TURN credentials compiled into nat_traversal_server.
 *
 * Setup:
 *   cp turn_secrets.h.example turn_secrets.h
 *   # edit turn_secrets.h with your TURN key ID and API token
 *   make
 *
 * turn_secrets.h is gitignored. Environment variables still override these
 * values at runtime when set.
 */
#define RMG_TURN_KEY_ID_EMBED "3f5454369315b09ca751c8b5cb90eb8b"
#define RMG_TURN_API_TOKEN_EMBED "5a7185e2e026287e7bd75b2679c6386f1d2be08598d260e910db21168e917dcd"
#define RMG_TURN_CREDENTIAL_TTL_EMBED 86400

#endif /* TURN_SECRETS_H */

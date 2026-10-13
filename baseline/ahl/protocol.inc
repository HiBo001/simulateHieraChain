// AHL uses key-level 2PL admission and locking. It deliberately does not inherit
// Saguaro's participant-domain exclusion or its cross-coordinator abort timer.
// Membership enforces every leaf's sole ancestor is the unique AHL root.
static constexpr bool sagDomainOrdering=false;
#include "../saguaro/protocol.inc"

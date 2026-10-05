// Keep admission, locking, authenticated votes, recovery and PBFT phases shared.
// Membership enforces that every leaf's sole ancestor is the unique AHL root.
#include "../saguaro/protocol.inc"

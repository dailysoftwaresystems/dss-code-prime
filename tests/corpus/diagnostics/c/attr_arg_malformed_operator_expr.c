// [[D-C-SPECIFIER-PREFIX-TOKEN-SCANS-READ-AN-ERROR-NODE-AS-A-TOKEN]]: an
// attribute argument that opens a constant expression and never finishes it.
// The parser recovers, so `analyze()` runs over a tree holding a NodeKind::Error
// node inside a declaration's specifier prefix -- which is exactly the shape
// that used to abort this whole binary. Its VALUE here is not the codes it pins
// but that the run SURVIVES it and every sibling fixture still reports.
long long v __attribute__((__aligned__(2 +)));

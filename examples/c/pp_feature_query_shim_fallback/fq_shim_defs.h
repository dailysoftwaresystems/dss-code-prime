// The header the `__has_include` arm resolves AND then #includes, so the exit
// code depends on the header truly resolving rather than on the probe merely
// folding to 1.
#define FQ_INCLUDE_WEIGHT 12

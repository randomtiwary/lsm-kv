// Ensures the reldb source glob produces a non-empty translation unit until
// real components land in subsequent PRs.
namespace reldb {
namespace {
const int kReldbPlaceholder = 0;
}
const int* ReldbPlaceholder() { return &kReldbPlaceholder; }
}  // namespace reldb

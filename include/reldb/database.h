#pragma once

// Public entry point for the educational relational layer (see docs/RELATIONAL.md).
// PR 12 only declares the namespace; Open / tables / transactions arrive in later PRs.

namespace reldb {

// Placeholder so the include path and library link are exercised by the smoke test.
inline const char* LibraryName() { return "reldb"; }

}  // namespace reldb

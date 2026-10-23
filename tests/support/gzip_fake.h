#pragma once
// For tests that fake the compressor: the production GzipOutcome (extracted from gzip_body.h by the
// runner into gzip_outcome.inc) and the failure budget network_upload.h feeds.
#include <cstdint>
#include "gzip_outcome.inc"
unsigned gzip_noted_failures = 0;
void gzipNoteFailure() { ++gzip_noted_failures; }

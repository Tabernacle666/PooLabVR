#pragma once
// Trace invocations are preprocessor-elided; no runtime journal or buffers.
#define LogEvent(...) ((void)0)

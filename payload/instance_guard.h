#pragma once

/* Returns a descriptor to keep open for the lifetime of the process.
 * Only the kernel-confirmed owner of this dedicated lock may be replaced.
 * Stored PID text is informational and is never used as a signal target. */
int instance_claim(const char *path, int replace);

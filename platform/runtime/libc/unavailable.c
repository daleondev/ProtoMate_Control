#include <errno.h>
#include <stddef.h>

// This firmware profile owns no entropy source. Fail explicitly; Newlib must
// never seed arc4random from deterministic replacement data.
int _getentropy(void* buffer, size_t length)
{
    (void)buffer;
    (void)length;
    errno = ENOSYS;
    return -1;
}

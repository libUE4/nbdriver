#include "nbdriver_abi.h"

_Static_assert(NBDRIVER_ABI_VERSION == 2U, "abi version");
_Static_assert(sizeof(struct nbdriver_abi_info) == 16U, "abi info size");
_Static_assert(_IOC_SIZE(NBDRIVER_IOC_GET_ABI) == 16U, "ioctl ABI size");

int main(void)
{
	struct nbdriver_abi_info info = { 0 };

	return (int)info.size;
}

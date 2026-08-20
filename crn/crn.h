#include <stddef.h>
#include <cstring>

extern "C" {
	unsigned int crn_get_decompressed_size(const void *src, unsigned int src_size, unsigned int level_index);
	void crn_decompress(const void *src, unsigned int src_size, void *dst, unsigned int dst_size, unsigned int level_index);
	// how many mip levels the crn file actually carries (1 = base level only)
	unsigned int crn_get_levels(const void *src, unsigned int src_size);
}
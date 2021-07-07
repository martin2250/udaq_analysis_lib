#include <pybind11/pybind11.h>

// https://en.wikipedia.org/wiki/Fletcher%27s_checksum#Optimizations
uint16_t fletcher16_wikipedia(const uint8_t *data, size_t len) {
	uint32_t c0, c1;

	/*  Found by solving for c1 overflow: */
	/* n > 0 and n * (n+1) / 2 * (2^8-1) < (2^32-1). */
	for (c0 = c1 = 0; len > 0; ) {
		size_t blocklen = len;
		if (blocklen > 5002) {
			blocklen = 5002;
		}
		len -= blocklen;
		do {
			c0 = c0 + *data++;
			c1 = c1 + c0;
		} while (--blocklen);
		c0 = c0 % 255;
		c1 = c1 % 255;
   }
   return (c1 << 8 | c0);
}

uint16_t fletcher_16(pybind11::buffer input) {
    pybind11::buffer_info info = input.request();
    return fletcher16_wikipedia((uint8_t*)info.ptr, (size_t)info.size);
}

PYBIND11_MODULE(fletcher_16, m) {
    m.doc() = "16 bit fletcher checksum";

    m.def("fletcher_16", &fletcher_16, "fletcher_16");
}

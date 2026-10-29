// Single translation unit for the stb single-header libraries.
#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#define STB_IMAGE_IMPLEMENTATION
#define STB_TRUETYPE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image.h"
#include "stb_truetype.h"
#include "stb_image_write.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

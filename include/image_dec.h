#ifndef IMAGE_DEC_H
#define IMAGE_DEC_H

typedef struct {
    int width;
    int height;
    unsigned int* pixels; // 32-bit ARGB (0xAARRGGBB), row-major, top-down
    unsigned int page_count; // Number of contiguous PMM pages
} image_t;

void image_init(image_t* img);
void image_free(image_t* img);

// Returns 0 on success, negative on error.
// If error occurs, err_msg is populated.
int image_load_from_memory(const unsigned char* data, unsigned int size, image_t* out_img, char* err_msg, int err_msg_len);
int image_load_from_file(const char* path, image_t* out_img, char* err_msg, int err_msg_len);

#endif // IMAGE_DEC_H

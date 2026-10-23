#include "image_dec.h"
#include "mm.h"
#include "vfs.h"

// -------------------------------------------------------------
// Utility functions (independent of libc)
// -------------------------------------------------------------
static void* img_memset(void* s, int c, unsigned int n) {
    unsigned char* p = (unsigned char*)s;
    while (n--) *p++ = (unsigned char)c;
    return s;
}

static void* img_memcpy(void* dest, const void* src, unsigned int n) {
    unsigned char* d = (unsigned char*)dest;
    const unsigned char* s = (const unsigned char*)src;
    while (n--) *d++ = *s++;
    return dest;
}

static void my_str_copy(char* dest, const char* src, int max_len) {
    int i = 0;
    while (src[i] && i < max_len - 1) {
        dest[i] = src[i];
        i++;
    }
    dest[i] = '\0';
}

void image_init(image_t* img) {
    if (!img) return;
    img->width = 0;
    img->height = 0;
    img->pixels = 0;
    img->page_count = 0;
}

void image_free(image_t* img) {
    if (!img) return;
    if (img->pixels && img->page_count > 0) {
        pmm_free_pages(img->pixels, img->page_count);
        img->pixels = 0;
        img->page_count = 0;
    }
    img->width = 0;
    img->height = 0;
}

// -------------------------------------------------------------
// Tiny Inflate (tinf) - Embedded lightweight Deflate / zlib decompressor
// -------------------------------------------------------------
typedef struct {
    unsigned short counts[16];
    unsigned short symbols[288];
} tinf_tree;

typedef struct {
    const unsigned char* source;
    unsigned int tag;
    int bitcount;
    tinf_tree ltree; // Literal/length tree
    tinf_tree dtree; // Distance tree
} tinf_data;

// Build Huffman tree from code lengths
static void tinf_build_tree(tinf_tree* t, const unsigned char* lengths, unsigned int num) {
    unsigned short offs[16];
    unsigned int i, sum;

    for (i = 0; i < 16; ++i) t->counts[i] = 0;
    for (i = 0; i < num; ++i) t->counts[lengths[i]]++;
    t->counts[0] = 0;

    for (sum = 0, i = 0; i < 16; ++i) {
        offs[i] = sum;
        sum += t->counts[i];
    }
    for (i = 0; i < num; ++i) {
        if (lengths[i]) {
            t->symbols[offs[lengths[i]]++] = i;
        }
    }
}

// Read bits from input stream
static unsigned int tinf_getbits(tinf_data* d, int num) {
    unsigned int val = d->tag;
    while (d->bitcount < num) {
        d->tag |= ((unsigned int)*d->source++) << d->bitcount;
        d->bitcount += 8;
    }
    d->tag >>= num;
    d->bitcount -= num;
    return val & ((1U << num) - 1);
}

// Decode symbol using Huffman tree
static int tinf_decode_symbol(tinf_data* d, const tinf_tree* t) {
    int base = 0, offs = 0;
    int len;
    for (len = 1; len <= 15; ++len) {
        base = (base << 1) | tinf_getbits(d, 1);
        int count = t->counts[len];
        if (base < count) {
            return t->symbols[offs + base];
        }
        base -= count;
        offs += count;
    }
    return -1;
}

// Given length code, get extra bits and base length
static const unsigned short length_base[30] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258, 0
};
static const unsigned char length_extra[30] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0, 0
};
static const unsigned short dist_base[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
};
static const unsigned char dist_extra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};

static void tinf_build_fixed_trees(tinf_tree* lt, tinf_tree* dt) {
    unsigned char lengths[288];
    unsigned int i;
    for (i = 0; i < 144; ++i) lengths[i] = 8;
    for (; i < 256; ++i) lengths[i] = 9;
    for (; i < 280; ++i) lengths[i] = 7;
    for (; i < 288; ++i) lengths[i] = 8;
    tinf_build_tree(lt, lengths, 288);
    for (i = 0; i < 30; ++i) lengths[i] = 5;
    tinf_build_tree(dt, lengths, 30);
}

static int tinf_inflate_block(tinf_data* d, unsigned char* dest, unsigned int* destLen, unsigned int maxLen) {
    while (1) {
        int sym = tinf_decode_symbol(d, &d->ltree);
        if (sym < 0) return -1;
        if (sym < 256) {
            if (*destLen >= maxLen) return -1;
            dest[(*destLen)++] = (unsigned char)sym;
        } else if (sym == 256) {
            return 0; // End of block
        } else {
            sym -= 257;
            if (sym >= 29) return -1;
            int len = length_base[sym] + tinf_getbits(d, length_extra[sym]);
            int dist_sym = tinf_decode_symbol(d, &d->dtree);
            if (dist_sym < 0 || dist_sym >= 30) return -1;
            int dist = dist_base[dist_sym] + tinf_getbits(d, dist_extra[dist_sym]);
            if ((int)*destLen < dist) return -1;
            if (*destLen + len > maxLen) return -1;
            const unsigned char* src = dest + *destLen - dist;
            while (len--) {
                dest[(*destLen)++] = *src++;
            }
        }
    }
}

static const unsigned char cl_order[19] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

static int tinf_uncompress(unsigned char* dest, unsigned int* destLen, const unsigned char* src, unsigned int maxDestLen) {
    tinf_data d;
    d.source = src;
    d.tag = 0;
    d.bitcount = 0;
    *destLen = 0;

    int bfinal;
    do {
        bfinal = tinf_getbits(&d, 1);
        int btype = tinf_getbits(&d, 2);

        if (btype == 0) { // Uncompressed block
            d.tag >>= (d.bitcount & 7);
            d.bitcount -= (d.bitcount & 7);
            unsigned int len = tinf_getbits(&d, 16);
            unsigned int nlen = tinf_getbits(&d, 16);
            if ((len ^ 0xFFFF) != nlen) return -1;
            if (*destLen + len > maxDestLen) return -1;
            while (len--) {
                dest[(*destLen)++] = (unsigned char)tinf_getbits(&d, 8);
            }
        } else if (btype == 1) { // Fixed Huffman
            tinf_build_fixed_trees(&d.ltree, &d.dtree);
            if (tinf_inflate_block(&d, dest, destLen, maxDestLen) < 0) return -1;
        } else if (btype == 2) { // Dynamic Huffman
            int hlit = tinf_getbits(&d, 5) + 257;
            int hdist = tinf_getbits(&d, 5) + 1;
            int hclen = tinf_getbits(&d, 4) + 4;
            unsigned char cl_lengths[19];
            img_memset(cl_lengths, 0, sizeof(cl_lengths));
            for (int i = 0; i < hclen; ++i) {
                cl_lengths[cl_order[i]] = (unsigned char)tinf_getbits(&d, 3);
            }
            tinf_tree cl_tree;
            tinf_build_tree(&cl_tree, cl_lengths, 19);

            unsigned char lengths[288 + 32];
            int num_codes = hlit + hdist;
            int idx = 0;
            while (idx < num_codes) {
                int sym = tinf_decode_symbol(&d, &cl_tree);
                if (sym < 0) return -1;
                if (sym < 16) {
                    lengths[idx++] = (unsigned char)sym;
                } else if (sym == 16) {
                    if (idx == 0) return -1;
                    int rep = tinf_getbits(&d, 2) + 3;
                    unsigned char prev = lengths[idx - 1];
                    while (rep-- && idx < num_codes) lengths[idx++] = prev;
                } else if (sym == 17) {
                    int rep = tinf_getbits(&d, 3) + 3;
                    while (rep-- && idx < num_codes) lengths[idx++] = 0;
                } else if (sym == 18) {
                    int rep = tinf_getbits(&d, 7) + 11;
                    while (rep-- && idx < num_codes) lengths[idx++] = 0;
                }
            }
            tinf_build_tree(&d.ltree, lengths, hlit);
            tinf_build_tree(&d.dtree, lengths + hlit, hdist);
            if (tinf_inflate_block(&d, dest, destLen, maxDestLen) < 0) return -1;
        } else {
            return -1; // Reserved block type
        }
    } while (!bfinal);

    return 0;
}

// -------------------------------------------------------------
// BMP Decoder Implementation
// -------------------------------------------------------------
static int decode_bmp(const unsigned char* data, unsigned int size, image_t* out_img, char* err_msg, int err_msg_len) {
    if (size < 54) {
        my_str_copy(err_msg, "BMP file too small", err_msg_len);
        return -1;
    }
    if (data[0] != 'B' || data[1] != 'M') {
        my_str_copy(err_msg, "Not a valid BMP image", err_msg_len);
        return -1;
    }

    unsigned int bfOffBits = *(unsigned int*)(data + 10);
    int biWidth = *(int*)(data + 18);
    int biHeight = *(int*)(data + 22);
    unsigned short biBitCount = *(unsigned short*)(data + 28);
    unsigned int biCompression = *(unsigned int*)(data + 30);

    if (biWidth <= 0 || biHeight == 0) {
        my_str_copy(err_msg, "Invalid BMP dimensions", err_msg_len);
        return -1;
    }

    int top_down = 0;
    int height = biHeight;
    if (height < 0) {
        top_down = 1;
        height = -height;
    }
    int width = biWidth;

    if (biCompression != 0 && biCompression != 3) {
        my_str_copy(err_msg, "Unsupported BMP compression", err_msg_len);
        return -1;
    }

    // Check memory requirements
    unsigned int total_pixels = (unsigned int)width * (unsigned int)height;
    unsigned int raw_bytes = total_pixels * 4;
    unsigned int pages_needed = (raw_bytes + 4095) / 4096;
    unsigned int free_pages = pmm_get_free_pages();

    if (pages_needed > free_pages) {
        my_str_copy(err_msg, "Out of Memory: Not enough RAM for image", err_msg_len);
        return -2;
    }

    unsigned int* pix = (unsigned int*)pmm_alloc_pages(pages_needed);
    if (!pix) {
        my_str_copy(err_msg, "Out of Memory: PMM allocation failed", err_msg_len);
        return -2;
    }

    out_img->width = width;
    out_img->height = height;
    out_img->pixels = pix;
    out_img->page_count = pages_needed;

    // Palette handling for 1, 4, 8 bpp
    unsigned int palette[256];
    if (biBitCount <= 8) {
        const unsigned char* pal_ptr = data + 14 + *(unsigned int*)(data + 14);
        int colors = 1 << biBitCount;
        for (int i = 0; i < colors; i++) {
            unsigned char b = pal_ptr[i * 4 + 0];
            unsigned char g = pal_ptr[i * 4 + 1];
            unsigned char r = pal_ptr[i * 4 + 2];
            palette[i] = 0xFF000000 | ((unsigned int)r << 16) | ((unsigned int)g << 8) | (unsigned int)b;
        }
    }

    unsigned int row_bytes = ((width * biBitCount + 31) / 32) * 4;
    const unsigned char* src_bits = data + bfOffBits;

    int has_non_zero_alpha = 0;
    if (biBitCount == 32) {
        for (int y = 0; y < height; y++) {
            const unsigned char* row = src_bits + y * row_bytes;
            for (int x = 0; x < width; x++) {
                if (row[x * 4 + 3] != 0) {
                    has_non_zero_alpha = 1;
                    break;
                }
            }
            if (has_non_zero_alpha) break;
        }
    }

    for (int y = 0; y < height; y++) {
        int src_y = top_down ? y : (height - 1 - y);
        const unsigned char* row = src_bits + src_y * row_bytes;
        unsigned int* dst_row = &pix[y * width];

        if (biBitCount == 32) {
            for (int x = 0; x < width; x++) {
                unsigned char b = row[x * 4 + 0];
                unsigned char g = row[x * 4 + 1];
                unsigned char r = row[x * 4 + 2];
                unsigned char a = has_non_zero_alpha ? row[x * 4 + 3] : 0xFF;
                dst_row[x] = ((unsigned int)a << 24) | ((unsigned int)r << 16) | ((unsigned int)g << 8) | (unsigned int)b;
            }
        } else if (biBitCount == 24) {
            for (int x = 0; x < width; x++) {
                unsigned char b = row[x * 3 + 0];
                unsigned char g = row[x * 3 + 1];
                unsigned char r = row[x * 3 + 2];
                dst_row[x] = 0xFF000000 | ((unsigned int)r << 16) | ((unsigned int)g << 8) | (unsigned int)b;
            }
        } else if (biBitCount == 16) {
            for (int x = 0; x < width; x++) {
                unsigned short c = *(unsigned short*)(row + x * 2);
                unsigned char r = (c >> 10) & 0x1F; r = (r << 3) | (r >> 2);
                unsigned char g = (c >> 5) & 0x1F;  g = (g << 3) | (g >> 2);
                unsigned char b = c & 0x1F;         b = (b << 3) | (b >> 2);
                dst_row[x] = 0xFF000000 | ((unsigned int)r << 16) | ((unsigned int)g << 8) | (unsigned int)b;
            }
        } else if (biBitCount == 8) {
            for (int x = 0; x < width; x++) {
                dst_row[x] = palette[row[x]];
            }
        } else if (biBitCount == 4) {
            for (int x = 0; x < width; x++) {
                unsigned char byte = row[x / 2];
                unsigned char idx = (x & 1) ? (byte & 0x0F) : (byte >> 4);
                dst_row[x] = palette[idx];
            }
        } else if (biBitCount == 1) {
            for (int x = 0; x < width; x++) {
                unsigned char byte = row[x / 8];
                unsigned char idx = (byte >> (7 - (x & 7))) & 1;
                dst_row[x] = palette[idx];
            }
        }
    }

    return 0;
}

// -------------------------------------------------------------
// PNG Decoder Implementation
// -------------------------------------------------------------
static inline int paeth_predict(int a, int b, int c) {
    int p = a + b - c;
    int pa = p > a ? p - a : a - p;
    int pb = p > b ? p - b : b - p;
    int pc = p > c ? p - c : c - p;
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

static unsigned int read_be32(const unsigned char* p) {
    return ((unsigned int)p[0] << 24) | ((unsigned int)p[1] << 16) | ((unsigned int)p[2] << 8) | (unsigned int)p[3];
}

static int decode_png(const unsigned char* data, unsigned int size, image_t* out_img, char* err_msg, int err_msg_len) {
    if (size < 29) {
        my_str_copy(err_msg, "PNG file too small", err_msg_len);
        return -1;
    }
    // Verify 8-byte PNG signature
    if (data[0] != 0x89 || data[1] != 'P' || data[2] != 'N' || data[3] != 'G' ||
        data[4] != 0x0D || data[5] != 0x0A || data[6] != 0x1A || data[7] != 0x0A) {
        my_str_copy(err_msg, "Not a valid PNG file", err_msg_len);
        return -1;
    }

    int width = 0, height = 0;
    int bit_depth = 0, color_type = 0;

    unsigned char palette[256 * 4];
    img_memset(palette, 0xFF, sizeof(palette));
    int has_palette = 0;

    // Scan chunks
    unsigned int pos = 8;
    unsigned int idat_size = 0;
    const unsigned char* first_idat = 0;

    while (pos + 8 <= size) {
        unsigned int chunk_len = read_be32(data + pos);
        unsigned int chunk_type = read_be32(data + pos + 4);
        const unsigned char* chunk_data = data + pos + 8;

        if (pos + 12 + chunk_len > size) break;

        if (chunk_type == 0x49484452) { // 'IHDR'
            width = (int)read_be32(chunk_data);
            height = (int)read_be32(chunk_data + 4);
            bit_depth = chunk_data[8];
            color_type = chunk_data[9];
        } else if (chunk_type == 0x504C5445) { // 'PLTE'
            int entries = chunk_len / 3;
            if (entries > 256) entries = 256;
            for (int i = 0; i < entries; i++) {
                palette[i * 4 + 0] = chunk_data[i * 3 + 0]; // R
                palette[i * 4 + 1] = chunk_data[i * 3 + 1]; // G
                palette[i * 4 + 2] = chunk_data[i * 3 + 2]; // B
                palette[i * 4 + 3] = 0xFF;                  // A
            }
            has_palette = 1;
        } else if (chunk_type == 0x74524E53) { // 'tRNS' (Transparency)
            if (color_type == 3 && has_palette) {
                int entries = chunk_len < 256 ? chunk_len : 256;
                for (int i = 0; i < entries; i++) {
                    palette[i * 4 + 3] = chunk_data[i];
                }
            }
        } else if (chunk_type == 0x49444154) { // 'IDAT'
            if (!first_idat) first_idat = chunk_data;
            idat_size += chunk_len;
        } else if (chunk_type == 0x49454E44) { // 'IEND'
            break;
        }

        pos += 12 + chunk_len;
    }

    if (width <= 0 || height <= 0 || !first_idat) {
        my_str_copy(err_msg, "Corrupt PNG metadata or no IDAT", err_msg_len);
        return -1;
    }

    int bytes_per_pixel = 4;
    if (color_type == 0) bytes_per_pixel = 1; // Grayscale
    else if (color_type == 2) bytes_per_pixel = 3; // RGB
    else if (color_type == 3) bytes_per_pixel = 1; // Palette
    else if (color_type == 4) bytes_per_pixel = 2; // Gray + Alpha
    else if (color_type == 6) bytes_per_pixel = 4; // RGBA

    unsigned int stride = 1 + (unsigned int)width * bytes_per_pixel;
    unsigned int raw_decompressed_size = stride * (unsigned int)height;

    // Check available physical memory
    unsigned int out_pages = ((unsigned int)width * (unsigned int)height * 4 + 4095) / 4096;
    unsigned int raw_pages = (raw_decompressed_size + 4095) / 4096;
    unsigned int total_pages_needed = out_pages + raw_pages;

    if (total_pages_needed > pmm_get_free_pages()) {
        my_str_copy(err_msg, "Out of Memory: 4K PNG requires more RAM", err_msg_len);
        return -2;
    }

    // Allocate decompressed scanlines buffer
    unsigned char* raw_buf = (unsigned char*)pmm_alloc_pages(raw_pages);
    if (!raw_buf) {
        my_str_copy(err_msg, "Out of Memory: Raw buffer alloc failed", err_msg_len);
        return -2;
    }

    // Combine multiple IDAT chunks if necessary
    const unsigned char* zlib_data = first_idat;
    unsigned char* idat_combined = 0;
    unsigned int idat_pages = 0;

    // Fast check if single IDAT chunk
    pos = 8;
    int idat_count = 0;
    while (pos + 8 <= size) {
        unsigned int chunk_len = read_be32(data + pos);
        unsigned int chunk_type = read_be32(data + pos + 4);
        if (chunk_type == 0x49444154) idat_count++;
        pos += 12 + chunk_len;
    }

    if (idat_count > 1) {
        idat_pages = (idat_size + 4095) / 4096;
        idat_combined = (unsigned char*)pmm_alloc_pages(idat_pages);
        if (idat_combined) {
            unsigned int cpos = 0;
            pos = 8;
            while (pos + 8 <= size) {
                unsigned int chunk_len = read_be32(data + pos);
                unsigned int chunk_type = read_be32(data + pos + 4);
                if (chunk_type == 0x49444154) {
                    img_memcpy(idat_combined + cpos, data + pos + 8, chunk_len);
                    cpos += chunk_len;
                }
                pos += 12 + chunk_len;
            }
            zlib_data = idat_combined;
        }
    }

    // Decompress zlib stream (skip 2-byte header)
    unsigned int decomp_len = 0;
    int ret = tinf_uncompress(raw_buf, &decomp_len, zlib_data + 2, raw_decompressed_size);

    if (idat_combined) {
        pmm_free_pages(idat_combined, idat_pages);
    }

    if (ret != 0 || decomp_len < raw_decompressed_size) {
        pmm_free_pages(raw_buf, raw_pages);
        my_str_copy(err_msg, "PNG decompression failed", err_msg_len);
        return -1;
    }

    // Allocate final 32-bit ARGB image buffer
    unsigned int* pix = (unsigned int*)pmm_alloc_pages(out_pages);
    if (!pix) {
        pmm_free_pages(raw_buf, raw_pages);
        my_str_copy(err_msg, "Out of Memory: Output image buffer failed", err_msg_len);
        return -2;
    }

    out_img->width = width;
    out_img->height = height;
    out_img->pixels = pix;
    out_img->page_count = out_pages;

    // Unfilter scanlines and convert to 32-bit ARGB
    const unsigned char* prev_row = 0;

    for (int y = 0; y < height; y++) {
        unsigned char* cur_row = raw_buf + (unsigned int)y * stride;
        int filter_type = cur_row[0];
        unsigned char* cur_pixels = cur_row + 1;

        if (filter_type == 1) { // Sub
            for (int i = bytes_per_pixel; i < width * bytes_per_pixel; i++) {
                cur_pixels[i] += cur_pixels[i - bytes_per_pixel];
            }
        } else if (filter_type == 2) { // Up
            if (prev_row) {
                for (int i = 0; i < width * bytes_per_pixel; i++) {
                    cur_pixels[i] += prev_row[i];
                }
            }
        } else if (filter_type == 3) { // Average
            for (int i = 0; i < width * bytes_per_pixel; i++) {
                int a = (i >= bytes_per_pixel) ? cur_pixels[i - bytes_per_pixel] : 0;
                int b = prev_row ? prev_row[i] : 0;
                cur_pixels[i] += (unsigned char)((a + b) >> 1);
            }
        } else if (filter_type == 4) { // Paeth
            for (int i = 0; i < width * bytes_per_pixel; i++) {
                int a = (i >= bytes_per_pixel) ? cur_pixels[i - bytes_per_pixel] : 0;
                int b = prev_row ? prev_row[i] : 0;
                int c = (prev_row && i >= bytes_per_pixel) ? prev_row[i - bytes_per_pixel] : 0;
                cur_pixels[i] += (unsigned char)paeth_predict(a, b, c);
            }
        }

        prev_row = cur_pixels;

        // Convert scanline to 32-bit ARGB
        unsigned int* dst_row = &pix[y * width];
        if (color_type == 6) { // RGBA
            for (int x = 0; x < width; x++) {
                unsigned char r = cur_pixels[x * 4 + 0];
                unsigned char g = cur_pixels[x * 4 + 1];
                unsigned char b = cur_pixels[x * 4 + 2];
                unsigned char a = cur_pixels[x * 4 + 3];
                dst_row[x] = ((unsigned int)a << 24) | ((unsigned int)r << 16) | ((unsigned int)g << 8) | (unsigned int)b;
            }
        } else if (color_type == 2) { // RGB
            for (int x = 0; x < width; x++) {
                unsigned char r = cur_pixels[x * 3 + 0];
                unsigned char g = cur_pixels[x * 3 + 1];
                unsigned char b = cur_pixels[x * 3 + 2];
                dst_row[x] = 0xFF000000 | ((unsigned int)r << 16) | ((unsigned int)g << 8) | (unsigned int)b;
            }
        } else if (color_type == 3) { // Palette
            for (int x = 0; x < width; x++) {
                unsigned char idx = cur_pixels[x];
                unsigned char r = palette[idx * 4 + 0];
                unsigned char g = palette[idx * 4 + 1];
                unsigned char b = palette[idx * 4 + 2];
                unsigned char a = palette[idx * 4 + 3];
                dst_row[x] = ((unsigned int)a << 24) | ((unsigned int)r << 16) | ((unsigned int)g << 8) | (unsigned int)b;
            }
        } else if (color_type == 0) { // Grayscale
            for (int x = 0; x < width; x++) {
                unsigned char v = cur_pixels[x];
                dst_row[x] = 0xFF000000 | ((unsigned int)v << 16) | ((unsigned int)v << 8) | (unsigned int)v;
            }
        } else if (color_type == 4) { // Gray + Alpha
            for (int x = 0; x < width; x++) {
                unsigned char v = cur_pixels[x * 2 + 0];
                unsigned char a = cur_pixels[x * 2 + 1];
                dst_row[x] = ((unsigned int)a << 24) | ((unsigned int)v << 16) | ((unsigned int)v << 8) | (unsigned int)v;
            }
        }
    }

    pmm_free_pages(raw_buf, raw_pages);
    return 0;
}

// -------------------------------------------------------------
// Universal Image Loader API
// -------------------------------------------------------------
int image_load_from_memory(const unsigned char* data, unsigned int size, image_t* out_img, char* err_msg, int err_msg_len) {
    if (!data || size < 8 || !out_img) {
        my_str_copy(err_msg, "Invalid buffer", err_msg_len);
        return -1;
    }

    image_init(out_img);

    if (data[0] == 'B' && data[1] == 'M') {
        return decode_bmp(data, size, out_img, err_msg, err_msg_len);
    } else if (data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G') {
        return decode_png(data, size, out_img, err_msg, err_msg_len);
    }

    my_str_copy(err_msg, "Unsupported format (Only BMP and PNG supported)", err_msg_len);
    return -1;
}

int image_load_from_file(const char* path, image_t* out_img, char* err_msg, int err_msg_len) {
    if (!path || !path[0] || !out_img) {
        my_str_copy(err_msg, "Invalid file path", err_msg_len);
        return -1;
    }

    vfs_dirent_t dirent;
    if (vfs_find_file(path, &dirent) != VFS_OK) {
        my_str_copy(err_msg, "File not found", err_msg_len);
        return -1;
    }

    unsigned int file_size = dirent.size;
    if (file_size == 0) {
        my_str_copy(err_msg, "File is empty", err_msg_len);
        return -1;
    }

    unsigned int pages = (file_size + 4095) / 4096;
    if (pages > pmm_get_free_pages()) {
        my_str_copy(err_msg, "Out of Memory: File exceeds available RAM", err_msg_len);
        return -2;
    }

    unsigned char* file_buf = (unsigned char*)pmm_alloc_pages(pages);
    if (!file_buf) {
        my_str_copy(err_msg, "Out of Memory: Cannot allocate file buffer", err_msg_len);
        return -2;
    }

    int bytes_read = vfs_read_file(path, file_buf, file_size);
    if (bytes_read < (int)file_size) {
        pmm_free_pages(file_buf, pages);
        my_str_copy(err_msg, "Read error from VFS", err_msg_len);
        return -1;
    }

    int ret = image_load_from_memory(file_buf, file_size, out_img, err_msg, err_msg_len);
    pmm_free_pages(file_buf, pages);

    return ret;
}

#include "vmware_svga.h"
#include "pci.h"
#include "port.h"
#include "mmio.h"
#include "log.h"

#ifndef NULL
#define NULL ((void*)0)
#endif

#define SVGA_ID_0 0x90000000U
#define SVGA_ID_1 0x90000001U
#define SVGA_ID_2 0x90000002U

#define SVGA_REG_ID             0
#define SVGA_REG_ENABLE         1
#define SVGA_REG_WIDTH          2
#define SVGA_REG_HEIGHT         3
#define SVGA_REG_MAX_WIDTH      4
#define SVGA_REG_MAX_HEIGHT     5
#define SVGA_REG_DEPTH          6
#define SVGA_REG_BPP            7
#define SVGA_REG_BYTES_PER_LINE 12
#define SVGA_REG_FB_START       13
#define SVGA_REG_FB_OFFSET      14
#define SVGA_REG_VRAM_SIZE      15
#define SVGA_REG_CAPABILITIES   17
#define SVGA_REG_MEM_SIZE       19
#define SVGA_REG_CONFIG_DONE    20
#define SVGA_REG_SYNC           21
#define SVGA_REG_BUSY           22
#define SVGA_REG_CURSOR_ID      24
#define SVGA_REG_CURSOR_X       25
#define SVGA_REG_CURSOR_Y       26
#define SVGA_REG_CURSOR_ON      27

#define SVGA_FIFO_MIN       0
#define SVGA_FIFO_MAX       1
#define SVGA_FIFO_NEXT_CMD  2
#define SVGA_FIFO_STOP      3
#define SVGA_FIFO_COMMIT    4

#define SVGA_CMD_UPDATE      1
#define SVGA_CMD_RECT_FILL   2
#define SVGA_CMD_RECT_COPY   3
#define SVGA_CMD_DEFINE_CURSOR 19
#define SVGA_CMD_MOVE_CURSOR   20

#define SVGA_CURSOR_HIDE 0
#define SVGA_CURSOR_SHOW 1

#define SVGA_FIFO_MIN_BYTES 4096U
#define SVGA_FIFO_MIN_SIZE  16384U
#define SVGA_WAIT_LOOPS     500000

// 2012.001 keeps hardware cursor disabled until VMware host behavior is
// validated. The cursor previously shared the command FIFO with UPDATE and a
// rejected cursor command could stop all subsequent screen updates.
#define VMWARE_ENABLE_HARDWARE_CURSOR 0

static const pci_device_t* g_pci;
static int g_probed;
static int g_active;
static int g_fifo_ready;
static int g_cursor_ready;
static int g_cursor_visible;
static int g_reg_mmio;
static unsigned int g_io_index;
static unsigned int g_io_data;
static unsigned int g_reg_mmio_base;
static unsigned int g_fb_base;
static unsigned int g_fifo_phys;
static unsigned int g_fifo_size;
static unsigned int g_fifo_min;
static unsigned int g_fifo_max;
static unsigned int g_vram_size;
static unsigned int g_capabilities;
static unsigned int g_interface_id;
static unsigned int g_max_width;
static unsigned int g_max_height;
static int g_width;
static int g_height;
static int g_bpp;
static int g_pitch;
static const char* g_name = "VMware SVGA II";
static const char* g_status = "Not detected";
static volatile unsigned int* g_fifo;

static unsigned int svga_read_reg(unsigned int reg)
{
    if (g_reg_mmio) {
        mmio_write32((void*)(g_reg_mmio_base), reg);
        return mmio_read32((void*)(g_reg_mmio_base + 4));
    }
    outl((unsigned short)g_io_index, reg);
    return inl((unsigned short)g_io_data);
}

static void svga_write_reg(unsigned int reg, unsigned int value)
{
    if (g_reg_mmio) {
        mmio_write32((void*)(g_reg_mmio_base), reg);
        mmio_write32((void*)(g_reg_mmio_base + 4), value);
        return;
    }
    outl((unsigned short)g_io_index, reg);
    outl((unsigned short)g_io_data, value);
}

static void svga_fifo_write(unsigned int index, unsigned int value)
{
    mmio_write32((void*)(g_fifo + index), value);
}

static unsigned int svga_fifo_read(unsigned int index)
{
    return mmio_read32((void*)(g_fifo + index));
}

static unsigned int svga_bytes_per_pixel(int bpp)
{
    if (bpp <= 8) return 1;
    if (bpp <= 16) return 2;
    if (bpp <= 24) return 3;
    return 4;
}

void vmware_svga_sync(void)
{
    if (!g_active) return;
    svga_write_reg(SVGA_REG_SYNC, 1);
    for (int i = 0; i < SVGA_WAIT_LOOPS; i++) {
        if (!(svga_read_reg(SVGA_REG_BUSY) & 1U)) return;
        outb(0x80, 0);
    }
    g_fifo_ready = 0;
    g_status = "FIFO timeout (linear framebuffer fallback)";
    log_printf("[SVGA] FIFO sync timeout (busy=%u)\n",
               svga_read_reg(SVGA_REG_BUSY));
}

static int svga_fifo_emit(const unsigned int* words, unsigned int count)
{
    if (!g_fifo_ready || !words || count == 0 || count > 160) return 0;

    for (int attempt = 0; attempt < 4; attempt++) {
        unsigned int next = svga_fifo_read(SVGA_FIFO_NEXT_CMD);
        unsigned int stop = svga_fifo_read(SVGA_FIFO_STOP);

        if (next < g_fifo_min || next > g_fifo_max) next = g_fifo_min;
        if (stop < g_fifo_min || stop > g_fifo_max) stop = g_fifo_min;

        unsigned int free_bytes;
        if (next >= stop) {
            free_bytes = (g_fifo_max - next) + (stop - g_fifo_min);
        } else {
            free_bytes = stop - next;
        }

        if (free_bytes >= count * 4U + 4U) {
            unsigned int cursor = next;
            for (unsigned int i = 0; i < count; i++) {
                mmio_write32((void*)((unsigned char*)g_fifo + cursor),
                             words[i]);
                cursor += 4U;
                if (cursor == g_fifo_max) cursor = g_fifo_min;
            }
            mmio_barrier();
            svga_fifo_write(SVGA_FIFO_NEXT_CMD, cursor);
            mmio_barrier();
            svga_fifo_write(SVGA_FIFO_COMMIT, cursor);
            return 1;
        }

        vmware_svga_sync();
        if (!g_fifo_ready) return 0;
    }
    return 0;
}

static int svga_normalize_rect(int* x, int* y, int* width, int* height)
{
    if (*x < 0) {
        *width += *x;
        *x = 0;
    }
    if (*y < 0) {
        *height += *y;
        *y = 0;
    }
    if (*x + *width > g_width) *width = g_width - *x;
    if (*y + *height > g_height) *height = g_height - *y;
    return *width > 0 && *height > 0 && *x >= 0 && *y >= 0;
}

#if VMWARE_ENABLE_HARDWARE_CURSOR
static int svga_define_cursor(void)
{
    static const char* shape[16] = {
        "X               ",
        "XX              ",
        "X.X             ",
        "X..X            ",
        "X...X           ",
        "X....X          ",
        "X.....X         ",
        "X......X        ",
        "X.......X       ",
        "X........X      ",
        "X.........X     ",
        "X......XXXX     ",
        "X..X..X         ",
        "X.X X..X        ",
        "XX  X..X        ",
        "     XX         "
    };

    unsigned int command[40];

    for (int row = 0; row < 16; row++) {
        unsigned short and_mask = 0;
        unsigned short xor_mask = 0;
        for (int column = 0; column < 16; column++) {
            unsigned short bit = (unsigned short)(0x8000U >> column);
            char pixel = shape[row][column];
            if (pixel == 'X') {
                xor_mask |= bit;
            } else if (pixel == '.') {
                and_mask &= (unsigned short)~bit;
                xor_mask &= (unsigned short)~bit;
            } else {
                and_mask |= bit;
            }
        }
        command[8 + row] = and_mask;
        command[8 + 16 + row] = xor_mask;
    }

    command[0] = SVGA_CMD_DEFINE_CURSOR;
    command[1] = 0;
    command[2] = 0;
    command[3] = 0;
    command[4] = 16;
    command[5] = 16;
    command[6] = 1;
    command[7] = 1;
    return svga_fifo_emit(command, 40);
}
#else
static int svga_define_cursor(void)
{
    return 0;
}
#endif

static int svga_init_fifo(void)
{
    g_fifo_ready = 0;
    if (!g_fifo || !g_fifo_phys) return 0;

    g_fifo_size = svga_read_reg(SVGA_REG_MEM_SIZE);
    if (g_fifo_size < SVGA_FIFO_MIN_SIZE) {
        log_printf("[SVGA] FIFO unavailable or too small: %u bytes\n",
                   g_fifo_size);
        return 0;
    }

    g_fifo_min = SVGA_FIFO_MIN_BYTES;
    if (g_fifo_min + 8192U >= g_fifo_size) g_fifo_min = 0;
    g_fifo_max = g_fifo_size;

    svga_fifo_write(SVGA_FIFO_MIN, g_fifo_min);
    svga_fifo_write(SVGA_FIFO_MAX, g_fifo_max);
    svga_fifo_write(SVGA_FIFO_NEXT_CMD, g_fifo_min);
    svga_fifo_write(SVGA_FIFO_STOP, g_fifo_min);
    svga_fifo_write(SVGA_FIFO_COMMIT, g_fifo_min);
    g_fifo_ready = 1;
    return 1;
}

static void svga_restore_previous_mode(unsigned int enabled,
                                       unsigned int config_done,
                                       unsigned int width,
                                       unsigned int height,
                                       unsigned int bpp)
{
    if (!enabled || width == 0 || height == 0 || bpp == 0) {
        svga_write_reg(SVGA_REG_ENABLE, 0);
        return;
    }
    svga_write_reg(SVGA_REG_WIDTH, width);
    svga_write_reg(SVGA_REG_HEIGHT, height);
    svga_write_reg(SVGA_REG_BPP, bpp);
    svga_write_reg(SVGA_REG_ENABLE, 1);
    svga_write_reg(SVGA_REG_CONFIG_DONE, config_done);
}

int vmware_svga_probe(void)
{
    g_probed = 1;
    g_active = 0;
    g_fifo_ready = 0;
    g_cursor_ready = 0;
    g_pci = 0;
    g_fifo = 0;
    g_status = "SVGA device not found";

    pci_scan_all();
    g_pci = pci_find_by_id(VMWARE_PCI_VENDOR_ID, VMWARE_SVGA_II_ID);
    if (!g_pci) {
        g_pci = pci_find_by_id(VMWARE_PCI_VENDOR_ID,
                               VMWARE_SVGA_II_FUSION_ID);
    }
    if (!g_pci) {
        g_pci = pci_find_by_id(VMWARE_PCI_VENDOR_ID,
                               VMWARE_SVGA_ADAPTER_ID);
    }
    if (!g_pci) return 0;

    if (g_pci->device == VMWARE_SVGA_II_FUSION_ID) {
        g_name = "VMware SVGA II (Fusion)";
    } else if (g_pci->device == VMWARE_SVGA_ADAPTER_ID) {
        g_name = "VMware SVGA Adapter";
    } else {
        g_name = "VMware SVGA II";
    }

    g_reg_mmio = 0;
    g_io_index = 0;
    g_io_data = 0;
    g_reg_mmio_base = 0;
    if (g_pci->bar_valid[0] && g_pci->bar_is_io[0]) {
        g_io_index = g_pci->bar[0];
        g_io_data = g_pci->bar[0] + 1;
    } else if (g_pci->bar_valid[0] && !g_pci->bar_is_io[0]) {
        g_reg_mmio = 1;
        g_reg_mmio_base = g_pci->bar[0];
    } else {
        g_status = "SVGA BAR0 invalid";
        return 0;
    }

    if (!g_pci->bar_valid[1] || g_pci->bar_is_io[1]) {
        g_status = "SVGA BAR1 framebuffer invalid";
        return 0;
    }

    pci_enable_io(g_pci);
    pci_enable_memory(g_pci);
    pci_enable_bus_master(g_pci);

    g_interface_id = svga_read_reg(SVGA_REG_ID);
    if ((g_interface_id & 0xFFFF0000U) != SVGA_ID_0) {
        log_printf("[SVGA] unsupported interface ID 0x%X\n", g_interface_id);
        g_status = "Unsupported SVGA interface";
        return 0;
    }

    g_capabilities = svga_read_reg(SVGA_REG_CAPABILITIES);
    g_vram_size = svga_read_reg(SVGA_REG_VRAM_SIZE);
    g_max_width = svga_read_reg(SVGA_REG_MAX_WIDTH);
    g_max_height = svga_read_reg(SVGA_REG_MAX_HEIGHT);

    if (g_pci->bar_valid[2] && !g_pci->bar_is_io[2]) {
        g_fifo_phys = g_pci->bar[2];
        g_fifo = (volatile unsigned int*)g_fifo_phys;
    }

    g_probed = 1;
    g_status = "Detected, mode not configured";
    log_printf("[SVGA] %s id=0x%X caps=0x%X vram=%uKB max=%ux%u\n",
               g_name, g_interface_id, g_capabilities,
               g_vram_size / 1024U, g_max_width, g_max_height);
    return 1;
}

int vmware_svga_set_mode(int width, int height, int bpp)
{
    if (!g_probed && !vmware_svga_probe()) return 0;
    if (!g_pci || width <= 0 || height <= 0) return 0;
    if (width > 1024 || height > 768) return 0;
    if (g_max_width && (unsigned int)width > g_max_width) return 0;
    if (g_max_height && (unsigned int)height > g_max_height) return 0;
    if (bpp != 8 && bpp != 15 && bpp != 16 && bpp != 24 && bpp != 32) {
        return 0;
    }

    unsigned int required = (unsigned int)width * (unsigned int)height *
                            svga_bytes_per_pixel(bpp);
    if (g_vram_size && required > g_vram_size) return 0;
    if (!g_reg_mmio && (!g_io_index || !g_io_data)) return 0;

    unsigned int old_enabled = svga_read_reg(SVGA_REG_ENABLE);
    unsigned int old_config_done = svga_read_reg(SVGA_REG_CONFIG_DONE);
    unsigned int old_width = svga_read_reg(SVGA_REG_WIDTH);
    unsigned int old_height = svga_read_reg(SVGA_REG_HEIGHT);
    unsigned int old_bpp = svga_read_reg(SVGA_REG_BPP);

    g_active = 0;
    g_cursor_ready = 0;
    svga_write_reg(SVGA_REG_CONFIG_DONE, 0);
    svga_write_reg(SVGA_REG_ENABLE, 0);

    svga_write_reg(SVGA_REG_ID, SVGA_ID_2);
    unsigned int negotiated_id = svga_read_reg(SVGA_REG_ID);
    if ((negotiated_id & 0xFFFF0000U) != SVGA_ID_0) {
        log_printf("[SVGA] ID negotiation failed: 0x%X\n", negotiated_id);
        svga_restore_previous_mode(old_enabled, old_config_done,
                                   old_width, old_height, old_bpp);
        g_status = "SVGA ID negotiation failed";
        return 0;
    }
    g_interface_id = negotiated_id;

    svga_write_reg(SVGA_REG_WIDTH, (unsigned int)width);
    svga_write_reg(SVGA_REG_HEIGHT, (unsigned int)height);
    svga_write_reg(SVGA_REG_BPP, (unsigned int)bpp);
    svga_write_reg(SVGA_REG_ENABLE, 1);

    unsigned int pitch = svga_read_reg(SVGA_REG_BYTES_PER_LINE);
    unsigned int framebuffer = svga_read_reg(SVGA_REG_FB_START);
    unsigned int minimum_pitch = (unsigned int)width *
                                 svga_bytes_per_pixel(bpp);

    if (!framebuffer || pitch < minimum_pitch) {
        log_printf("[SVGA] mode rejected fb=0x%X pitch=%u min=%u\n",
                   framebuffer, pitch, minimum_pitch);
        svga_restore_previous_mode(old_enabled, old_config_done,
                                   old_width, old_height, old_bpp);
        g_status = "Mode configuration failed";
        return 0;
    }

    g_fb_base = framebuffer;
    g_width = width;
    g_height = height;
    g_bpp = bpp;
    g_pitch = (int)pitch;
    g_active = 1;

    svga_init_fifo();
    svga_write_reg(SVGA_REG_CONFIG_DONE, 1);

    if (g_fifo_ready && VMWARE_ENABLE_HARDWARE_CURSOR) {
        g_cursor_ready = svga_define_cursor();
        if (g_cursor_ready) {
            svga_write_reg(SVGA_REG_CURSOR_ID, 0);
            svga_write_reg(SVGA_REG_CURSOR_ON, SVGA_CURSOR_HIDE);
        }
    } else {
        svga_write_reg(SVGA_REG_CURSOR_ON, SVGA_CURSOR_HIDE);
    }

    if (g_fifo_ready) {
        if (g_capabilities & SVGA_CAP_3D) {
            g_status = "3D-capable profile: FIFO 2D, 2D-only mode";
        } else {
            g_status = g_cursor_ready ? "FIFO 2D + hardware cursor"
                                      : "FIFO 2D (software cursor fallback)";
        }
    } else {
        g_status = "Linear framebuffer fallback";
    }

    log_printf("[SVGA] mode %dx%dx%d pitch=%u fb=0x%X fifo=%s cursor=%s\n",
               width, height, bpp, pitch, framebuffer,
               g_fifo_ready ? "ready" : "fallback",
               g_cursor_ready ? "ready" : "software");
    if (g_capabilities & SVGA_CAP_3D) {
        log_write("[SVGA] 3D-capable profile accepted; FIFO 2D retained\n");
    }
    return 1;
}

int vmware_svga_is_active(void)
{
    return g_active;
}

int vmware_svga_is_3d_capable(void)
{
    return (g_capabilities & SVGA_CAP_3D) != 0;
}

int vmware_svga_supports_mode(int width, int height, int bpp)
{
    if (width <= 0 || height <= 0 || width > 1024 || height > 768) return 0;
    if (bpp != 8 && bpp != 15 && bpp != 16 && bpp != 24 && bpp != 32) {
        return 0;
    }
    if (g_max_width && (unsigned int)width > g_max_width) return 0;
    if (g_max_height && (unsigned int)height > g_max_height) return 0;
    unsigned int required = (unsigned int)width * (unsigned int)height *
                            svga_bytes_per_pixel(bpp);
    return !g_vram_size || required <= g_vram_size;
}

const char* vmware_svga_get_name(void)
{
    return g_name;
}

const char* vmware_svga_get_status(void)
{
    return g_status;
}

unsigned int vmware_svga_get_vram_size(void)
{
    return g_vram_size;
}

unsigned int vmware_svga_get_capabilities(void)
{
    return g_capabilities;
}

unsigned int vmware_svga_get_framebuffer(void)
{
    return g_fb_base;
}

int vmware_svga_get_pitch(void)
{
    return g_pitch;
}

int vmware_svga_fifo_ready(void)
{
    return g_fifo_ready;
}

int vmware_svga_cursor_ready(void)
{
    return g_cursor_ready;
}

void vmware_svga_present_rect(int x, int y, int width, int height)
{
    if (!g_active || !g_fifo_ready ||
        !svga_normalize_rect(&x, &y, &width, &height)) {
        return;
    }

    unsigned int command[5];
    command[0] = SVGA_CMD_UPDATE;
    command[1] = (unsigned int)x;
    command[2] = (unsigned int)y;
    command[3] = (unsigned int)width;
    command[4] = (unsigned int)height;
    if (svga_fifo_emit(command, 5)) {
        vmware_svga_sync();
    } else {
        g_fifo_ready = 0;
        g_status = "FIFO full (linear framebuffer fallback)";
    }
}

static void svga_software_fill(int x, int y, int width, int height,
                               unsigned int color)
{
    unsigned int bytes = svga_bytes_per_pixel(g_bpp);
    unsigned char* framebuffer = (unsigned char*)g_fb_base;
    for (int row = y; row < y + height; row++) {
        unsigned char* pixel = framebuffer + row * g_pitch + x * bytes;
        if (bytes == 4) {
            unsigned int* p = (unsigned int*)pixel;
            for (int column = 0; column < width; column++) p[column] = color;
        } else if (bytes == 2) {
            unsigned short value = (unsigned short)color;
            unsigned short* p = (unsigned short*)pixel;
            for (int column = 0; column < width; column++) p[column] = value;
        } else if (bytes == 3) {
            for (int column = 0; column < width; column++) {
                pixel[column * 3] = (unsigned char)color;
                pixel[column * 3 + 1] = (unsigned char)(color >> 8);
                pixel[column * 3 + 2] = (unsigned char)(color >> 16);
            }
        } else {
            for (int column = 0; column < width; column++) {
                pixel[column] = (unsigned char)color;
            }
        }
    }
}

void vmware_svga_fill_rect(int x, int y, int width, int height,
                           unsigned int color)
{
    int original_x = x;
    int original_y = y;
    int original_width = width;
    int original_height = height;
    if (!g_active || !svga_normalize_rect(&x, &y, &width, &height)) return;

    int accelerated = 0;
    if (g_fifo_ready && (g_capabilities & SVGA_CAP_RECT_FILL)) {
        unsigned int command[6];
        command[0] = SVGA_CMD_RECT_FILL;
        command[1] = color;
        command[2] = (unsigned int)x;
        command[3] = (unsigned int)y;
        command[4] = (unsigned int)width;
        command[5] = (unsigned int)height;
        accelerated = svga_fifo_emit(command, 6);
    }

    if (!accelerated) {
        svga_software_fill(x, y, width, height, color);
    }
    vmware_svga_present_rect(original_x, original_y,
                             original_width, original_height);
}

static void svga_software_copy(int sx, int sy, int dx, int dy,
                               int width, int height)
{
    unsigned int bytes = svga_bytes_per_pixel(g_bpp);
    unsigned char* framebuffer = (unsigned char*)g_fb_base;
    int row_start = height - 1;
    int row_end = -1;
    int row_step = -1;
    if (dy < sy) {
        row_start = 0;
        row_end = height;
        row_step = 1;
    }

    for (int row = row_start; row != row_end; row += row_step) {
        unsigned char* source =
            framebuffer + (sy + row) * g_pitch + sx * bytes;
        unsigned char* destination =
            framebuffer + (dy + row) * g_pitch + dx * bytes;
        if (bytes == 4) {
            unsigned int* s = (unsigned int*)source;
            unsigned int* d = (unsigned int*)destination;
            for (int column = 0; column < width; column++) {
                d[column] = s[column];
            }
        } else if (bytes == 2) {
            unsigned short* s = (unsigned short*)source;
            unsigned short* d = (unsigned short*)destination;
            for (int column = 0; column < width; column++) {
                d[column] = s[column];
            }
        } else {
            int count = width * (int)bytes;
            for (int column = 0; column < count; column++) {
                destination[column] = source[column];
            }
        }
    }
}

void vmware_svga_screen_blt(int sx, int sy, int dx, int dy,
                            int width, int height)
{
    if (!g_active) return;
    if (sx < 0) { width += sx; dx -= sx; sx = 0; }
    if (sy < 0) { height += sy; dy -= sy; sy = 0; }
    if (dx < 0) { width += dx; sx -= dx; dx = 0; }
    if (dy < 0) { height += dy; sy -= dy; dy = 0; }
    if (sx + width > g_width) width = g_width - sx;
    if (sy + height > g_height) height = g_height - sy;
    if (dx + width > g_width) width = g_width - dx;
    if (dy + height > g_height) height = g_height - dy;
    if (width <= 0 || height <= 0) return;

    int accelerated = 0;
    if (g_fifo_ready && (g_capabilities & SVGA_CAP_RECT_COPY)) {
        unsigned int command[7];
        command[0] = SVGA_CMD_RECT_COPY;
        command[1] = (unsigned int)sx;
        command[2] = (unsigned int)sy;
        command[3] = (unsigned int)dx;
        command[4] = (unsigned int)dy;
        command[5] = (unsigned int)width;
        command[6] = (unsigned int)height;
        accelerated = svga_fifo_emit(command, 7);
    }
    if (!accelerated) svga_software_copy(sx, sy, dx, dy, width, height);
    vmware_svga_present_rect(dx, dy, width, height);
}

int vmware_svga_set_cursor(int x, int y, int visible)
{
    if (!g_cursor_ready) return 0;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= g_width) x = g_width - 1;
    if (y >= g_height) y = g_height - 1;

    unsigned int command[4];
    command[0] = SVGA_CMD_MOVE_CURSOR;
    command[1] = 0;
    command[2] = (unsigned int)x;
    command[3] = (unsigned int)y;
    int moved = svga_fifo_emit(command, 4);
    if (moved) vmware_svga_sync();

    svga_write_reg(SVGA_REG_CURSOR_X, (unsigned int)x);
    svga_write_reg(SVGA_REG_CURSOR_Y, (unsigned int)y);
    int show = visible != 0;
    if (show != g_cursor_visible) {
        svga_write_reg(SVGA_REG_CURSOR_ON,
                       show ? SVGA_CURSOR_SHOW : SVGA_CURSOR_HIDE);
        g_cursor_visible = show;
    }
    return 1;
}

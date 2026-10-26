#ifndef VMWARE_SVGA_H
#define VMWARE_SVGA_H

#define VMWARE_PCI_VENDOR_ID 0x15AD
#define VMWARE_SVGA_II_ID    0x0405
#define VMWARE_SVGA_II_FUSION_ID 0x0406
#define VMWARE_SVGA_ADAPTER_ID   0x0710

#define SVGA_CAP_RECT_FILL  0x00000001U
#define SVGA_CAP_RECT_COPY  0x00000002U
#define SVGA_CAP_ALPHA_CURSOR 0x00000010U
#define SVGA_CAP_3D         0x00000200U
#define SVGA_CAP_EXTENDED_FIFO 0x00000400U

int vmware_svga_probe(void);
int vmware_svga_is_active(void);
int vmware_svga_is_3d_capable(void);
int vmware_svga_supports_mode(int width, int height, int bpp);
int vmware_svga_set_mode(int width, int height, int bpp);
const char* vmware_svga_get_name(void);
const char* vmware_svga_get_status(void);
unsigned int vmware_svga_get_vram_size(void);
unsigned int vmware_svga_get_capabilities(void);
unsigned int vmware_svga_get_framebuffer(void);
int vmware_svga_get_pitch(void);
int vmware_svga_fifo_ready(void);
int vmware_svga_cursor_ready(void);
void vmware_svga_present_rect(int x, int y, int width, int height);
void vmware_svga_fill_rect(int x, int y, int width, int height,
                           unsigned int color);
void vmware_svga_screen_blt(int sx, int sy, int dx, int dy,
                            int width, int height);
void vmware_svga_sync(void);
int vmware_svga_set_cursor(int x, int y, int visible);

#endif

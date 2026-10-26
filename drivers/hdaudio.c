#include "hdaudio.h"
#include "pci.h"
#include "port.h"
#include "mmio.h"
#include "dma.h"
#include "log.h"

#ifndef NULL
#define NULL ((void*)0)
#endif

#define HDA_REG_GCAP            0x00
#define HDA_REG_VMIN            0x02
#define HDA_REG_VMAJ            0x03
#define HDA_REG_OUTPAY          0x04
#define HDA_REG_INPAY           0x06
#define HDA_REG_GCTL            0x08
#define HDA_REG_STATESTS        0x0E
#define HDA_REG_INTCTL          0x20
#define HDA_REG_INTSTS          0x24

#define HDA_REG_CORBLBASE       0x40
#define HDA_REG_CORBUBASE       0x44
#define HDA_REG_CORBWP          0x48
#define HDA_REG_CORBRP          0x4A
#define HDA_REG_CORBCTL         0x4C
#define HDA_REG_CORBSTS         0x4D
#define HDA_REG_CORBSIZE        0x4E
#define HDA_REG_RIRBLBASE       0x50
#define HDA_REG_RIRBUBASE       0x54
#define HDA_REG_RIRBWP          0x58
#define HDA_REG_RINTCNT         0x5A
#define HDA_REG_RIRBCTL         0x5C
#define HDA_REG_RIRBSTS         0x5D
#define HDA_REG_RIRBSIZE        0x5E
#define HDA_REG_IC              0x60
#define HDA_REG_IR              0x64
#define HDA_REG_IRS             0x68
#define HDA_REG_DPLBASE         0x70
#define HDA_REG_DPUBASE         0x74

#define HDA_GCTL_RESET          (1U << 0)
#define HDA_CORBRP_RESET        (1U << 15)
#define HDA_RIRBWP_RESET        (1U << 15)
#define HDA_CORBCTL_RUN         (1U << 1)
#define HDA_RIRBCTL_DMA         (1U << 1)
#define HDA_IRS_VALID           (1U << 1)
#define HDA_IRS_BUSY            (1U << 0)

#define HDA_SD_OUT0             0x100
#define HDA_SD_CTL              0x00
#define HDA_SD_STS              0x03
#define HDA_SD_LPIB             0x04
#define HDA_SD_CBL              0x08
#define HDA_SD_LVI              0x0C
#define HDA_SD_FORMAT           0x12
#define HDA_SD_BDLPL            0x18
#define HDA_SD_BDLPU            0x1C
#define HDA_SD_RESET            (1U << 0)
#define HDA_SD_RUN              (1U << 1)
#define HDA_SD_ERRORS           0x18

#define HDA_VERB_PARAMETERS          0x0F00
#define HDA_VERB_GET_CONFIG_DEFAULT  0x0F1C
#define HDA_VERB_GET_CONNECT_LIST    0x0F02
#define HDA_VERB_SET_STREAM_FORMAT   0x0200
#define HDA_VERB_SET_AMP_GAIN_MUTE   0x0300
#define HDA_VERB_SET_CONNECT_SEL     0x0701
#define HDA_VERB_SET_POWER_STATE     0x0705
#define HDA_VERB_SET_CHANNEL_STREAM  0x0706
#define HDA_VERB_SET_PIN_CONTROL     0x0707
#define HDA_VERB_SET_EAPD            0x070C

#define HDA_PAR_VENDOR_ID       0x00
#define HDA_PAR_NODE_COUNT      0x04
#define HDA_PAR_AUDIO_FG_CAP    0x08
#define HDA_PAR_WIDGET_CAP      0x09
#define HDA_PAR_PIN_CAP         0x0C
#define HDA_PAR_AMP_IN_CAP      0x0D
#define HDA_PAR_CONNECTION_LEN  0x0E
#define HDA_PAR_AMP_OUT_CAP     0x12

#define HDA_WCAP_IN_AMP         (1U << 1)
#define HDA_WCAP_OUT_AMP        (1U << 2)
#define HDA_WCAP_CONN_LIST      (1U << 8)
#define HDA_WCAP_DIGITAL        (1U << 9)
#define HDA_WCAP_POWER          (1U << 10)
#define HDA_WCAP_TYPE_SHIFT     20
#define HDA_WCAP_TYPE_MASK      0x0FU

#define HDA_WIDGET_AUDIO_OUT    0
#define HDA_WIDGET_AUDIO_IN     1
#define HDA_WIDGET_MIXER        2
#define HDA_WIDGET_SELECTOR     3
#define HDA_WIDGET_PIN          4

#define HDA_PIN_OUTPUT_CAP      (1U << 4)
#define HDA_PINCTL_OUTPUT       (1U << 6)
#define HDA_PINCTL_HEADPHONE    (1U << 7)

#define HDA_AMP_MUTE            (1U << 7)
#define HDA_AMP_INPUT           (1U << 14)
#define HDA_AMP_OUTPUT          (1U << 15)
#define HDA_AMP_LEFT            (1U << 13)
#define HDA_AMP_RIGHT           (1U << 12)

#define HDA_FORMAT_44K          (1U << 14)
#define HDA_FORMAT_16BIT        (1U << 4)
#define HDA_FORMAT_STEREO       1U

#define HDA_MAX_WIDGETS 64
#define HDA_MAX_CONNECTIONS 8
#define HDA_MAX_ROUTE 8
#define HDA_CORB_ENTRIES 256
#define HDA_RIRB_ENTRIES 256
#define HDA_PERIOD_BYTES 16384U
#define HDA_PERIOD_COUNT 4U
#define HDA_DMA_BYTES 65536U
#define HDA_HALF_BYTES 32768U
#define HDA_STREAM_TAG 1U
#define HDA_WAIT_LOOPS 200000

typedef struct {
    unsigned int response;
    unsigned int response_ex;
} hda_rirb_entry_t;

typedef struct {
    unsigned int address;
    unsigned int length;
    unsigned int flags;
} hda_bdl_entry_t;

typedef struct {
    unsigned short nid;
    unsigned int caps;
    unsigned int pin_cap;
    unsigned int amp_in_cap;
    unsigned int amp_out_cap;
    unsigned int config_default;
    unsigned char type;
    unsigned char conn_count;
    unsigned short conn[HDA_MAX_CONNECTIONS];
} hda_widget_t;

static const pci_device_t* g_pci;
static volatile unsigned char* g_regs;
static unsigned int g_controller_id;
static unsigned int g_gcap;
static unsigned int g_codec_mask;
static unsigned int g_codec_addr;
static unsigned int g_codec_id;
static unsigned int g_stream_base = HDA_SD_OUT0;
static unsigned int g_stream_format;
static int g_rate;
static int g_present;
static int g_running;
static int g_paused;
static int g_corb_ready;
static int g_is_via;
static int g_is_via_1708s;
static int g_volume = 85;
static int g_last_current_half = -1;
static unsigned int g_corb_wp;

static dma_buffer_t g_corb;
static dma_buffer_t g_rirb;
static dma_buffer_t g_pcm;
static dma_buffer_t g_bdl;

static int g_widget_count;
static hda_widget_t g_widgets[HDA_MAX_WIDGETS];
static int g_best_route[HDA_MAX_ROUTE];
static int g_best_route_count;
static int g_best_score;
static int g_tmp_route[HDA_MAX_ROUTE];
static int g_tmp_input_index[HDA_MAX_ROUTE];
static int g_best_input_index[HDA_MAX_ROUTE];
static unsigned short g_output_pin;
static unsigned char g_name[80];
static const char* g_status = "Not detected";

static unsigned char hda_read8(unsigned int reg)
{
    return mmio_read8(g_regs + reg);
}

static unsigned short hda_read16(unsigned int reg)
{
    return mmio_read16(g_regs + reg);
}

static unsigned int hda_read32(unsigned int reg)
{
    return mmio_read32(g_regs + reg);
}

static void hda_write8(unsigned int reg, unsigned char value)
{
    mmio_write8(g_regs + reg, value);
}

static void hda_write16(unsigned int reg, unsigned short value)
{
    mmio_write16(g_regs + reg, value);
}

static void hda_write32(unsigned int reg, unsigned int value)
{
    mmio_write32(g_regs + reg, value);
}

static void hda_delay(void)
{
    outb(0x80, 0);
}

static void hda_copy_string(unsigned char* dst, const char* src, int max)
{
    int i = 0;
    while (src && src[i] && i < max - 1) {
        dst[i] = (unsigned char)src[i];
        i++;
    }
    dst[i] = 0;
}

static void hda_append_hex(unsigned int value)
{
    const char* hex = "0123456789ABCDEF";
    int pos = 0;
    while (g_name[pos]) pos++;
    if (pos + 6 < (int)sizeof(g_name)) {
        g_name[pos++] = '0';
        g_name[pos++] = 'x';
        int started = 0;
        for (int shift = 28; shift >= 0; shift -= 4) {
            unsigned int digit = (value >> shift) & 0xF;
            if (digit || started || shift == 0) {
                g_name[pos++] = (unsigned char)hex[digit];
                started = 1;
            }
        }
        g_name[pos] = 0;
    }
}

static int hda_direct_command(unsigned int codec, unsigned short nid,
                              unsigned short verb, unsigned short payload,
                              unsigned int* response)
{
    int ready = 0;
    for (int i = 0; i < HDA_WAIT_LOOPS; i++) {
        if (!(hda_read16(HDA_REG_IRS) & HDA_IRS_BUSY)) {
            ready = 1;
            break;
        }
        hda_delay();
    }
    if (!ready) return 0;
    if (hda_read16(HDA_REG_IRS) & HDA_IRS_VALID) {
        hda_write16(HDA_REG_IRS, HDA_IRS_VALID);
    }

    unsigned int command = (codec << 28) | ((unsigned int)nid << 20) |
                           ((unsigned int)verb << 8) | payload;
    hda_write32(HDA_REG_IC, command);
    for (int i = 0; i < HDA_WAIT_LOOPS; i++) {
        if (hda_read16(HDA_REG_IRS) & HDA_IRS_VALID) {
            *response = hda_read32(HDA_REG_IR);
            hda_write16(HDA_REG_IRS, HDA_IRS_VALID);
            return 1;
        }
        hda_delay();
    }
    return 0;
}

static int hda_corb_command(unsigned int codec, unsigned short nid,
                            unsigned short verb, unsigned short payload,
                            unsigned int* response)
{
    if (!g_corb_ready || !dma_is_valid(&g_corb) ||
        !dma_is_valid(&g_rirb)) {
        return 0;
    }

    volatile unsigned int* corb = (volatile unsigned int*)g_corb.virt;
    volatile hda_rirb_entry_t* rirb =
        (volatile hda_rirb_entry_t*)g_rirb.virt;
    unsigned int old_wp = hda_read16(HDA_REG_RIRBWP) & 0xFFU;
    g_corb_wp = (g_corb_wp + 1U) % HDA_CORB_ENTRIES;
    corb[g_corb_wp] = (codec << 28) | ((unsigned int)nid << 20) |
                      ((unsigned int)verb << 8) | payload;
    dma_flush(&g_corb);
    hda_write16(HDA_REG_CORBWP, (unsigned short)g_corb_wp);

    for (int i = 0; i < HDA_WAIT_LOOPS; i++) {
        unsigned int wp = hda_read16(HDA_REG_RIRBWP) & 0xFFU;
        if (wp != old_wp) {
            unsigned int index = wp % HDA_RIRB_ENTRIES;
            *response = rirb[index].response;
            return 1;
        }
        hda_delay();
    }
    return 0;
}

static int hda_command(unsigned int codec, unsigned short nid,
                       unsigned short verb, unsigned short payload,
                       unsigned int* response)
{
    unsigned int value = 0;
    if (hda_corb_command(codec, nid, verb, payload, &value)) {
        *response = value;
        return 1;
    }
    if (hda_direct_command(codec, nid, verb, payload, &value)) {
        g_corb_ready = 0;
        *response = value;
        return 1;
    }
    return 0;
}

static unsigned int hda_param(unsigned int codec, unsigned short nid,
                              unsigned int parameter)
{
    unsigned int response = 0;
    hda_command(codec, nid, HDA_VERB_PARAMETERS,
                (unsigned short)parameter, &response);
    return response;
}

static int hda_reset_controller(void)
{
    unsigned int control = hda_read32(HDA_REG_GCTL);
    hda_write32(HDA_REG_GCTL, control & ~HDA_GCTL_RESET);
    for (int i = 0; i < HDA_WAIT_LOOPS; i++) {
        if (!(hda_read32(HDA_REG_GCTL) & HDA_GCTL_RESET)) break;
        hda_delay();
    }
    hda_write32(HDA_REG_GCTL, hda_read32(HDA_REG_GCTL) | HDA_GCTL_RESET);
    for (int i = 0; i < HDA_WAIT_LOOPS; i++) {
        if (hda_read32(HDA_REG_GCTL) & HDA_GCTL_RESET) return 1;
        hda_delay();
    }
    return 0;
}

static void hda_init_corb(void)
{
    g_corb_ready = 0;
    if (!dma_is_valid(&g_corb) || !dma_is_valid(&g_rirb)) return;

    hda_write8(HDA_REG_CORBCTL, 0);
    hda_write8(HDA_REG_RIRBCTL, 0);
    hda_write8(HDA_REG_CORBSTS, hda_read8(HDA_REG_CORBSTS));
    hda_write8(HDA_REG_RIRBSTS, hda_read8(HDA_REG_RIRBSTS));

    hda_write32(HDA_REG_CORBLBASE, g_corb.phys);
    hda_write32(HDA_REG_CORBUBASE, 0);
    hda_write32(HDA_REG_RIRBLBASE, g_rirb.phys);
    hda_write32(HDA_REG_RIRBUBASE, 0);
    hda_write8(HDA_REG_CORBSIZE, 2);
    hda_write8(HDA_REG_RIRBSIZE, 2);

    hda_write16(HDA_REG_CORBRP, HDA_CORBRP_RESET);
    for (int i = 0; i < HDA_WAIT_LOOPS; i++) {
        if (!(hda_read16(HDA_REG_CORBRP) & HDA_CORBRP_RESET)) break;
        hda_delay();
    }
    hda_write16(HDA_REG_CORBRP, 0);
    hda_write16(HDA_REG_CORBWP, 0);
    hda_write16(HDA_REG_RIRBWP, HDA_RIRBWP_RESET);
    hda_write16(HDA_REG_RINTCNT, 1);

    hda_write8(HDA_REG_RIRBCTL, HDA_RIRBCTL_DMA);
    hda_write8(HDA_REG_CORBCTL, HDA_CORBCTL_RUN);
    g_corb_wp = 0;
    g_corb_ready = 1;
}

static int hda_is_via_1708s_id(unsigned int id)
{
    if ((id >> 16) != 0x1106) return 0;
    switch (id & 0xFFFFU) {
        case 0x0397:
        case 0x1397:
        case 0x2397:
        case 0x3397:
        case 0x5397:
        case 0x6397:
        case 0x7397:
            return 1;
        default:
            return 0;
    }
}

static int hda_is_via_family(unsigned int id)
{
    if ((id >> 16) != 0x1106) return 0;
    unsigned int device = id & 0xFFFFU;
    return (device >= 0x1708 && device <= 0x170B) ||
           hda_is_via_1708s_id(id);
}

static hda_widget_t* hda_find_widget(unsigned short nid)
{
    for (int i = 0; i < g_widget_count; i++) {
        if (g_widgets[i].nid == nid) return &g_widgets[i];
    }
    return NULL;
}

static int hda_add_widget(unsigned int codec, unsigned short nid, int depth)
{
    if (g_widget_count >= HDA_MAX_WIDGETS || depth > 3 ||
        hda_find_widget(nid)) {
        return 0;
    }

    hda_widget_t* widget = &g_widgets[g_widget_count++];
    widget->nid = nid;
    widget->caps = hda_param(codec, nid, HDA_PAR_WIDGET_CAP);
    widget->type = (unsigned char)((widget->caps >> HDA_WCAP_TYPE_SHIFT) &
                                   HDA_WCAP_TYPE_MASK);
    widget->amp_in_cap = hda_param(codec, nid, HDA_PAR_AMP_IN_CAP);
    widget->amp_out_cap = hda_param(codec, nid, HDA_PAR_AMP_OUT_CAP);
    if (widget->type == HDA_WIDGET_PIN) {
        widget->pin_cap = hda_param(codec, nid, HDA_PAR_PIN_CAP);
        hda_command(codec, nid, HDA_VERB_GET_CONFIG_DEFAULT, 0,
                    &widget->config_default);
    }

    if (widget->caps & HDA_WCAP_CONN_LIST) {
        unsigned int length = hda_param(codec, nid, HDA_PAR_CONNECTION_LEN);
        int count = length & 0x7F;
        int long_form = (length & 0x80) != 0;
        if (count > HDA_MAX_CONNECTIONS) count = HDA_MAX_CONNECTIONS;
        widget->conn_count = (unsigned char)count;
        for (int index = 0; index < count; index += long_form ? 1 : 2) {
            unsigned int list = 0;
            hda_command(codec, nid, HDA_VERB_GET_CONNECT_LIST,
                        (unsigned short)index, &list);
            widget->conn[index] = (unsigned short)(list & 0xFFFFU);
            if (!long_form && index + 1 < count) {
                widget->conn[index + 1] = (unsigned short)(list >> 16);
            }
        }
    }

    unsigned int children = hda_param(codec, nid, HDA_PAR_NODE_COUNT);
    unsigned int start = children >> 16;
    unsigned int count = children & 0xFFU;
    if (count > 32) count = 32;
    for (unsigned int i = 0; i < count; i++) {
        hda_add_widget(codec, (unsigned short)(start + i), depth + 1);
    }
    return 1;
}

static int hda_build_widgets(unsigned int codec)
{
    g_widget_count = 0;
    unsigned int functions = hda_param(codec, 0, HDA_PAR_NODE_COUNT);
    unsigned int start = functions >> 16;
    unsigned int count = functions & 0xFFU;
    if (count > 16) count = 16;
    for (unsigned int i = 0; i < count; i++) {
        unsigned short function_nid = (unsigned short)(start + i);
        unsigned int children =
            hda_param(codec, function_nid, HDA_PAR_NODE_COUNT);
        unsigned int child_start = children >> 16;
        unsigned int child_count = children & 0xFFU;
        if (child_count > 32) child_count = 32;
        for (unsigned int child = 0; child < child_count; child++) {
            hda_add_widget(codec,
                           (unsigned short)(child_start + child), 0);
        }
    }
    return g_widget_count > 0;
}

static int hda_connection_index(const hda_widget_t* widget,
                                unsigned short nid)
{
    for (int i = 0; i < widget->conn_count; i++) {
        if (widget->conn[i] == nid) return i;
    }
    return -1;
}

static int hda_pin_score(const hda_widget_t* widget)
{
    if (!widget || widget->type != HDA_WIDGET_PIN) return -1;
    if (widget->caps & HDA_WCAP_DIGITAL) return -1;
    unsigned int device = (widget->config_default >> 20) & 0xFU;
    int score = 0;
    if (device == 0) score = 400;
    else if (device == 2) score = 300;
    else if (device == 1) score = 200;
    else if (widget->pin_cap & HDA_PIN_OUTPUT_CAP) score = 50;
    else score = -1;
    if (!(widget->pin_cap & HDA_PIN_OUTPUT_CAP)) score -= 100;
    return score;
}

static void hda_route_dfs(int current_index, int depth)
{
    if (depth + 1 >= HDA_MAX_ROUTE) return;
    const hda_widget_t* current = &g_widgets[current_index];

    for (int i = 0; i < g_widget_count; i++) {
        if (i == current_index) continue;
        hda_widget_t* candidate = &g_widgets[i];
        if (candidate->type != HDA_WIDGET_MIXER &&
            candidate->type != HDA_WIDGET_SELECTOR &&
            candidate->type != HDA_WIDGET_PIN) {
            continue;
        }

        int input_index =
            hda_connection_index(candidate, current->nid);
        if (input_index < 0 && g_is_via_1708s &&
            candidate->nid == 0x16 && current->type == HDA_WIDGET_AUDIO_OUT) {
            input_index = candidate->conn_count;
        }
        if (input_index < 0) continue;

        g_tmp_route[depth + 1] = i;
        g_tmp_input_index[depth + 1] = input_index;
        if (candidate->type == HDA_WIDGET_PIN) {
            int score = hda_pin_score(candidate);
            if (score < 0) continue;
            score += 40 - depth * 4;
            if (score > g_best_score) {
                g_best_score = score;
                g_best_route_count = depth + 2;
                for (int j = 0; j < g_best_route_count; j++) {
                    g_best_route[j] = g_tmp_route[j];
                    g_best_input_index[j] = g_tmp_input_index[j];
                }
                g_output_pin = candidate->nid;
            }
            continue;
        }

        int duplicate = 0;
        for (int j = 0; j <= depth; j++) {
            if (g_tmp_route[j] == i) duplicate = 1;
        }
        if (!duplicate) hda_route_dfs(i, depth + 1);
    }
}

static int hda_choose_route(void)
{
    g_best_route_count = 0;
    g_best_score = -1;
    g_output_pin = 0;

    for (int i = 0; i < g_widget_count; i++) {
        if (g_widgets[i].type != HDA_WIDGET_AUDIO_OUT ||
            (g_widgets[i].caps & HDA_WCAP_DIGITAL)) {
            continue;
        }
        g_tmp_route[0] = i;
        g_tmp_input_index[0] = 0;
        hda_route_dfs(i, 0);
    }
    return g_best_route_count >= 2;
}

static unsigned int hda_amp_payload(unsigned int caps, int volume,
                                    int input, int input_index)
{
    unsigned int steps = (caps >> 8) & 0x7FU;
    unsigned int index = 0;
    if (volume <= 0) {
        volume = 0;
        index = steps;
    } else if (volume < 100 && steps) {
        index = (steps * (unsigned int)(100 - volume) + 99U) / 100U;
    }
    unsigned int payload = HDA_AMP_LEFT | HDA_AMP_RIGHT |
                           (index & 0x7FU);
    if (input) payload |= HDA_AMP_INPUT | ((unsigned int)input_index << 8);
    else payload |= HDA_AMP_OUTPUT;
    if (volume == 0) payload |= HDA_AMP_MUTE;
    return payload;
}

static void hda_apply_volume(void)
{
    if (!g_present || g_best_route_count == 0) return;
    for (int i = 0; i < g_best_route_count; i++) {
        hda_widget_t* widget = &g_widgets[g_best_route[i]];
        int input_index = g_best_input_index[i];
        if (widget->type == HDA_WIDGET_MIXER &&
            (widget->caps & HDA_WCAP_IN_AMP) && widget->amp_in_cap &&
            input_index >= 0 && input_index < widget->conn_count) {
            hda_command(g_codec_addr, widget->nid,
                        HDA_VERB_SET_AMP_GAIN_MUTE,
                        (unsigned short)hda_amp_payload(
                            widget->amp_in_cap, g_volume, 1, input_index),
                        0);
        }
        if (widget->caps & HDA_WCAP_OUT_AMP && widget->amp_out_cap) {
            hda_command(g_codec_addr, widget->nid,
                        HDA_VERB_SET_AMP_GAIN_MUTE,
                        (unsigned short)hda_amp_payload(
                            widget->amp_out_cap, g_volume, 0, 0),
                        0);
        }
    }
}

static void hda_apply_route(void)
{
    if (g_best_route_count == 0) return;
    for (int i = 0; i < g_best_route_count; i++) {
        hda_widget_t* widget = &g_widgets[g_best_route[i]];
        if (widget->caps & HDA_WCAP_POWER) {
            hda_command(g_codec_addr, widget->nid,
                        HDA_VERB_SET_POWER_STATE, 0, 0);
        }
        if (widget->type == HDA_WIDGET_SELECTOR &&
            g_best_input_index[i] >= 0 &&
            g_best_input_index[i] < widget->conn_count) {
            hda_command(g_codec_addr, widget->nid,
                        HDA_VERB_SET_CONNECT_SEL,
                        (unsigned short)g_best_input_index[i], 0);
        }
        if (widget->type == HDA_WIDGET_PIN) {
            unsigned int device =
                (widget->config_default >> 20) & 0xFU;
            unsigned int pin = HDA_PINCTL_OUTPUT;
            if (device == 2) pin |= HDA_PINCTL_HEADPHONE;
            hda_command(g_codec_addr, widget->nid,
                        HDA_VERB_SET_PIN_CONTROL,
                        (unsigned short)pin, 0);
            hda_command(g_codec_addr, widget->nid,
                        HDA_VERB_SET_EAPD, 0x0002, 0);
        }
    }
    hda_apply_volume();
}

static int hda_select_codec(void)
{
    g_codec_mask = hda_read16(HDA_REG_STATESTS) & 0xF;
    if (!g_codec_mask) {
        for (int i = 0; i < HDA_WAIT_LOOPS; i++) {
            g_codec_mask = hda_read16(HDA_REG_STATESTS) & 0xF;
            if (g_codec_mask) break;
            hda_delay();
        }
    }
    if (!g_codec_mask) return 0;

    for (unsigned int codec = 0; codec < 4; codec++) {
        if (!(g_codec_mask & (1U << codec))) continue;
        unsigned int codec_id = hda_param(codec, 0, HDA_PAR_VENDOR_ID);
        if (codec_id == 0 || codec_id == 0xFFFFFFFFU) continue;
        unsigned int revision = hda_param(codec, 0, 0x02);
        unsigned int subsystem = hda_param(codec, 0, 0x01);
        log_printf("[HDA ] codec%u id=0x%X rev=0x%X sub=0x%X\n",
                   codec, codec_id, revision, subsystem);

        g_is_via = hda_is_via_family(codec_id);
        g_is_via_1708s = hda_is_via_1708s_id(codec_id);
        if (g_is_via_1708s) {
            hda_command(codec, 1, 0x0F98, 0x0001, 0);
            hda_command(codec, 1, 0x0F88, 0x00C0, 0);
        }

        if (!hda_build_widgets(codec)) continue;
        if (!hda_choose_route()) {
            log_printf("[HDA ] codec%u has no usable output path\n", codec);
            continue;
        }

        g_codec_addr = codec;
        g_codec_id = codec_id;
        for (int route = 0; route < g_best_route_count; route++) {
            hda_widget_t* widget = &g_widgets[g_best_route[route]];
            log_printf("[HDA ] route[%u] nid=0x%X type=%u cfg=0x%X\n",
                       (unsigned int)route, widget->nid, widget->type,
                       widget->config_default);
        }
        hda_apply_route();
        return 1;
    }
    return 0;
}

static void hda_init_bdl(void)
{
    if (!dma_is_valid(&g_pcm) || !dma_is_valid(&g_bdl)) return;
    volatile hda_bdl_entry_t* bdl =
        (volatile hda_bdl_entry_t*)g_bdl.virt;
    for (unsigned int i = 0; i < HDA_PERIOD_COUNT; i++) {
        bdl[i].address = g_pcm.phys + i * HDA_PERIOD_BYTES;
        bdl[i].length = HDA_PERIOD_BYTES;
        bdl[i].flags = 0;
    }
    dma_flush(&g_bdl);
}

static void hda_free_buffers(void)
{
    dma_free(&g_corb);
    dma_free(&g_rirb);
    dma_free(&g_pcm);
    dma_free(&g_bdl);
    g_corb_ready = 0;
}

static int hda_try_controller(const pci_device_t* dev)
{
    if (!dev || !dev->bar_valid[0] || dev->bar_is_io[0]) return 0;
    unsigned char* registers = (unsigned char*)dev->bar[0];
    if (!registers) return 0;

    pci_enable_memory(dev);
    pci_enable_bus_master(dev);
    g_pci = dev;
    g_regs = registers;
    g_controller_id = ((unsigned int)dev->vendor << 16) | dev->device;
    g_gcap = hda_read32(HDA_REG_GCAP);
    unsigned int output_streams = (g_gcap >> 12) & 0xFU;
    log_printf("[HDA ] controller %X:%X gcap=0x%X outs=%u\n",
               dev->vendor, dev->device, g_gcap, output_streams);
    if (!output_streams) return 0;

    hda_write32(HDA_REG_INTCTL, 0);
    if (!hda_reset_controller()) {
        g_status = "Controller reset timeout";
        return 0;
    }

    g_corb = dma_alloc_aligned(HDA_CORB_ENTRIES * 4, 128);
    g_rirb = dma_alloc_aligned(HDA_RIRB_ENTRIES * 8, 128);
    g_pcm = dma_alloc_aligned(HDA_DMA_BYTES, 128);
    g_bdl = dma_alloc_aligned(HDA_PERIOD_COUNT * sizeof(hda_bdl_entry_t),
                              128);
    if (!dma_is_valid(&g_corb) || !dma_is_valid(&g_rirb) ||
        !dma_is_valid(&g_pcm) || !dma_is_valid(&g_bdl)) {
        g_status = "DMA allocation failed";
        hda_free_buffers();
        return 0;
    }

    hda_init_corb();
    if (!g_corb_ready) {
        log_write("[HDA ] CORB/RIRB unavailable, trying immediate commands\n");
    }
    hda_init_bdl();
    if (!hda_select_codec()) {
        g_status = "No routable HDA codec";
        hda_free_buffers();
        return 0;
    }

    g_present = 1;
    g_rate = 48000;
    g_stream_format = HDA_FORMAT_16BIT | HDA_FORMAT_STEREO;
    hdaudio_set_format(g_rate, 16, 1);
    g_status = "48kHz/16-bit stereo ready";
    return 1;
}

int hdaudio_init(void)
{
    g_present = 0;
    g_running = 0;
    g_paused = 0;
    g_corb_ready = 0;
    g_codec_addr = 0;
    g_codec_id = 0;
    g_output_pin = 0;
    g_widget_count = 0;
    g_best_route_count = 0;
    g_last_current_half = -1;
    g_status = "No HDA controller";
    g_name[0] = 0;

    pci_scan_all();
    for (int i = 0; i < pci_get_device_count(); i++) {
        const pci_device_t* dev = pci_get_device(i);
        if (!dev || dev->class_code != 0x04 || dev->subclass != 0x03) {
            continue;
        }
        if (hda_try_controller(dev)) {
            if ((g_codec_id >> 16) == 0x1106 &&
                hda_is_via_1708s_id(g_codec_id)) {
                hda_copy_string(g_name, "VIA VT1708S HDA / pin ",
                                sizeof(g_name));
                hda_append_hex(g_output_pin);
            } else if (g_is_via) {
                hda_copy_string(g_name, "VIA HDA / codec ",
                                sizeof(g_name));
                hda_append_hex(g_codec_id);
            } else if ((g_controller_id >> 16) == 0x15AD) {
                hda_copy_string(g_name, "VMware HDA / codec ",
                                sizeof(g_name));
                hda_append_hex(g_codec_id);
            } else {
                hda_copy_string(g_name, "Intel HDA / codec ",
                                sizeof(g_name));
                hda_append_hex(g_codec_id);
            }
            log_printf("[HDA ] active controller=%X codec=%X pin=%u path=%u\n",
                       g_controller_id, g_codec_id, g_output_pin,
                       (unsigned int)g_best_route_count);
            return 1;
        }
        hda_free_buffers();
    }
    return 0;
}

int hdaudio_is_present(void)
{
    return g_present;
}

const char* hdaudio_get_name(void)
{
    return g_name[0] ? (const char*)g_name : "Intel High Definition Audio";
}

const char* hdaudio_get_status(void)
{
    return g_status;
}

unsigned int hdaudio_get_controller_id(void)
{
    return g_controller_id;
}

unsigned int hdaudio_get_codec_id(void)
{
    return g_codec_id;
}

unsigned short hdaudio_get_output_pin(void)
{
    return g_output_pin;
}

static unsigned int hda_stream_format(int rate)
{
    return HDA_FORMAT_16BIT | HDA_FORMAT_STEREO |
           (rate == 44100 ? HDA_FORMAT_44K : 0);
}

static void hda_stream_reset(void)
{
    unsigned int control = hda_read32(g_stream_base + HDA_SD_CTL);
    hda_write32(g_stream_base + HDA_SD_CTL,
                control & ~(HDA_SD_RUN | HDA_SD_RESET));
    hda_delay();
    hda_write32(g_stream_base + HDA_SD_CTL,
                hda_read32(g_stream_base + HDA_SD_CTL) | HDA_SD_RESET);
    for (int i = 0; i < 1000; i++) hda_delay();
    hda_write32(g_stream_base + HDA_SD_CTL,
                hda_read32(g_stream_base + HDA_SD_CTL) & ~HDA_SD_RESET);
}

void hdaudio_set_format(int rate, int bits, int stereo)
{
    if (bits != 16 || !stereo) return;
    if (rate != 44100 && rate != 48000) rate = 48000;
    if (g_running) hdaudio_stop_dma();
    g_rate = rate;
    g_stream_format = hda_stream_format(rate);
    if (!g_present) return;

    int converter = g_best_route[0];
    if (converter >= 0 && converter < g_widget_count) {
        hda_command(g_codec_addr, g_widgets[converter].nid,
                    HDA_VERB_SET_STREAM_FORMAT,
                    (unsigned short)g_stream_format, 0);
    }
    hda_write16(g_stream_base + HDA_SD_FORMAT,
                (unsigned short)g_stream_format);
    g_status = rate == 44100 ? "44.1kHz/16-bit stereo ready"
                             : "48kHz/16-bit stereo ready";
}

void hdaudio_start_dma(void)
{
    if (!g_present || !dma_is_valid(&g_pcm) || !dma_is_valid(&g_bdl)) return;
    hda_stream_reset();
    hda_write32(g_stream_base + HDA_SD_CBL, HDA_DMA_BYTES);
    hda_write16(g_stream_base + HDA_SD_LVI,
                (unsigned short)(HDA_PERIOD_COUNT - 1));
    hda_write16(g_stream_base + HDA_SD_FORMAT,
                (unsigned short)g_stream_format);
    hda_write32(g_stream_base + HDA_SD_BDLPL, g_bdl.phys);
    hda_write32(g_stream_base + HDA_SD_BDLPU, 0);
    hda_write8(g_stream_base + HDA_SD_STS, HDA_SD_ERRORS);

    int converter = g_best_route[0];
    if (converter >= 0 && converter < g_widget_count) {
        unsigned short stream_id =
            (unsigned short)((HDA_STREAM_TAG << 4) | 0);
        hda_command(g_codec_addr, g_widgets[converter].nid,
                    HDA_VERB_SET_STREAM_FORMAT,
                    (unsigned short)g_stream_format, 0);
        hda_command(g_codec_addr, g_widgets[converter].nid,
                    HDA_VERB_SET_CHANNEL_STREAM, stream_id, 0);
    }

    unsigned int control = HDA_STREAM_TAG << 20;
    hda_write32(g_stream_base + HDA_SD_CTL, control | HDA_SD_RUN);
    g_running = 1;
    g_paused = 0;
    g_last_current_half = 0;
    hda_apply_volume();
}

void hdaudio_stop_dma(void)
{
    if (!g_present) return;
    hda_write32(g_stream_base + HDA_SD_CTL,
                hda_read32(g_stream_base + HDA_SD_CTL) & ~HDA_SD_RUN);
    hda_stream_reset();
    int converter = g_best_route[0];
    if (converter >= 0 && converter < g_widget_count) {
        hda_command(g_codec_addr, g_widgets[converter].nid,
                    HDA_VERB_SET_CHANNEL_STREAM, 0, 0);
    }
    g_running = 0;
    g_paused = 0;
    g_last_current_half = -1;
}

void hdaudio_pause_dma(int pause)
{
    if (!g_present || !g_running) return;
    if (pause) {
        hda_write32(g_stream_base + HDA_SD_CTL,
                    hda_read32(g_stream_base + HDA_SD_CTL) & ~HDA_SD_RUN);
        g_paused = 1;
    } else {
        hda_write32(g_stream_base + HDA_SD_CTL,
                    hda_read32(g_stream_base + HDA_SD_CTL) | HDA_SD_RUN);
        g_paused = 0;
    }
}

void hdaudio_set_volume(int master_vol, int pcm_vol)
{
    int volume = (master_vol + pcm_vol) / 2;
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    g_volume = volume;
    hda_apply_volume();
}

unsigned int hdaudio_get_dma_frame_pos(void)
{
    if (!g_present) return 0;
    unsigned int position = hda_read32(g_stream_base + HDA_SD_LPIB);
    if (position >= HDA_DMA_BYTES) position = 0;
    return position / 4;
}

const void* hdaudio_get_dma_buffer(void)
{
    return dma_is_valid(&g_pcm) ? g_pcm.virt : NULL;
}

void hdaudio_write_half(int half_idx, const void* data, unsigned int bytes)
{
    if (!dma_is_valid(&g_pcm) || (half_idx != 0 && half_idx != 1) ||
        !data) {
        return;
    }
    if (bytes > HDA_HALF_BYTES) bytes = HDA_HALF_BYTES;
    unsigned char* destination =
        (unsigned char*)g_pcm.virt + half_idx * HDA_HALF_BYTES;
    const unsigned char* source = (const unsigned char*)data;
    for (unsigned int i = 0; i < bytes; i++) destination[i] = source[i];
    for (unsigned int i = bytes; i < HDA_HALF_BYTES; i++) destination[i] = 0;
    dma_flush(&g_pcm);
}

void hdaudio_silence_half(int half_idx)
{
    if (!dma_is_valid(&g_pcm) || (half_idx != 0 && half_idx != 1)) return;
    unsigned char* destination =
        (unsigned char*)g_pcm.virt + half_idx * HDA_HALF_BYTES;
    for (unsigned int i = 0; i < HDA_HALF_BYTES; i++) destination[i] = 0;
    dma_flush(&g_pcm);
}

int hdaudio_poll_half_needed(void)
{
    if (!g_present || !g_running || g_paused) return -1;
    unsigned char status = hda_read8(g_stream_base + HDA_SD_STS);
    if (status & HDA_SD_ERRORS) {
        hda_write8(g_stream_base + HDA_SD_STS, status & HDA_SD_ERRORS);
        log_printf("[HDA ] stream error status=0x%X\n", status);
    }

    unsigned int position = hda_read32(g_stream_base + HDA_SD_LPIB);
    if (position >= HDA_DMA_BYTES) position = 0;
    int current_half = position >= HDA_HALF_BYTES ? 1 : 0;
    if (g_last_current_half < 0) {
        g_last_current_half = current_half;
        return -1;
    }
    if (current_half != g_last_current_half) {
        int finished = g_last_current_half;
        g_last_current_half = current_half;
        return finished;
    }
    return -1;
}

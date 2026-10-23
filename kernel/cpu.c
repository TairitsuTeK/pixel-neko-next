#include "cpu.h"
#include "timer.h"

cpu_info_t g_cpu_info = {
    .vendor = "Unknown",
    .brand = "Unknown CPU",
    .family = 0,
    .model = 0,
    .stepping = 0,
    .max_basic_leaf = 0,
    .max_ext_leaf = 0,
    .features_ecx = 0,
    .features_edx = 0,
    .mhz = 0
};

char cpu_model_string[49] = "Unknown CPU";

static inline int has_cpuid(void) {
    unsigned int eax, ecx;
    asm volatile (
        "pushfl\n\t"
        "pushfl\n\t"
        "popl %0\n\t"
        "movl %0, %1\n\t"
        "xorl $0x200000, %0\n\t"
        "pushl %0\n\t"
        "popfl\n\t"
        "pushfl\n\t"
        "popl %0\n\t"
        "popfl\n\t"
        : "=&r" (eax), "=&r" (ecx)
    );
    return ((eax ^ ecx) & 0x200000) != 0;
}

static inline void cpuid(unsigned int leaf, unsigned int *eax, unsigned int *ebx, unsigned int *ecx, unsigned int *edx) {
    unsigned int a, b, c, d;
    asm volatile ("cpuid"
                  : "=a" (a), "=b" (b), "=c" (c), "=d" (d)
                  : "0" (leaf));
    if (eax) *eax = a;
    if (ebx) *ebx = b;
    if (ecx) *ecx = c;
    if (edx) *edx = d;
}

static int str_equal(const char* a, const char* b) {
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == *b;
}

static void str_copy_n(char* dst, const char* src, int max) {
    int i = 0;
    while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static void clean_brand_string(char* dst, const char* src, int max) {
    // 1. Skip leading whitespace
    int s = 0;
    while (src[s] == ' ' || src[s] == '\t') s++;
    
    // 2. Copy and collapse repeated spaces
    int d = 0;
    int prev_space = 0;
    while (src[s] && d < max - 1) {
        char ch = src[s++];
        if (ch == ' ' || ch == '\t') {
            if (!prev_space) {
                dst[d++] = ' ';
                prev_space = 1;
            }
        } else {
            dst[d++] = ch;
            prev_space = 0;
        }
    }
    // Trim trailing space
    while (d > 0 && dst[d - 1] == ' ') d--;
    dst[d] = '\0';
}

void cpu_detect(void) {
    if (!has_cpuid()) {
        str_copy_n(cpu_model_string, "Classic x86 CPU", sizeof(cpu_model_string));
        str_copy_n(g_cpu_info.brand, cpu_model_string, sizeof(g_cpu_info.brand));
        return;
    }

    unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;
    unsigned int max_basic = 0;
    cpuid(0, &max_basic, &ebx, &ecx, &edx);
    g_cpu_info.max_basic_leaf = max_basic;

    char vendor[13];
    *(unsigned int*)&vendor[0] = ebx;
    *(unsigned int*)&vendor[4] = edx;
    *(unsigned int*)&vendor[8] = ecx;
    vendor[12] = '\0';
    str_copy_n(g_cpu_info.vendor, vendor, sizeof(g_cpu_info.vendor));

    unsigned int family = 0, model = 0, stepping = 0, brand_idx = 0;

    if (max_basic >= 1) {
        cpuid(1, &eax, &ebx, &ecx, &edx);
        stepping = eax & 0x0F;
        model = (eax >> 4) & 0x0F;
        family = (eax >> 8) & 0x0F;
        unsigned int ext_model = (eax >> 16) & 0x0F;
        unsigned int ext_family = (eax >> 20) & 0xFF;
        brand_idx = ebx & 0xFF;

        if (family == 6 || family == 15) {
            model |= (ext_model << 4);
        }
        if (family == 15) {
            family += ext_family;
        }

        g_cpu_info.family = family;
        g_cpu_info.model = model;
        g_cpu_info.stepping = stepping;
        g_cpu_info.features_ecx = ecx;
        g_cpu_info.features_edx = edx;
    }

    // 1. Check Extended Brand String (0x80000002..0x80000004)
    // On Pentium Pro, Klamath, Deschutes, Katmai, leaf 0x80000000 returns max basic leaf (<= 3).
    // It only contains extended leaves if ext_max >= 0x80000004.
    unsigned int ext_max = 0;
    cpuid(0x80000000, &ext_max, &ebx, &ecx, &edx);
    g_cpu_info.max_ext_leaf = ext_max;

    if ((ext_max & 0xFFFF0000) == 0x80000000 && ext_max >= 0x80000004) {
        char raw_brand[49];
        unsigned int* p = (unsigned int*)raw_brand;
        for (int i = 0; i < 3; i++) {
            cpuid(0x80000002 + i, &eax, &ebx, &ecx, &edx);
            *p++ = eax; *p++ = ebx; *p++ = ecx; *p++ = edx;
        }
        raw_brand[48] = '\0';

        char cleaned[49];
        clean_brand_string(cleaned, raw_brand, sizeof(cleaned));
        if (cleaned[0] != '\0') {
            str_copy_n(cpu_model_string, cleaned, sizeof(cpu_model_string));
            str_copy_n(g_cpu_info.brand, cleaned, sizeof(g_cpu_info.brand));
            return;
        }
    }

    // 2. Brand Index lookup for Intel (Pentium III Coppermine/Tualatin, etc.)
    if (str_equal(vendor, "GenuineIntel") && brand_idx > 0) {
        switch (brand_idx) {
            case 0x01: str_copy_n(cpu_model_string, "Intel Celeron", sizeof(cpu_model_string)); break;
            case 0x02: str_copy_n(cpu_model_string, "Intel Pentium III", sizeof(cpu_model_string)); break;
            case 0x03: str_copy_n(cpu_model_string, "Intel Pentium III Xeon", sizeof(cpu_model_string)); break;
            case 0x04: str_copy_n(cpu_model_string, "Intel Pentium III", sizeof(cpu_model_string)); break;
            case 0x06: str_copy_n(cpu_model_string, "Intel Pentium III-M", sizeof(cpu_model_string)); break;
            case 0x07: str_copy_n(cpu_model_string, "Intel Celeron-M", sizeof(cpu_model_string)); break;
            case 0x08:
            case 0x09: str_copy_n(cpu_model_string, "Intel Pentium 4", sizeof(cpu_model_string)); break;
            case 0x0A: str_copy_n(cpu_model_string, "Intel Celeron", sizeof(cpu_model_string)); break;
            case 0x0B: str_copy_n(cpu_model_string, "Intel Xeon", sizeof(cpu_model_string)); break;
            case 0x0C: str_copy_n(cpu_model_string, "Intel Xeon MP", sizeof(cpu_model_string)); break;
            case 0x0E: str_copy_n(cpu_model_string, "Intel Pentium 4-M", sizeof(cpu_model_string)); break;
            case 0x0F:
            case 0x13:
            case 0x17: str_copy_n(cpu_model_string, "Intel Celeron Mobile", sizeof(cpu_model_string)); break;
            case 0x12: str_copy_n(cpu_model_string, "Intel Celeron M", sizeof(cpu_model_string)); break;
            case 0x14: str_copy_n(cpu_model_string, "Intel Celeron", sizeof(cpu_model_string)); break;
            case 0x16: str_copy_n(cpu_model_string, "Intel Pentium M", sizeof(cpu_model_string)); break;
            default: break;
        }
        if (str_equal(cpu_model_string, "Unknown CPU") == 0) {
            str_copy_n(g_cpu_info.brand, cpu_model_string, sizeof(g_cpu_info.brand));
            return;
        }
    }

    // 3. Fallback Family / Model Table (Pentium Pro, Pentium II, early Pentium III, etc.)
    if (str_equal(vendor, "GenuineIntel")) {
        if (family == 6) {
            switch (model) {
                case 1:  str_copy_n(cpu_model_string, "Intel Pentium Pro", sizeof(cpu_model_string)); break;
                case 3:  str_copy_n(cpu_model_string, "Intel Pentium II (Klamath)", sizeof(cpu_model_string)); break;
                case 5:  str_copy_n(cpu_model_string, "Intel Pentium II / Celeron (Deschutes)", sizeof(cpu_model_string)); break;
                case 6:  str_copy_n(cpu_model_string, "Intel Celeron (Mendocino)", sizeof(cpu_model_string)); break;
                case 7:  str_copy_n(cpu_model_string, "Intel Pentium III (Katmai)", sizeof(cpu_model_string)); break;
                case 8:  str_copy_n(cpu_model_string, "Intel Pentium III (Coppermine)", sizeof(cpu_model_string)); break;
                case 9:  str_copy_n(cpu_model_string, "Intel Pentium M (Banias)", sizeof(cpu_model_string)); break;
                case 10: str_copy_n(cpu_model_string, "Intel Pentium III Xeon", sizeof(cpu_model_string)); break;
                case 11: str_copy_n(cpu_model_string, "Intel Pentium III (Tualatin)", sizeof(cpu_model_string)); break;
                case 13: str_copy_n(cpu_model_string, "Intel Pentium M (Dothan)", sizeof(cpu_model_string)); break;
                case 14: str_copy_n(cpu_model_string, "Intel Core Duo / Solo (Yonah)", sizeof(cpu_model_string)); break;
                default: str_copy_n(cpu_model_string, "Intel P6 Family CPU", sizeof(cpu_model_string)); break;
            }
            str_copy_n(g_cpu_info.brand, cpu_model_string, sizeof(g_cpu_info.brand));
            return;
        } else if (family == 5) {
            if (model == 4) str_copy_n(cpu_model_string, "Intel Pentium MMX", sizeof(cpu_model_string));
            else str_copy_n(cpu_model_string, "Intel Pentium (P5)", sizeof(cpu_model_string));
            str_copy_n(g_cpu_info.brand, cpu_model_string, sizeof(g_cpu_info.brand));
            return;
        } else if (family == 15) {
            str_copy_n(cpu_model_string, "Intel Pentium 4", sizeof(cpu_model_string));
            str_copy_n(g_cpu_info.brand, cpu_model_string, sizeof(g_cpu_info.brand));
            return;
        }
    } else if (str_equal(vendor, "AuthenticAMD")) {
        if (family == 5) {
            if (model <= 3) str_copy_n(cpu_model_string, "AMD K5", sizeof(cpu_model_string));
            else if (model == 6 || model == 7) str_copy_n(cpu_model_string, "AMD K6", sizeof(cpu_model_string));
            else if (model == 8) str_copy_n(cpu_model_string, "AMD K6-2", sizeof(cpu_model_string));
            else str_copy_n(cpu_model_string, "AMD K6-III", sizeof(cpu_model_string));
            str_copy_n(g_cpu_info.brand, cpu_model_string, sizeof(g_cpu_info.brand));
            return;
        } else if (family == 6) {
            if (model <= 3) str_copy_n(cpu_model_string, "AMD Athlon / Duron", sizeof(cpu_model_string));
            else if (model == 8 || model == 10) str_copy_n(cpu_model_string, "AMD Athlon XP", sizeof(cpu_model_string));
            else str_copy_n(cpu_model_string, "AMD Athlon", sizeof(cpu_model_string));
            str_copy_n(g_cpu_info.brand, cpu_model_string, sizeof(g_cpu_info.brand));
            return;
        } else if (family >= 15) {
            str_copy_n(cpu_model_string, "AMD K8 (Athlon 64)", sizeof(cpu_model_string));
            str_copy_n(g_cpu_info.brand, cpu_model_string, sizeof(g_cpu_info.brand));
            return;
        }
    } else if (str_equal(vendor, "CentaurHauls") || str_equal(vendor, "VIA VIA VIA ")) {
        if (family == 5) {
            if (model == 4) str_copy_n(cpu_model_string, "IDT WinChip C6", sizeof(cpu_model_string));
            else if (model == 8) str_copy_n(cpu_model_string, "IDT WinChip 2", sizeof(cpu_model_string));
            else if (model == 9) str_copy_n(cpu_model_string, "IDT WinChip 3", sizeof(cpu_model_string));
            else str_copy_n(cpu_model_string, "IDT WinChip", sizeof(cpu_model_string));
            str_copy_n(g_cpu_info.brand, cpu_model_string, sizeof(g_cpu_info.brand));
            return;
        } else if (family == 6) {
            if (model == 6 || model == 7) str_copy_n(cpu_model_string, "VIA Cyrix III / C3 (Samuel)", sizeof(cpu_model_string));
            else if (model == 8) str_copy_n(cpu_model_string, "VIA C3 (Ezra)", sizeof(cpu_model_string));
            else if (model == 9) str_copy_n(cpu_model_string, "VIA C3 (Nehemiah)", sizeof(cpu_model_string));
            else str_copy_n(cpu_model_string, "VIA C3 Processor", sizeof(cpu_model_string));
            str_copy_n(g_cpu_info.brand, cpu_model_string, sizeof(g_cpu_info.brand));
            return;
        }
    } else if (str_equal(vendor, "CyrixInstead")) {
        if (family == 5) {
            if (model == 4) str_copy_n(cpu_model_string, "Cyrix MediaGX", sizeof(cpu_model_string));
            else str_copy_n(cpu_model_string, "Cyrix 6x86", sizeof(cpu_model_string));
            str_copy_n(g_cpu_info.brand, cpu_model_string, sizeof(g_cpu_info.brand));
            return;
        } else if (family == 6) {
            str_copy_n(cpu_model_string, "Cyrix 6x86MX / MII", sizeof(cpu_model_string));
            str_copy_n(g_cpu_info.brand, cpu_model_string, sizeof(g_cpu_info.brand));
            return;
        }
    } else if (str_equal(vendor, "RiseRiseRise")) {
        str_copy_n(cpu_model_string, "Rise mP6", sizeof(cpu_model_string));
        str_copy_n(g_cpu_info.brand, cpu_model_string, sizeof(g_cpu_info.brand));
        return;
    } else if (str_equal(vendor, "TransmetaCPU")) {
        str_copy_n(cpu_model_string, "Transmeta Crusoe", sizeof(cpu_model_string));
        str_copy_n(g_cpu_info.brand, cpu_model_string, sizeof(g_cpu_info.brand));
        return;
    }

    str_copy_n(cpu_model_string, vendor, sizeof(cpu_model_string));
    str_copy_n(g_cpu_info.brand, vendor, sizeof(g_cpu_info.brand));
}

#include "keyboard.h"
#include "irq.h"
#include "port.h"
#include "mouse.h"

static int shift = 0;
static int ctrl = 0;
static int caps = 0;
static int alt = 0;
static int e0_flag = 0;

#define KEY_BUF_SIZE 64
static int key_buf[KEY_BUF_SIZE];
static volatile int key_head = 0, key_tail = 0;

static void push_key(int key) {
    int next = (key_head + 1) % KEY_BUF_SIZE;
    if (next != key_tail) {
        key_buf[key_head] = key;
        key_head = next;
    }
}

int keyboard_get_key(void) {
    unsigned int flags = irq_save_flags();
    if (key_head == key_tail) {
        irq_restore_flags(flags);
        return 0;
    }
    int tail = key_tail;
    int key = key_buf[tail];
    key_tail = (tail + 1) % KEY_BUF_SIZE;
    irq_restore_flags(flags);
    return key;
}

int keyboard_get_ctrl(void) {
    return ctrl;
}

/* Drain the 8042 output buffer into the PS/2 parsers.
 *
 * CSM firmware that provides "Legacy USB Support" synthesises PS/2 traffic
 * from the USB keyboard and mouse inside SMM. Its queue is shallow, so the
 * kernel has to keep consuming port 0x60 even while it is busy drawing the
 * boot animation and before IRQ1/IRQ12 are unmasked. If that queue fills up
 * the firmware stops servicing the USB devices, they suspend and both LEDs go
 * out even though nothing is electrically wrong.
 *
 * Safe to call both from an IRQ handler and from a plain polling loop. */
void ps2_input_poll(void) {
    unsigned int flags = irq_save_flags();
    int drained = 0;

    while ((inb(0x64) & 0x01) && drained++ < 32) {
        unsigned char status = inb(0x64);
        unsigned char data = inb(0x60);
        if (status & 0x20) mouse_handle_byte(data);
        else keyboard_handle_byte(data);
    }

    irq_restore_flags(flags);
}

// Scancode to ASCII tables
static unsigned char kbdus[128] =
{
    0,  27, '1', '2', '3', '4', '5', '6', '7', '8',
  '9', '0', '-', '=', '\b',
  '\t',
  'q', 'w', 'e', 'r',
  't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0,
  'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';',
 '\'', '`',   0,
 '\\', 'z', 'x', 'c', 'v', 'b', 'n',
  'm', ',', '.', '/',   0,
  '*',
    0,
  ' ',
    0,
    0,
    0,   0,   0,   0,   0,   0,   0,   0,
    0,
    0,
    0,
    0,
    0,
    0,
  '-',
    0,
    0,
    0,
  '+',
    0,
    0,
    0,
    0,
    0,
    0,   0,   0,
    0,
    0,
    0,
};

static unsigned char kbdus_shift[128] =
{
    0,  27, '!', '@', '#', '$', '%', '^', '&', '*',
  '(', ')', '_', '+', '\b',
  '\t',
  'Q', 'W', 'E', 'R',
  'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0,
  'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':',
 '\"', '~',   0,
 '|', 'Z', 'X', 'C', 'V', 'B', 'N',
  'M', '<', '>', '?',   0,
  '*',
    0,
  ' ',
    0,
    0,
    0,   0,   0,   0,   0,   0,   0,   0,
    0,
    0,
    0,
    0,
    0,
    0,
  '-',
    0,
    0,
    0,
  '+',
    0,
    0,
    0,
    0,
    0,
    0,   0,   0,
    0,
    0,
    0,
};

static int e1_skip = 0;

void keyboard_handle_byte(unsigned char scancode) {
    if (scancode == 0xE1) {
        e1_skip = 2; // Pause key sends E1 1D 45 ... skip next bytes
        return;
    }
    if (e1_skip > 0) {
        e1_skip--;
        return;
    }

    if (scancode == 0xE0) {
        e0_flag = 1;
        return;
    }
    
    int release = scancode & 0x80;
    scancode &= 0x7F;

    if (e0_flag) {
        // Extended keys
        if (scancode == 0x1D) {
            ctrl = !release; // Right Ctrl
        } else if (scancode == 0x38) {
            alt = !release;  // Right Alt
        } else if (scancode == 0x2A || scancode == 0x37) {
            // PrintScreen make sequence (E0 2A E0 37) - ignore fake shift
        } else if (!release) {
            switch(scancode) {
                case 0x48: push_key(200); break; // UP
                case 0x50: push_key(201); break; // DOWN
                case 0x4B: push_key(202); break; // LEFT
                case 0x4D: push_key(203); break; // RIGHT
                case 0x47: push_key(204); break; // HOME
                case 0x4F: push_key(205); break; // END
                case 0x49: push_key(206); break; // PGUP
                case 0x51: push_key(207); break; // PGDN
                case 0x53: push_key(208); break; // DEL
            }
        }
        e0_flag = 0;
        return;
    }

    // Standard keys (not E0 extended)
    if (scancode == 0x2A || scancode == 0x36) {
        shift = !release;
    } else if (scancode == 0x1D) {
        ctrl = !release;
    } else if (scancode == 0x38) {
        alt = !release;
    } else if (scancode == 0x3A && !release) {
        caps = !caps;
    } else if (!release) {
        if (scancode < 128) {
            char base = kbdus[scancode];
            char shifted = kbdus_shift[scancode];
            char c = 0;

            if (base >= 'a' && base <= 'z') {
                // Letter keys: CapsLock and Shift interact via XOR
                // Shift + CapsLock produces lowercase!
                int uppercase = shift ^ caps;
                c = uppercase ? (base - 32) : base;
            } else {
                // Non-letter keys: CapsLock does not affect symbols/digits
                c = shift ? shifted : base;
            }

            if (c) {
                push_key((unsigned char)c);
            }
        }
    }
}

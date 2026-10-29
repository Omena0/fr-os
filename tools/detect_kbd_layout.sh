#!/bin/sh
# detect_kbd_layout.sh — print "fi" or "us".
#
# The kernel's PS/2 scancode tables are selected at build time, because the
# guest has no way to ask the host what keyboard it is using. This prints the
# layout the host is configured with so make can pass a -D flag.
#
# Priority order matters: an X11/vconsole keymap is what the physical keys
# actually do. LANG says what language the user reads, which can easily be
# fi_FI.UTF-8 on a US keyboard, so it is only a fallback.

layout=""

# 1. X11 layout from localectl, if systemd is present.
if [ -z "$layout" ] && command -v localectl >/dev/null 2>&1; then
    status=$(localectl status 2>/dev/null)
    case "$status" in
        *"X11 Layout: fi"*|*"X11 Layout: fi,"*|*"X11 Layout: fi "*|*"X11 Layout: fi-fi"*)
            layout=fi ;;
        *"VC Keymap: fi"*)
            layout=fi ;;
    esac
fi

# 2. Debian/Ubuntu's keyboard defaults.
if [ -z "$layout" ] && [ -r /etc/default/keyboard ]; then
    if grep -Eq '^[[:space:]]*XKBLAYOUT="(fi|fi,|.*,fi")' /etc/default/keyboard 2>/dev/null; then
        layout=fi
    fi
fi

# 3. systemd vconsole settings.
if [ -z "$layout" ] && [ -r /etc/vconsole.conf ]; then
    if grep -Eq '^[[:space:]]*KEYMAP=(fi|"fi")' /etc/vconsole.conf 2>/dev/null; then
        layout=fi
    fi
fi

# 4. Last resort: the locale. fi_FI implies a Finnish keyboard often enough.
if [ -z "$layout" ]; then
    for var in LC_ALL LC_CTYPE LANG; do
        eval "value=\${$var}"
        case "$value" in
            fi*|*_FI.*|*_FI)
                layout=fi
                break
                ;;
        esac
    done
fi

# 5. Anything else is the US layout this kernel used to hard-code.
[ -n "$layout" ] || layout=us

printf '%s\n' "$layout"

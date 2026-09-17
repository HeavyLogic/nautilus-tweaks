#!/usr/bin/env bash

# Check for root privileges
if [[ $EUID -ne 0 ]]; then
   echo "(!) Please run the installation via: sudo make install"
   exit 1
fi

EXTENSIONS_DIR="/usr/lib/nautilus/extensions-4"
LOCALE_DIR="/usr/share/locale"
GETTEXT_PACKAGE="nautilus-tweaks"
REAL_USER="${SUDO_USER:-$USER}"

# 1. Quit Nautilus on behalf of the real user to unload old modules
if [[ -n "$REAL_USER" && "$REAL_USER" != "root" ]]; then
    REAL_UID=$(id -u "$REAL_USER" 2>/dev/null)
    if [[ -n "$REAL_UID" && -d "/run/user/$REAL_UID" ]]; then
        # Graceful quit via user's D-Bus session
        runuser -u "$REAL_USER" -- env \
            XDG_RUNTIME_DIR="/run/user/$REAL_UID" \
            DBUS_SESSION_BUS_ADDRESS="unix:path=/run/user/$REAL_UID/bus" \
            nautilus -q 2>/dev/null || true
    fi

    # Fallback to force-terminate if still lingering
    if command -v killall >/dev/null 2>&1; then
        killall -u "$REAL_USER" -q nautilus 2>/dev/null || true
    else
        pkill -u "$REAL_USER" -x nautilus 2>/dev/null || true
    fi
fi

# 2. Clean up old build artifacts and installed modules
rm -f libnautilus-tweaks-*.so
rm -f "$EXTENSIONS_DIR"/libnautilus-tweaks-*.so

# --------------------------------------------------------------------------- #
# PROJECT MODULES LIST                                                        #
# Format: "ID:source_file.c:Menu description"                                 #
# --------------------------------------------------------------------------- #
MODULES=(
    "actions:nautilus-tweaks-actions.c:Custom actions (Copy Paths, VS Code, Root)"
    "mount:nautilus-tweaks-mount.c:Server mounting (SFTP, FTP, Rclone)"
    "permissions:nautilus-tweaks-permissions.c:Permission management (chmod / chown)"
)

SELECTED=()
for ((i=0; i<${#MODULES[@]}; i++)); do
    SELECTED[i]=1
done

CURRENT=0
TOTAL=${#MODULES[@]}

trap 'tput cnorm; echo' EXIT
tput civis

draw_menu() {
    clear
    echo "========================================================"
    echo "             Nautilus Tweaks — Installer                "
    echo "========================================================"
    echo " [↑ / ↓] Navigate (or W/S, K/J)     [Space / 1-9] Toggle"
    echo " [Enter] Build & Install           [A] Select all  [N] Deselect all"
    echo "--------------------------------------------------------"
    echo ""

    for ((i=0; i<TOTAL; i++)); do
        IFS=":" read -r mod_id mod_src mod_desc <<< "${MODULES[i]}"
        num=$((i + 1))

        if [[ ${SELECTED[i]} -eq 1 ]]; then
            BOX="\e[32m[*]\e[0m"
        else
            BOX="\e[90m[ ]\e[0m"
        fi

        if [[ $i -eq $CURRENT ]]; then
            echo -e " \e[1;36m>\e[0m $num. $BOX \e[1;37m$mod_desc\e[0m \e[90m($mod_src)\e[0m"
        else
            echo -e "   $num. $BOX $mod_desc \e[90m($mod_src)\e[0m"
        fi
    done
    echo ""
    echo "--------------------------------------------------------"
}

# Input handling
while true; do
    draw_menu

    IFS= read -rsn1 KEY < /dev/tty || true

    if [[ "$KEY" == $'\x1b' ]]; then
        read -rsn2 -t 0.05 KEY2 < /dev/tty || true
        case "$KEY2" in
            "[A"|"OA") # Arrow Up
                if [ $CURRENT -gt 0 ]; then
                    CURRENT=$((CURRENT - 1))
                else
                    CURRENT=$((TOTAL - 1))
                fi
                ;;
            "[B"|"OB") # Arrow Down
                if [ $CURRENT -lt $((TOTAL - 1)) ]; then
                    CURRENT=$((CURRENT + 1))
                else
                    CURRENT=0
                fi
                ;;
        esac
    elif [[ "$KEY" == "w" || "$KEY" == "W" || "$KEY" == "k" || "$KEY" == "K" || "$KEY" == "ц" || "$KEY" == "Ц" || "$KEY" == "л" || "$KEY" == "Л" ]]; then
        if [ $CURRENT -gt 0 ]; then
            CURRENT=$((CURRENT - 1))
        else
            CURRENT=$((TOTAL - 1))
        fi
    elif [[ "$KEY" == "s" || "$KEY" == "S" || "$KEY" == "j" || "$KEY" == "J" || "$KEY" == "ы" || "$KEY" == "Ы" || "$KEY" == "о" || "$KEY" == "О" ]]; then
        if [ $CURRENT -lt $((TOTAL - 1)) ]; then
            CURRENT=$((CURRENT + 1))
        else
            CURRENT=0
        fi
    elif [[ "$KEY" =~ ^[1-9]$ ]]; then
        idx=$((KEY - 1))
        if [ $idx -lt $TOTAL ]; then
            CURRENT=$idx
            SELECTED[idx]=$(( 1 - SELECTED[idx] ))
        fi
    elif [[ "$KEY" == " " ]]; then
        SELECTED[CURRENT]=$(( 1 - SELECTED[CURRENT] ))
    elif [[ "$KEY" == "a" || "$KEY" == "A" || "$KEY" == "ф" || "$KEY" == "Ф" ]]; then
        for ((i=0; i<TOTAL; i++)); do SELECTED[i]=1; done
    elif [[ "$KEY" == "n" || "$KEY" == "N" || "$KEY" == "т" || "$KEY" == "Т" ]]; then
        for ((i=0; i<TOTAL; i++)); do SELECTED[i]=0; done
    elif [[ "$KEY" == "" ]]; then
        break
    fi
done

clear

# --------------------------------------------------------------------------- #
# 1. BUILD & INSTALL TRANSLATIONS                                             #
# --------------------------------------------------------------------------- #
if [ -d "po" ]; then
    if command -v msgfmt >/dev/null 2>&1; then
        echo "==> Compiling and installing translations..."
        for po_file in po/*.po; do
            [ -f "$po_file" ] || continue
            lang=$(basename "$po_file" .po)
            target_dir="$LOCALE_DIR/$lang/LC_MESSAGES"

            install -d "$target_dir"
            msgfmt "$po_file" -o "$target_dir/${GETTEXT_PACKAGE}.mo"
            echo -e "  [+] Translation installed: \e[1m$lang\e[0m -> $target_dir/${GETTEXT_PACKAGE}.mo"
        done
        echo ""
    else
        echo -e "(!) \e[33mWarning: 'msgfmt' not found. Install gettext to enable translations.\e[0m\n"
    fi
fi

# --------------------------------------------------------------------------- #
# 2. COMPILE & INSTALL EXTENSION MODULES                                      #
# --------------------------------------------------------------------------- #
echo "==> Compiling and installing modules..."
echo ""

CFLAGS="-Wall -Wno-unused-parameter -O2 -fPIC $(pkg-config --cflags libnautilus-extension-4 gtk4 gio-2.0)"
LDFLAGS="-shared $(pkg-config --libs libnautilus-extension-4 gtk4 gio-2.0)"

install -d "$EXTENSIONS_DIR"
INSTALLED_COUNT=0

for ((i=0; i<TOTAL; i++)); do
    if [[ ${SELECTED[i]} -eq 1 ]]; then
        IFS=":" read -r mod_id mod_src mod_desc <<< "${MODULES[i]}"
        OUT_LIB="libnautilus-tweaks-${mod_id}.so"

        echo -e "  [+] Compiling \e[1m$mod_src\e[0m -> $OUT_LIB"
        gcc $CFLAGS "$mod_src" tweaks-config.c tweaks-log.c -o "$OUT_LIB" $LDFLAGS

        install -m 755 "$OUT_LIB" "$EXTENSIONS_DIR"/
        echo -e "      \e[32mInstalled to $EXTENSIONS_DIR/$OUT_LIB\e[0m"
        INSTALLED_COUNT=$((INSTALLED_COUNT + 1))
    fi
done

echo ""
if [ $INSTALLED_COUNT -eq 0 ]; then
    echo "(!) All modules were disabled. System directory cleaned."
else
    echo -e "==> \e[32mDone! Successfully installed $INSTALLED_COUNT module(s).\e[0m"
fi
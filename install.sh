#!/usr/bin/env bash

# Проверяем запуск с правами root
if [[ $EUID -ne 0 ]]; then
   echo "(!) Пожалуйста, запустите установку через: sudo make install"
   exit 1
fi

EXTENSIONS_DIR="/usr/lib/nautilus/extensions-4"
REAL_USER="${SUDO_USER:-$USER}"

# 1. Закрываем Nautilus от имени реального пользователя
if [[ -n "$REAL_USER" && "$REAL_USER" != "root" ]]; then
    su - "$REAL_USER" -c "nautilus -q" 2>/dev/null || true
fi

# 2. Чистим старые файлы сборки и старые модули в системе
rm -f libnautilus-tweaks-*.so
rm -f "$EXTENSIONS_DIR"/libnautilus-tweaks-*.so

# --------------------------------------------------------------------------- #
# СПИСОК МОДУЛЕЙ ПРОЕКТА                                                      #
# Формат: "ID:исходный_файл.c:Описание для меню"                              #
# --------------------------------------------------------------------------- #
MODULES=(
    "actions:nautilus-tweaks-actions.c:Кастомные действия (Пути, VS Code, Root)"
    "mount:nautilus-tweaks-mount.c:Монтирование серверов (SFTP, FTP, Rclone)"
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
    echo "            Nautilus Tweaks — Установка                 "
    echo "========================================================"
    echo " [↑ / ↓] Навигация (или W/S, K/J)   [Пробел / Цифры 1-9] Вкл/Выкл"
    echo " [Enter] Собрать и установить      [A] Выбрать все  [N] Снять все"
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

        # Выравнивание: ровно 3 визуальных символа перед номером в обеих ветках
        if [[ $i -eq $CURRENT ]]; then
            echo -e " \e[1;36m>\e[0m $num. $BOX \e[1;37m$mod_desc\e[0m \e[90m($mod_src)\e[0m"
        else
            echo -e "   $num. $BOX $mod_desc \e[90m($mod_src)\e[0m"
        fi
    done
    echo ""
    echo "--------------------------------------------------------"
}

# Обработка ввода
while true; do
    draw_menu

    IFS= read -rsn1 KEY < /dev/tty || true

    if [[ "$KEY" == $'\x1b' ]]; then
        read -rsn2 -t 0.05 KEY2 < /dev/tty || true
        case "$KEY2" in
            "[A"|"OA") # Стрелка Вверх
                if [ $CURRENT -gt 0 ]; then
                    CURRENT=$((CURRENT - 1))
                else
                    CURRENT=$((TOTAL - 1))
                fi
                ;;
            "[B"|"OB") # Стрелка Вниз
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
echo "==> Компиляция и установка модулей..."
echo ""

CFLAGS="-Wall -Wno-unused-parameter -O2 -fPIC $(pkg-config --cflags libnautilus-extension-4 gtk4 gio-2.0)"
LDFLAGS="-shared $(pkg-config --libs libnautilus-extension-4 gtk4 gio-2.0)"

install -d "$EXTENSIONS_DIR"
INSTALLED_COUNT=0

for ((i=0; i<TOTAL; i++)); do
    if [[ ${SELECTED[i]} -eq 1 ]]; then
        IFS=":" read -r mod_id mod_src mod_desc <<< "${MODULES[i]}"
        OUT_LIB="libnautilus-tweaks-${mod_id}.so"

        echo -e "  [+] Компиляция \e[1m$mod_src\e[0m -> $OUT_LIB"
        gcc $CFLAGS "$mod_src" tweaks-config.c -o "$OUT_LIB" $LDFLAGS

        install -m 755 "$OUT_LIB" "$EXTENSIONS_DIR"/
        echo -e "      \e[32mУстановлен в $EXTENSIONS_DIR/$OUT_LIB\e[0m"
        INSTALLED_COUNT=$((INSTALLED_COUNT + 1))
    fi
done

echo ""
if [ $INSTALLED_COUNT -eq 0 ]; then
    echo "(!) Все модули были отключены. Системная папка очищена."
else
    echo -e "==> \e[32mГотово! Успешно установлено модулей: $INSTALLED_COUNT\e[0m"
fi
#!/bin/bash

OLD_REG="old_reg"
NEW_REG="new_reg"
CMD_FILE="extracted_commands.txt"
EXT_FILE="extracted_extensions.txt"

echo "--- [ Extracting Application Data from Wine Registry ] ---"

if [ ! -d "$OLD_REG" ] || [ ! -d "$NEW_REG" ]; then
    echo "[!] Directories 'old_reg' or 'new_reg' not found."
    exit 1
fi

> "$CMD_FILE"
> "$EXT_FILE"
TMP_CMDS="/tmp/wine_temp_cmds.txt"
TMP_APPS="/tmp/wine_temp_apps.txt"
> "$TMP_CMDS"
> "$TMP_APPS"

# 1. دمج التغييرات بشكل نظيف
diff -u "$OLD_REG/system.reg" "$NEW_REG/system.reg" 2>/dev/null | sed 's/\r$//' > /tmp/diff_full.txt
diff -u "$OLD_REG/user.reg" "$NEW_REG/user.reg" 2>/dev/null | sed 's/\r$//' >> /tmp/diff_full.txt

# 2. استخراج أسماء البرامج من Handlers و App Paths (بدون تحذيرات grep)
grep -i "ContextMenuHandlers" /tmp/diff_full.txt | grep "^+" | awk -F 'ContextMenuHandlers' '{print $2}' | tr -d '\\' | awk -F ']' '{print $1}' >> "$TMP_APPS"
grep -i "App Paths" /tmp/diff_full.txt | grep "^+" | awk -F 'App Paths' '{print $2}' | tr -d '\\' | sed 's/\.exe\].*//i' >> "$TMP_APPS"

# 3. استخراج الأوامر الصريحة (السر هنا: فلترة قبل الـ Loop لتسريع الأداء)
grep -E '^\+\[.*\\shell\\[^\\]+\\command\]|^\+@=' /tmp/diff_full.txt > /tmp/cmds_filtered.txt

ACTION_NAME=""
while read -r line; do
    if [[ "$line" =~ \\shell\\([^\\]+)\\command\] ]]; then
        ACTION_NAME="${BASH_REMATCH[1]}"
    elif [[ "$line" =~ ^\+@=\"(.*)\"$ ]] && [ -n "$ACTION_NAME" ]; then
        RAW_CMD="${BASH_REMATCH[1]}"
        if [[ "$RAW_CMD" =~ ([a-zA-Z0-9_-]+)\.exe ]]; then
            APP_N="${BASH_REMATCH[1]}"
        else
            APP_N="UnknownApp"
        fi
        echo "${APP_N}:::${ACTION_NAME}:::${RAW_CMD}" >> "$TMP_CMDS"
        echo "$APP_N" >> "$TMP_APPS"
        ACTION_NAME=""
    fi
done < /tmp/cmds_filtered.txt

# 4. تنظيف القائمة النهائية
APPS=$(sort -u "$TMP_APPS" | grep -v "^$" | grep -v "{" | grep -iv "32$")

if [ -z "$APPS" ]; then
    echo "[!] No changes or new apps detected."
    exit 0
fi

echo -e "[+] Detected Applications:\n$APPS\n"

# 5. توليد ملفات TXT
for APP_NAME in $APPS; do
    echo "===================================" >> "$CMD_FILE"
    echo "App: $APP_NAME" >> "$CMD_FILE"
    
    CMD_COUNT=$(grep -c "^${APP_NAME}:::" "$TMP_CMDS" 2>/dev/null || true)
    if [ "$CMD_COUNT" -gt 0 ]; then
        grep "^${APP_NAME}:::" "$TMP_CMDS" | while IFS=":::" read -r _ ACTION CMD; do
            echo "Action: $ACTION" >> "$CMD_FILE"
            echo "Command: $CMD" >> "$CMD_FILE"
        done
    else
        echo "Action: Open with $APP_NAME" >> "$CMD_FILE"
        echo "Command: (COM Object / No Explicit Command)" >> "$CMD_FILE"
    fi
    echo "" >> "$CMD_FILE"
    
    echo "App: $APP_NAME" >> "$EXT_FILE"
    
    EXTS=$(grep -i "Setup" /tmp/diff_full.txt | grep "^+" | grep -i "$APP_NAME" | awk -F 'Setup' '{print $2}' | tr -d '\\' | awk -F ']' '{print $1}' | sort -u)
    
    if [ -n "$EXTS" ]; then
        FORMATTED_EXTS=$(echo "$EXTS" | paste -sd ", " -)
        echo "Extensions: $FORMATTED_EXTS" >> "$EXT_FILE"
    else
        echo "Extensions: * (All Files)" >> "$EXT_FILE"
    fi
    echo "-----------------------------------" >> "$EXT_FILE"
done

echo -e "\n[+] Operation Successful."
echo "- Commands saved to: $CMD_FILE"
echo "- Extensions saved to: $EXT_FILE"
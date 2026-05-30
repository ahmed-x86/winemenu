#!/bin/bash

WINE_SOURCE="$HOME/.wine"
OLD_REG="old_reg"
NEW_REG="new_reg"

# السر هنا: إجبار Wine على كتابة السجلات في القرص الصلب قبل أي خطوة
echo "جاري إجبار Wine على حفظ السجلات في القرص (قد يستغرق بضع ثوانٍ)..."
wineserver -w

mkdir -p "$OLD_REG" "$NEW_REG"

if [ "$(ls -A "$NEW_REG")" ]; then
    echo "نقل النسخة الحالية إلى old_reg..."
    rm -rf "$OLD_REG"/*
    cp "$NEW_REG"/* "$OLD_REG/"
fi

echo "جلب النسخة الجديدة من $WINE_SOURCE إلى $NEW_REG..."
cp "$WINE_SOURCE/system.reg" "$WINE_SOURCE/user.reg" "$WINE_SOURCE/userdef.reg" "$NEW_REG/"

echo "اكتملت العملية بنجاح."
/*
 * Copyright (C) 2018 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

package com.android.settings.widget;

import android.content.res.ColorStateList;
import android.graphics.Color;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.DrawableWrapper;
import android.graphics.drawable.LayerDrawable;
import android.graphics.drawable.RippleDrawable;
import android.graphics.drawable.StateListDrawable;

final class PreferenceRippleHelper {
    private PreferenceRippleHelper() {}

    static void smooth(Drawable drawable) {
        if (drawable == null) return;
        if (drawable instanceof RippleDrawable) {
            ((RippleDrawable) drawable.mutate()).setEffectColor(
                    ColorStateList.valueOf(Color.TRANSPARENT));
        }
        if (drawable instanceof LayerDrawable) {
            LayerDrawable layers = (LayerDrawable) drawable;
            for (int i = 0; i < layers.getNumberOfLayers(); i++) smooth(layers.getDrawable(i));
        } else if (drawable instanceof StateListDrawable) {
            StateListDrawable states = (StateListDrawable) drawable;
            for (int i = 0; i < states.getStateCount(); i++) smooth(states.getStateDrawable(i));
        } else if (drawable instanceof DrawableWrapper) {
            smooth(((DrawableWrapper) drawable).getDrawable());
        }
    }
}

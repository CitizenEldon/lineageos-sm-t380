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

import androidx.preference.Preference;
import androidx.preference.PreferenceCategory;
import androidx.preference.PreferenceGroup;
import androidx.preference.PreferenceScreen;
import java.util.Collections;
import java.util.IdentityHashMap;
import java.util.Set;
import lineageos.preference.SelfRemovingListPreference;
import lineageos.preference.SelfRemovingPreference;
import lineageos.preference.SelfRemovingSwitchPreference;

/** Settles known availability before binding and tracks only categories hidden by this helper. */
final class PreferenceVisibilityHelper {
    private final PreferenceGroup mRoot;
    private final Set<Preference> mEmptyCategories =
            Collections.newSetFromMap(new IdentityHashMap<>());
    private boolean mUpdating;

    PreferenceVisibilityHelper(PreferenceGroup root) { mRoot = root; }

    PreferenceGroup prepare() {
        update();
        return mRoot;
    }

    void update() {
        if (mUpdating) return;
        mUpdating = true;
        try {
            updateGroup(mRoot);
        } finally {
            mUpdating = false;
        }
    }

    private void updateGroup(PreferenceGroup group) {
        boolean hasVisibleChild = false;
        for (int i = 0; i < group.getPreferenceCount(); i++) {
            Preference pref = group.getPreference(i);
            if ((pref instanceof SelfRemovingPreference
                    && !((SelfRemovingPreference) pref).isAvailable())
                    || (pref instanceof SelfRemovingListPreference
                    && !((SelfRemovingListPreference) pref).isAvailable())
                    || (pref instanceof SelfRemovingSwitchPreference
                    && !((SelfRemovingSwitchPreference) pref).isAvailable())) {
                pref.setVisible(false);
            }
            // A nested screen is a navigation destination, not an inline category.
            if (pref instanceof PreferenceGroup && !(pref instanceof PreferenceScreen)) {
                updateGroup((PreferenceGroup) pref);
            }
            hasVisibleChild |= pref.isVisible();
        }
        if (group instanceof PreferenceCategory) {
            if (!hasVisibleChild && group.isVisible()) {
                mEmptyCategories.add(group);
                group.setVisible(false);
            } else if (hasVisibleChild && mEmptyCategories.remove(group)) {
                group.setVisible(true);
            }
        }
    }
}

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

import static com.google.common.truth.Truth.assertThat;
import android.content.Context;
import android.content.res.ColorStateList;
import android.content.res.XmlResourceParser;
import org.xmlpull.v1.XmlPullParser;
import android.graphics.Color;
import android.graphics.drawable.ColorDrawable;
import android.graphics.drawable.InsetDrawable;
import android.graphics.drawable.RippleDrawable;
import android.graphics.drawable.StateListDrawable;
import android.view.View;
import androidx.preference.Preference;
import androidx.preference.PreferenceCategory;
import androidx.preference.PreferenceManager;
import androidx.preference.PreferenceScreen;
import androidx.recyclerview.widget.RecyclerView;
import com.android.settings.R;
import java.time.Duration;
import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.Robolectric;
import org.robolectric.RuntimeEnvironment;
import org.robolectric.shadows.ShadowSystemClock;
import lineageos.preference.SelfRemovingPreference;

@RunWith(RobolectricTestRunner.class)
public class SettingsPresentationTest {
    private Context context;
    private PreferenceScreen screen;
    @Before public void setUp() {
        context = RuntimeEnvironment.application;
        context.setTheme(R.style.Theme_Settings_Home);
        screen = new PreferenceManager(context).createPreferenceScreen(context);
    }
    private PreferenceCategory category() {
        PreferenceCategory category = new PreferenceCategory(context);
        screen.addPreference(category);
        return category;
    }
    @Test public void unavailableLineageRowAndEmptyCategory_areAbsentBeforeBinding() throws Exception {
        PreferenceCategory category = category();
        SelfRemovingPreference row;
        try (XmlResourceParser attributes = context.getResources().getXml(R.xml.power_usage_summary)) {
            while (attributes.next() != XmlPullParser.START_TAG) {}
            row = new SelfRemovingPreference(context, attributes);
        }
        row.setKey("unavailable_test");
        category.addPreference(row);
        row.setAvailable(false);
        new PreferenceVisibilityHelper(screen).prepare();
        assertThat(row.isVisible()).isFalse();
        assertThat(category.isVisible()).isFalse();
    }
    @Test public void nestedEmptyCategories_restoreWhenChildBecomesVisible() {
        PreferenceCategory outer = category();
        PreferenceCategory inner = new PreferenceCategory(context);
        outer.addPreference(inner);
        Preference row = new Preference(context);
        row.setVisible(false);
        inner.addPreference(row);
        PreferenceVisibilityHelper helper = new PreferenceVisibilityHelper(screen);
        helper.prepare();
        assertThat(outer.isVisible()).isFalse();
        row.setVisible(true);
        helper.update();
        assertThat(inner.isVisible()).isTrue();
        assertThat(outer.isVisible()).isTrue();
    }
    @Test public void explicitlyHiddenCategoryAndNestedScreen_preserveVisibility() {
        PreferenceCategory hidden = category();
        hidden.setVisible(false);
        hidden.addPreference(new Preference(context));
        PreferenceScreen destination = new PreferenceManager(context).createPreferenceScreen(context);
        screen.addPreference(destination);
        new PreferenceVisibilityHelper(screen).prepare();
        assertThat(hidden.isVisible()).isFalse();
        assertThat(destination.isVisible()).isTrue();
    }
    @Test public void adapter_revealsCategoryWhenAsynchronousChildAppears() {
        PreferenceCategory category = category();
        Preference row = new Preference(context);
        row.setVisible(false);
        category.addPreference(row);
        HighlightablePreferenceGroupAdapter adapter =
                new HighlightablePreferenceGroupAdapter(screen, null, false);
        assertThat(category.isVisible()).isFalse();
        row.setVisible(true);
        assertThat(category.isVisible()).isTrue();
    }
    @Test public void unresolvedFirstDraw_waitsWithoutBlocking_thenRestoresAnimator() {
        View view = new View(context);
        RecyclerView list = new RecyclerView(context) {
            @Override public boolean hasPendingAdapterUpdates() { return false; }
        };
        RecyclerView.ItemAnimator animator = list.getItemAnimator();
        boolean[] ready = {false};
        PreferenceInitialDraw gate = new PreferenceInitialDraw(view, list, () -> ready[0]);
        assertThat(gate.onPreDraw()).isFalse();
        assertThat(list.getItemAnimator()).isNull();
        ready[0] = true;
        assertThat(gate.onPreDraw()).isFalse();
        assertThat(gate.onPreDraw()).isTrue();
        assertThat(list.getItemAnimator()).isSameInstanceAs(animator);
    }
    @Test public void unavailableService_cannotHoldFirstDrawBeyondDeadline() {
        PreferenceInitialDraw gate = new PreferenceInitialDraw(new View(context),
                new RecyclerView(context), () -> false);
        assertThat(gate.onPreDraw()).isFalse();
        ShadowSystemClock.advanceBy(Duration.ofMillis(PreferenceInitialDraw.MAX_WAIT_MS));
        assertThat(gate.onPreDraw()).isTrue();
    }
    @Test public void slowInflation_doesNotConsumeInitialAvailabilityDeadline() {
        RecyclerView list = new RecyclerView(context) {
            @Override public boolean hasPendingAdapterUpdates() { return false; }
        };
        boolean[] ready = {false};
        PreferenceInitialDraw gate = new PreferenceInitialDraw(new View(context), list,
                () -> ready[0]);
        ShadowSystemClock.advanceBy(Duration.ofMillis(1000));
        assertThat(gate.onPreDraw()).isFalse();
        ready[0] = true;
        assertThat(gate.onPreDraw()).isFalse();
        assertThat(gate.onPreDraw()).isTrue();
    }
    @Test public void remoteAvailability_cannotOverrideUnavailableXmlConstraint() {
        SelfRemovingPreference row = new SelfRemovingPreference(context,
                Robolectric.buildAttributeSet()
                        .addAttribute(lineageos.preference.R.attr.requiresService,
                                "t380.test.absent.service")
                        .build());
        assertThat(row.isAvailable()).isFalse();
        row.setAvailable(true);
        assertThat(row.isAvailable()).isFalse();
        PreferenceCategory category = category();
        category.addPreference(row);
        new PreferenceVisibilityHelper(screen).prepare();
        assertThat(row.isVisible()).isFalse();
        assertThat(category.isVisible()).isFalse();
    }
    @Test public void detachingInitialView_restoresAnimatorAndCleansUp() {
        View view = new View(context);
        RecyclerView list = new RecyclerView(context);
        RecyclerView.ItemAnimator animator = list.getItemAnimator();
        PreferenceInitialDraw gate = new PreferenceInitialDraw(view, list, () -> false);
        gate.onViewDetachedFromWindow(view);
        assertThat(list.getItemAnimator()).isSameInstanceAs(animator);
    }
    @Test public void rippleInInactiveDrawableState_retainsWaveAndContentWithoutSparkles() {
        ColorDrawable content = new ColorDrawable(Color.BLUE);
        RippleDrawable ripple = new RippleDrawable(ColorStateList.valueOf(Color.RED), content, null);
        StateListDrawable states = new StateListDrawable();
        states.addState(new int[] {android.R.attr.state_pressed}, new InsetDrawable(ripple, 2));
        states.addState(new int[] {}, new ColorDrawable(Color.BLACK));
        PreferenceRippleHelper.smooth(states);
        assertThat(ripple.getEffectColor().getDefaultColor()).isEqualTo(Color.TRANSPARENT);
        assertThat(((ColorDrawable) ripple.getDrawable(0)).getColor()).isEqualTo(Color.BLUE);
        assertThat(content.getColor()).isEqualTo(Color.BLUE);
    }
}

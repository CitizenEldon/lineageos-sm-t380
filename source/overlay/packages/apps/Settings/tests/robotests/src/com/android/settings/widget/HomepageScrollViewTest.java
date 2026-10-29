/* Copyright (C) 2026 The Android Open Source Project
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy at http://www.apache.org/licenses/LICENSE-2.0
 * Unless required by law or agreed in writing, distributed software is provided
 * on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND.
 */
package com.android.settings.widget;

import static com.google.common.truth.Truth.assertThat;

import android.content.Context;
import android.view.View;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.TextView;

import androidx.recyclerview.widget.LinearLayoutManager;
import androidx.recyclerview.widget.RecyclerView;

import com.android.settings.R;

import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.RuntimeEnvironment;

@RunWith(RobolectricTestRunner.class)
public class HomepageScrollViewTest {
    private Context mContext;
    private HomepageScrollView mRoot;
    private FrameLayout mSuggestions;
    private RecyclerView mList;
    private int mCreated;

    @Before
    public void setUp() {
        mContext = RuntimeEnvironment.application;
        mContext.setTheme(R.style.Theme_Settings_Home);
        mRoot = new HomepageScrollView(mContext, null);
        LinearLayout content = new LinearLayout(mContext);
        content.setOrientation(LinearLayout.VERTICAL);
        mRoot.addView(content, new FrameLayout.LayoutParams(-1, -2));
        mSuggestions = new FrameLayout(mContext);
        mSuggestions.setId(R.id.suggestion_content);
        content.addView(mSuggestions, new LinearLayout.LayoutParams(-1, -2));
        mList = new RecyclerView(mContext);
        mList.setLayoutManager(new LinearLayoutManager(mContext));
        mList.setAdapter(new RecyclerView.Adapter<RecyclerView.ViewHolder>() {
            @Override
            public RecyclerView.ViewHolder onCreateViewHolder(ViewGroup parent, int type) {
                mCreated++;
                TextView row = new TextView(mContext);
                row.setLayoutParams(new RecyclerView.LayoutParams(-1, 50));
                return new RecyclerView.ViewHolder(row) {};
            }
            @Override
            public void onBindViewHolder(RecyclerView.ViewHolder holder, int position) {
                ((TextView) holder.itemView).setText("Preference " + position);
            }
            @Override
            public int getItemCount() { return 100; }
        });
        content.addView(mList, new LinearLayout.LayoutParams(-1, -2));
    }

    private void layout() {
        mRoot.measure(View.MeasureSpec.makeMeasureSpec(300, View.MeasureSpec.EXACTLY),
                View.MeasureSpec.makeMeasureSpec(500, View.MeasureSpec.EXACTLY));
        mRoot.layout(0, 0, 300, 500);
    }

    @Test
    public void emptyHeaders_listRecyclesAndCanReachOffscreenPreferences() {
        layout();
        assertThat(mCreated).isLessThan(25);
        assertThat(mList.canScrollVertically(1)).isTrue();
        assertThat(mRoot.getScrollCaptureHint()).isEqualTo(View.SCROLL_CAPTURE_HINT_AUTO);
        mList.scrollToPosition(90);
        layout();
        assertThat(((LinearLayoutManager) mList.getLayoutManager())
                .findLastVisibleItemPosition()).isAtLeast(90);
    }

    @Test
    public void visibleHeader_preservesOuterScrolling_thenReturnsToRecycling() {
        mSuggestions.addView(new TextView(mContext), new FrameLayout.LayoutParams(-1, 100));
        layout();
        assertThat(mRoot.canScrollVertically(1)).isTrue();
        mSuggestions.removeAllViews();
        layout();
        assertThat(mList.canScrollVertically(1)).isTrue();
        assertThat(mRoot.canScrollVertically(1)).isFalse();
    }
}
